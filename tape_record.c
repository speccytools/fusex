/* tape_record.c: edge-based tape recording
   Copyright (c) 1999-2017 Philip Kendall, Darren Salt, Witold Filipczyk
   Copyright (c) 2015-2018 UB880D
   Copyright (c) 2016-2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "config.h"
#include "libspectrum.h"
#include "fuse.h"
#include "machine.h"
#include "peripherals/ula.h"
#include "rzx.h"
#include "tape.h"
#include "tape_internals.h"
#include "ui/ui.h"

#define RECORD_MAX_DURATION 0x7fffffff

int tape_recording = 0;
static libspectrum_tape *recording_tape;
static libspectrum_qword frame_base, last_time, remainder;
static libspectrum_dword clock_rate;
static int initial_level, last_level;
static libspectrum_qword *durations;
static size_t used, capacity;

void
tape_record_set_tape( libspectrum_tape *current_tape )
{
  recording_tape = current_tape;
}

void
tape_record_init( libspectrum_tape *current_tape )
{
  tape_record_set_tape( current_tape );
}

void
tape_record_start( void )
{
  if( tape_recording || !recording_tape ) return;
  if( rzx_playback || rzx_recording ) {
    ui_error( UI_ERROR_ERROR, "Tape recording is unavailable during RZX" );
    return;
  }
  frame_base = remainder = 0;
  last_time = tstates;
  clock_rate = machine_current->timings.processor_speed;
  initial_level = last_level = !!ula_tape_level();
  used = 0;
  capacity = 1024;
  durations = libspectrum_new( libspectrum_qword, capacity );
  tape_recording = 1;
  ui_menu_activate( UI_MENU_ITEM_TAPE_RECORDING, 1 );
}

/* Called with the actual amount removed from the CPU/event timestamps.
   Tape recording is user-driven and unavailable during RZX. */
void
tape_record_frame( libspectrum_dword frame_length )
{
  if( tape_recording ) frame_base += frame_length;
}

static void
record_interval( void )
{
  libspectrum_qword now = frame_base + tstates;
  libspectrum_qword elapsed = now - last_time;
  /* Divide before multiplying so even very long unchanged levels do not
     overflow the intermediate conversion. */
  libspectrum_qword scaled = ( elapsed % clock_rate ) * 3500000 + remainder;
  if( used == capacity ) {
    capacity *= 2;
    durations = libspectrum_renew( libspectrum_qword, durations, capacity );
  }
  durations[used++] = ( elapsed / clock_rate ) * 3500000 + scaled / clock_rate;
  remainder = scaled % clock_rate;
  last_time = now;
}

void
tape_record_edge( int level )
{
  level = !!level;
  if( !tape_recording || level == last_level ) return;
  record_interval();
  last_level = level;
}

static int
append_block( libspectrum_tape_block *block )
{
  libspectrum_error error = libspectrum_tape_append_block( recording_tape, block );
  if( error ) {
    libspectrum_tape_block_free( block );
  } else {
    tape_modified = 1;
    ui_tape_browser_update( UI_TAPE_BROWSER_NEW_BLOCK, block );
  }
  return error;
}

static int
append_hold( libspectrum_dword duration, int level )
{
  libspectrum_tape_block *block =
    libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PAUSE );
  libspectrum_tape_block_set_pause_tstates( block, duration );
  libspectrum_tape_block_set_level( block, level );
  return append_block( block );
}

/* PULS always starts low. A leading zero pulse selects high. Each chunk
   starts with a defined polarity, independent of preceding blocks. */
static int
append_sequence( size_t first, size_t end, int level )
{
  size_t i, count = 0;
  libspectrum_dword *lengths = libspectrum_new( libspectrum_dword, end - first + 1 );
  size_t *repeats = libspectrum_new( size_t, end - first + 1 );
  libspectrum_tape_block *block;
  if( level ) { lengths[count] = 0; repeats[count++] = 1; }
  for( i = first; i < end; i++ ) {
    libspectrum_dword duration = durations[i];
    if( count && lengths[count - 1] == duration ) {
      repeats[count - 1]++;
    } else {
      lengths[count] = duration;
      repeats[count++] = 1;
    }
  }
  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PULSE_SEQUENCE );
  libspectrum_tape_block_set_count( block, count );
  libspectrum_tape_block_set_pulse_lengths( block, lengths );
  libspectrum_tape_block_set_pulse_repeats( block, repeats );
  return append_block( block );
}

int
tape_record_stop( void )
{
  size_t i, first = 0;
  int level = initial_level, sequence_level = initial_level;
  int error = 0;
  if( !tape_recording ) return 0;
  record_interval();
  /* Completed intervals are pulses. Very long holds use PAUS chunks,
     which preserve the level rather than introducing extra edges. */
  for( i = 0; i + 1 < used && !error; i++ ) {
    if( durations[i] > RECORD_MAX_DURATION ) {
      if( i > first ) error = append_sequence( first, i, sequence_level );
      while( durations[i] > RECORD_MAX_DURATION && !error ) {
        error = append_hold( RECORD_MAX_DURATION, level );
        durations[i] -= RECORD_MAX_DURATION;
      }
      first = i;
      sequence_level = level;
    }
    level = !level;
  }
  if( !error && i > first ) error = append_sequence( first, i, sequence_level );
  /* Stopping recording is not an output edge. Preserve the final level. */
  if( !error ) {
    libspectrum_qword tail = durations[used - 1];
    while( tail && !error ) {
      libspectrum_dword chunk = tail > RECORD_MAX_DURATION ? RECORD_MAX_DURATION : tail;
      error = append_hold( chunk, last_level );
      tail -= chunk;
    }
  }
  libspectrum_free( durations );
  durations = NULL;
  used = capacity = 0;
  tape_recording = 0;
  ui_menu_activate( UI_MENU_ITEM_TAPE_RECORDING, 0 );
  return error;
}
