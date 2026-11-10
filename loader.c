/* loader.c: loader detection
   Copyright (c) 2006-2018 Philip Kendall

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

   Author contact information:

   E-mail: philip-fuse@shadowmagic.org.uk

*/

#include "config.h"

#include "event.h"
#include "loader.h"
#include "loader_internals.h"
#include "memory_pages.h"
#include "rzx.h"
#include "settings.h"
#include "spectrum.h"
#include "tape.h"
#include "z80/z80.h"

static int successive_reads = 0;
static unsigned non_loader_frames = 0;
static libspectrum_signed_dword last_tstates_read = -100000;
static libspectrum_byte last_b_read = 0x00;
static int length_known1 = 0, length_known2 = 0;
static int length_long1 = 0, length_long2 = 0;

acceleration_mode_t acceleration_mode;
size_t acceleration_pc;

#define SOFTWARE_PROJECTS_SHORT_PULSE_ITERATIONS 10
#define SOFTWARE_PROJECTS_LONG_PULSE_ITERATIONS 20

/* Gremlin counts both halves of a double pulse in L. Each sampling loop takes
   29 T-states, giving approximately 24 or 48 iterations per tape pulse. */
#define GREMLIN_SHORT_PULSE_ITERATIONS 24
#define GREMLIN_LONG_PULSE_ITERATIONS 48

/* Movieload needs time to settle after the previous block before playback is
   restarted. Starting on the first recognised read corrupts the load. */
#define LOADER_DETECTION_READS 10
#define MOVIELOAD_DETECTION_READS 128
#define LOADER_STOP_NON_EAR_READS 10
/* Five seconds at the Spectrum's nominal frame rate. Moonlighter executes
   BASIC and scans the keyboard for over three seconds between ROM loads. */
#define LOADER_STOP_GRACE_FRAMES 250

void
loader_frame( libspectrum_dword frame_length )
{
  if( non_loader_frames < LOADER_STOP_GRACE_FRAMES ) non_loader_frames++;
  if( last_tstates_read > -100000 ) {
    last_tstates_read -= frame_length;
  }
}

void
loader_tape_play( void )
{
  non_loader_frames = 0;
  successive_reads = 0;
  acceleration_mode = ACCELERATION_MODE_NONE;
}

void
loader_tape_stop( void )
{
  successive_reads = 0;
  acceleration_mode = ACCELERATION_MODE_NONE;
}

static void
software_projects_accelerate( int long_pulse )
{
  int iterations = long_pulse ? SOFTWARE_PROJECTS_LONG_PULSE_ITERATIONS :
                                SOFTWARE_PROJECTS_SHORT_PULSE_ITERATIONS;

  z80.bc.b.h = z80.af_.b.h - iterations;

  /* Continue with the loader's edge-found path. */
  z80.pc.w = acceleration_pc + 9;
}

static void
gremlin_accelerate( int long_pulse )
{
  /* INC L has already executed once before loader_detect_loader(). */
  z80.hl.b.l += ( long_pulse ? GREMLIN_LONG_PULSE_ITERATIONS :
                                GREMLIN_SHORT_PULSE_ITERATIONS ) - 1;

  if( acceleration_mode == ACCELERATION_MODE_GREMLIN_RISING ) {
    /* Continue with the OUT and falling-edge loop. */
    z80.pc.w = acceleration_pc + 4;
  } else {
    /* The routine returns the combined rising/falling count in A. */
    z80.af.b.h = z80.hl.b.l;
    z80.pc.w = acceleration_pc + 4;
  }
}

static void
rom_loader_accelerate( int long_pulse )
{
  /* B is used to indicate the length of the pulses. */
  int set_b_high = long_pulse ^
                   ( acceleration_mode == ACCELERATION_MODE_DECREASING );
  z80.bc.b.h = set_b_high ? 0xfe : 0x00;

  /* Bit 5 of C is used to indicate the current microphone level. */
  z80.bc.b.l = ( z80.bc.b.l & ~0x20 ) |
               ( tape_microphone ? 0x00 : 0x20 );

  z80.af.b.l |= 0x01;

  /* Simulate the RET at the end of the edge-finding loop. */
  z80.pc.b.l = readbyte_internal( z80.sp.w ); z80.sp.w++;
  z80.pc.b.h = readbyte_internal( z80.sp.w ); z80.sp.w++;
}

void
accelerate_loader( int long_pulse )
{
  switch( acceleration_mode ) {
  case ACCELERATION_MODE_SOFTWARE_PROJECTS:
    /* The loader converts the number of loop iterations to a pulse length
       by subtracting B from A' and multiplying the result by four. */
    software_projects_accelerate( long_pulse );
    break;
  case ACCELERATION_MODE_GREMLIN_RISING:
  case ACCELERATION_MODE_GREMLIN_FALLING:
    gremlin_accelerate( long_pulse );
    break;
  case ACCELERATION_MODE_INCREASING:
  case ACCELERATION_MODE_DECREASING:
    rom_loader_accelerate( long_pulse );
    break;
  case ACCELERATION_MODE_NONE:
    break;
  }
}

static void
do_acceleration( void )
{
  if( length_known1 ) {
    accelerate_loader( length_long1 );
    event_remove_type( tape_edge_event );
    tape_next_edge( tstates, 1 );
    successive_reads = 0;
  }

  length_known1 = length_known2;
  length_long1 = length_long2;
}


static void
check_for_acceleration( void )
{
  /* If the IN occured at a different location to the one we're
     accelerating, stop acceleration */
  if( acceleration_mode && z80.pc.w != acceleration_pc )
    acceleration_mode = ACCELERATION_MODE_NONE;

  /* If we're not accelerating, check if this is a loader */
  if( !acceleration_mode ) {
    acceleration_mode = acceleration_detector_at_in( z80.pc.w );
    acceleration_pc = z80.pc.w;
  }

  if( acceleration_mode ) do_acceleration();
}

static int
loader_read_detected( libspectrum_word pc )
{
  return loader_loop_detector( pc ) ||
         movieload_loader_detector( pc ) || ula_read_uses_ear( pc );
}

static int
loader_counter_read_is_plausible( libspectrum_dword tstates_diff,
                                  libspectrum_byte b_diff )
{
  return tstates_diff <= 1000 &&
         ( b_diff == 1 || b_diff == 0 || b_diff == 0xff );
}

static void
loader_detect_while_playing( libspectrum_dword tstates_diff,
                             libspectrum_byte b_diff )
{
  if( loader_read_detected( z80.pc.w ) ||
      ( !ula_read_is_keyboard_scan( z80.pc.w ) &&
        loader_counter_read_is_plausible( tstates_diff, b_diff ) ) ) {
    successive_reads = 0;
    non_loader_frames = 0;
    return;
  }

  /* Keyboard scans can change B by -1 between their first two rows, which
     otherwise looks like a loader counter and resets successive_reads on
     every scan. Exclude their instruction pattern from the timing fallback.
     Keyboard-only activity also occurs between genuine loads: Moonlighter
     executes BASIC for several seconds before resuming its ROM loader.
     Give keyboard scans a bounded grace period after loading activity;
     other non-loader reads still use the normal consecutive-read limit. */
  if( successive_reads < LOADER_STOP_NON_EAR_READS ) successive_reads++;
  if( successive_reads >= LOADER_STOP_NON_EAR_READS &&
      ( !ula_read_is_keyboard_scan( z80.pc.w ) ||
        non_loader_frames >= LOADER_STOP_GRACE_FRAMES ) ) {
    tape_stop();
  }
}

static void
loader_start_after_reads( int reads )
{
  successive_reads++;
  if( successive_reads >= reads ) tape_do_play( 1 );
}

static void
loader_detect_while_stopped( libspectrum_dword tstates_diff,
                             libspectrum_byte b_diff )
{
  if( movieload_loader_detector( z80.pc.w ) && tstates_diff <= 500 ) {
    /* Unlike the other recognised loops, Movieload must sample the idle input
       for a while before playback starts. */
    loader_start_after_reads( MOVIELOAD_DETECTION_READS );
  } else if( loader_loop_detector( z80.pc.w ) ) {
    /* An instruction-level match also covers loaders which count outside B.
       Wait so playback starts between samples, like the timing heuristic. */
    loader_start_after_reads( LOADER_DETECTION_READS );
  } else if( ula_read_uses_ear( z80.pc.w ) && tstates_diff <= 500 &&
             ( b_diff == 1 || b_diff == 0xff ) ) {
    loader_start_after_reads( LOADER_DETECTION_READS );
  } else {
    successive_reads = 0;
  }
}

void
loader_detect_loader( void )
{
  libspectrum_dword tstates_diff = tstates - last_tstates_read;
  libspectrum_byte b_diff = z80.bc.b.h - last_b_read;

  last_tstates_read = tstates;
  last_b_read = z80.bc.b.h;

  if( !settings_current.detect_loader ) {
    successive_reads = 0;
  } else if( tape_is_playing() ) {
    loader_detect_while_playing( tstates_diff, b_diff );
  } else {
    loader_detect_while_stopped( tstates_diff, b_diff );
  }

  if( settings_current.accelerate_loader && tape_is_playing() &&
      !rzx_recording )
    check_for_acceleration();
}

void
loader_set_acceleration_flags( int flags, int from_acceleration )
{
  if( flags & LIBSPECTRUM_TAPE_FLAGS_LENGTH_SHORT ) {
    length_known2 = 1;
    length_long2 = 0;
  } else if( flags & LIBSPECTRUM_TAPE_FLAGS_LENGTH_LONG ) {
    length_known2 = 1;
    length_long2 = 1;
  } else {
    length_known2 = 0;
  }

  /* If this tape edge occurred due to normal timings rather than
     our tape acceleration, turn off acceleration for the next edge
     or we miss an edge. See [bugs:#387] for more details */
  if( !from_acceleration ) {
    length_known1 = 0;
  }
}
