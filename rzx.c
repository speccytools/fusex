/* rzx.c: .rzx files
   Copyright (c) 2002-2016 Philip Kendall
   Copyright (c) 2014-2018 Sergio Baldoví
   Copyright (c) 2015 Stuart Brady

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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
#include <windows.h>
#endif				/* #ifdef WIN32 */

#ifdef ENABLE_AUTOMATION
#include "automation/automation.h"
#endif
#include "debugger/debugger.h"
#include "event.h"
#include "fuse.h"
#include "infrastructure/startup_manager.h"
#include "machine.h"
#include "movie.h"
#include "peripherals/ula.h"
#include "rzx.h"
#include "peripherals/sound/general_sound.h"
#include "rzx_internal.h"
#include "settings.h"
#include "tape.h"
#include "snapshot.h"
#include "sound.h"
#include "timer/timer.h"
#include "ui/ui.h"
#include "utils.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"

#define RZX_SENTINEL_TIME ( ULA_CONTENTION_SIZE - 1000 )
#define RZX_SENTINEL_TIME_REDUCE 8000

/* The offset used to get the count of instructions from the R register;
   (instruction count) = R + rzx_instructions_offset */
int rzx_instructions_offset;

/* Are we currently playing back a .rzx file? */
int rzx_playback;
int rzx_spectaculator_plusd_compat;

int sentinel_warning;

/* Did RZX playback enable the legacy CMOS Z80 model? */
static int rzx_cmos_forced;

/* Did SPIN 0.5 record stale keyboard input during ROM tape saving? */
static int rzx_spin_tape_save_compat;
static int rzx_spin_tape_save_compat_active;
static int rzx_spin_tape_save_compat_warned;
static int rzx_spin_input_carry;
static int rzx_spin_input_carry_warned;
static libspectrum_byte rzx_spin_carried_input;

/* Did the recording emulator offer selectable CPU behaviour? */
static int rzx_spectaculator_selectable_cpu;

/* The number of instructions in the current .rzx playback frame */
size_t rzx_instruction_count;

/* The current RZX data */
libspectrum_rzx *rzx;

/* Debugger events */
static const char * const event_type_string = "rzx";
static const char * const end_event_detail_string = "end";

int end_event;

static int start_playback( libspectrum_rzx *from_rzx );
static int creator_is_spin_05( const libspectrum_creator *creator );
static int creator_has_broken_r7_restore( const libspectrum_creator *creator );
static int creator_is_spectaculator( const libspectrum_creator *creator );
static int
spectaculator_version_at_least_6_25( const libspectrum_creator *creator );
static int spin_tape_save_pc( void );
static int playback_frame( void );
static void rzx_sentinel( libspectrum_dword ts, int type,
			  void *user_data );

static int sentinel_event;

libspectrum_error
rzx_playback_byte( libspectrum_byte *value )
{
  if( rzx_spin_input_carry ) {
    *value = rzx_spin_carried_input;
    rzx_spin_input_carry = 0;
    return LIBSPECTRUM_ERROR_NONE;
  }
  return libspectrum_rzx_playback( rzx, value );
}

static int
rzx_init( void *context )
{
  rzx_playback = 0;
  rzx_recording_init();
  rzx_cmos_forced = 0;
  rzx_spin_tape_save_compat = 0;
  rzx_spin_tape_save_compat_active = 0;
  rzx_spin_tape_save_compat_warned = 0;
  rzx_spin_input_carry = 0;
  rzx_spin_input_carry_warned = 0;
  rzx_spectaculator_selectable_cpu = 0;
  rzx_spectaculator_plusd_compat = 0;

  sentinel_warning = 0;
  sentinel_event = event_register( rzx_sentinel, "RZX sentinel" );

  end_event = debugger_event_register( event_type_string, end_event_detail_string );

  return 0;
}

libspectrum_snap*
rzx_get_initial_snapshot( void )
{
  libspectrum_rzx_iterator it;

  for( it = libspectrum_rzx_iterator_begin( rzx );
       it;
       it = libspectrum_rzx_iterator_next( it ) ) {

    libspectrum_rzx_block_id id = libspectrum_rzx_iterator_get_type( it );

    switch( id ) {

    case LIBSPECTRUM_RZX_INPUT_BLOCK:
      /* If we get this then there can't have been an initial snap to start
         from */
      return NULL;
      
    case LIBSPECTRUM_RZX_SNAPSHOT_BLOCK:
      /* Got initial snap */
      return libspectrum_rzx_iterator_get_snap( it );
      
    default:
      continue;

    }

  }

  return NULL;
}

int rzx_start_playback( const char *filename, int check_snapshot )
{
  utils_file file;
  libspectrum_error libspec_error; int error;
  libspectrum_snap* snap;

  if( rzx_recording || tape_recording ) return 1;

  rzx = libspectrum_rzx_alloc();

  error = utils_read_file( filename, &file );
  if( error ) return error;

  libspec_error = libspectrum_rzx_read( rzx, file.buffer, file.length );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) {
#ifdef ENABLE_AUTOMATION
    automation_rzx_parse_error();
#endif
    utils_close_file( &file );
    return libspec_error;
  }

  utils_close_file( &file );

  snap = rzx_get_initial_snapshot();
  if( !snap && check_snapshot ) {
    /* We need to load an external snapshot. Could be skipped if the snapshot
       is preloaded from command line */
    error = utils_open_snap();
    if( error ) {
      ui_error( UI_ERROR_ERROR,
                "RZX recording contains no embedded snapshot and no "
                "external snapshot was loaded" );
#ifdef ENABLE_AUTOMATION
      automation_rzx_snapshot_error();
#endif
      return error;
    }
  }

  error = start_playback( rzx );
  if( error ) {
    libspectrum_rzx_free( rzx );
    return error;
  }

  return 0;
}

int
rzx_start_playback_from_buffer( const unsigned char *buffer, size_t length )
{
  return rzx_start_playback_from_buffer_with_snapshot_check( buffer, length, 1 );
}

int
rzx_start_playback_from_buffer_with_snapshot_check(
  const unsigned char *buffer, size_t length, int check_snapshot )
{
  int error;
  libspectrum_snap* snap;

  if( tape_recording ) return 1;
  if( rzx_recording ) return 0;

  rzx = libspectrum_rzx_alloc();

  error = libspectrum_rzx_read( rzx, buffer, length );
  if( error ) {
#ifdef ENABLE_AUTOMATION
    automation_rzx_parse_error();
#endif
    return error;
  }

  snap = rzx_get_initial_snapshot();
  if( !snap && check_snapshot ) {
    error = utils_open_snap();
    if( error ) {
      ui_error( UI_ERROR_ERROR,
                "RZX recording contains no embedded snapshot and no "
                "external snapshot was loaded" );
#ifdef ENABLE_AUTOMATION
      automation_rzx_snapshot_error();
#endif
      libspectrum_rzx_free( rzx );
      return error;
    }
  }

  error = start_playback( rzx );
  if( error ) {
    libspectrum_rzx_free( rzx );
    return error;
  }

  return 0;
}

static int
creator_is_spin_05( const libspectrum_creator *creator )
{
  const char *program;

  if( !creator ) return 0;

  program = libspectrum_creator_program( creator );
  if( !program || strncmp( program, "SPIN 0.5", 8 ) ) return 0;

  program += 8;
  if( strspn( program, " " ) != strlen( program ) ) return 0;

  return libspectrum_creator_major( creator ) == 0 &&
         libspectrum_creator_minor( creator ) == 5;
}

static int
creator_has_broken_r7_restore( const libspectrum_creator *creator )
{
  const char *program;

  if( !creator ) return 0;

  program = libspectrum_creator_program( creator );
  if( !program || strcmp( program, "Fuse" ) ) return 0;

  /* Fuse versions before 0.7 did not restore the high bit of R from
     snapshots, leaving it clear after the reset performed before loading. */
  return libspectrum_creator_major( creator ) < 7;
}

static int
creator_is_spectaculator( const libspectrum_creator *creator )
{
  const char *program;

  if( !creator ) return 0;

  program = libspectrum_creator_program( creator );
  return program && !strcmp( program, "Spectaculator" );
}

static int
spectaculator_version_at_least_6_25( const libspectrum_creator *creator )
{
  libspectrum_word major, minor;

  major = libspectrum_creator_major( creator );
  minor = libspectrum_creator_minor( creator );

  /* Spectaculator 6.25 introduced selectable CPU behaviour. Build 550 was
     the first public 6.25 release; major version 62 before that is 6.20. */
  return major > 62 || ( major == 62 && minor >= 550 );
}

static int
spin_tape_save_pc( void )
{
  return machine_current->machine == LIBSPECTRUM_MACHINE_48 &&
         PC >= 0x04c2 && PC <= 0x053f;
}

static int
start_playback( libspectrum_rzx *from_rzx )
{
  const libspectrum_creator *creator;
  int error, is_spectaculator;
  libspectrum_snap *snap;

  error = libspectrum_rzx_start_playback( from_rzx, 0, &snap );
  if( error ) {
#ifdef ENABLE_AUTOMATION
    automation_rzx_parse_error();
#endif
    return error;
  }

  if( snap ) {
    error = snapshot_copy_from( snap );
    if( error ) {
#ifdef ENABLE_AUTOMATION
      automation_rzx_snapshot_error();
#endif
      return error;
    }

    if( creator_has_broken_r7_restore(
          libspectrum_rzx_creator( from_rzx )
        ) )
      R7 = 0;
  }

  creator = libspectrum_rzx_creator( from_rzx );
  rzx_spectaculator_plusd_compat =
    creator_is_spectaculator( creator ) && snap &&
    libspectrum_snap_pc( snap ) == 0x0038 &&
    libspectrum_snap_plusd_active( snap ) &&
    !libspectrum_snap_plusd_paged( snap );
  rzx_spin_tape_save_compat = creator_is_spin_05( creator );
  rzx_spin_tape_save_compat_active = 0;
  rzx_spin_tape_save_compat_warned = 0;
  rzx_spin_input_carry = 0;
  rzx_spin_input_carry_warned = 0;

  is_spectaculator = creator_is_spectaculator( creator );
  rzx_spectaculator_selectable_cpu =
    is_spectaculator && spectaculator_version_at_least_6_25( creator );
  rzx_cmos_forced = is_spectaculator &&
    !rzx_spectaculator_selectable_cpu && !settings_current.z80_is_cmos;
  if( rzx_cmos_forced ) settings_current.z80_is_cmos = 1;
#ifdef ENABLE_AUTOMATION
  automation_rzx_started( snap != NULL );
#endif

  /* Forced RZX dispatch moves tstates between nominal event times rather
   * than CPU endpoints. Cancel cuts and retain nominal frame-only audio. */
  sound_suspend_subframe();

  /* End of frame will now be generated by the RZX code */
  event_remove_type( spectrum_frame_event );

  /* Add a sentinel event to prevent tstates overrun (bug #25) */
  event_add( RZX_SENTINEL_TIME, sentinel_event );

  sentinel_warning = 0;
  tstates = libspectrum_rzx_tstates( from_rzx );
  general_sound_reanchor();
  rzx_instruction_count = libspectrum_rzx_instructions( from_rzx );
  rzx_playback = 1;
  rzx_counter_reset();

  ui_menu_activate( UI_MENU_ITEM_RECORDING, 1 );
  ui_menu_activate( UI_MENU_ITEM_RECORDING_ROLLBACK, 0 );

  return 0;
}

void
rzx_spectaculator_cpu_hint( void )
{
  if( !rzx_spectaculator_selectable_cpu ) return;

  ui_error(
    UI_ERROR_INFO,
    "This RZX was recorded by a Spectaculator version with selectable CPU "
    "behaviour. Replaying it with the CMOS Z80 option %s may help",
    settings_current.z80_is_cmos ? "disabled" : "enabled"
  );
}

int rzx_stop_playback( int add_interrupt )
{
  libspectrum_error libspec_error;

  if( !rzx_playback ) return 0;
#ifdef ENABLE_AUTOMATION
  automation_rzx_aborted();
#endif

  /* Rejoin CPU-driven audio timing only at a fresh frame boundary. */
  sound_suspend_subframe();
  rzx_playback = 0;
  if( rzx_cmos_forced ) settings_current.z80_is_cmos = 0;
  rzx_cmos_forced = 0;
  rzx_spin_tape_save_compat = 0;
  rzx_spin_tape_save_compat_active = 0;
  rzx_spin_tape_save_compat_warned = 0;
  rzx_spin_input_carry = 0;
  rzx_spin_input_carry_warned = 0;
  rzx_spectaculator_selectable_cpu = 0;
  rzx_spectaculator_plusd_compat = 0;
  if( settings_current.movie_stop_after_rzx ) movie_stop();

  ui_menu_activate( UI_MENU_ITEM_RECORDING, 0 );
  ui_menu_activate( UI_MENU_ITEM_RECORDING_ROLLBACK, 0 );

  event_remove_type( sentinel_event );

  /* We've now finished with the RZX file, so add an end of frame
     event if we've been requested to do so; we don't if we just run
     out of frames, as this occurs just before a normal end of frame
     and everything works normally as rzx_playback is now zero again */
  if( add_interrupt ) {

    event_add( machine_current->timings.tstates_per_frame,
               spectrum_frame_event );

    /* We're no longer doing RZX playback, so tstates now be <= the
       normal frame count */
    if( tstates > machine_current->timings.tstates_per_frame )
      tstates = machine_current->timings.tstates_per_frame;

  } else {

    /* Ensure that tstates will be zero after it is reduced in
       spectrum_frame() */
    tstates = machine_current->timings.tstates_per_frame;

  }

  general_sound_reanchor();

  libspec_error = libspectrum_rzx_free( rzx );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) return libspec_error;

  debugger_event( end_event );

  return 0;
}  

int rzx_frame( void )
{
  if( rzx_recording ) return rzx_recording_frame();
  if( rzx_playback  ) return playback_frame();
  return 0;
}

static int playback_frame( void )
{
  int error, finished;
  size_t remaining;
  libspectrum_snap *snap;

  remaining = libspectrum_rzx_playback_inputs_remaining( rzx );

  if( rzx_spin_tape_save_compat && remaining &&
      ( rzx_spin_tape_save_compat_active || spin_tape_save_pc() ) ) {

    if( !rzx_spin_tape_save_compat_active ) {
      rzx_spin_tape_save_compat_active = 1;
      if( !rzx_spin_tape_save_compat_warned ) {
        ui_error_console( UI_ERROR_WARNING,
                          "Applying SPIN 0.5 RZX tape-save compatibility" );
        rzx_spin_tape_save_compat_warned = 1;
      }
    }

    error = libspectrum_rzx_playback_discard_inputs( rzx );
    if( error ) return error;
  }

  if( rzx_spin_tape_save_compat_active && !spin_tape_save_pc() )
    rzx_spin_tape_save_compat_active = 0;

  /* Some SPIN 0.5 files put an input executed after a fetch boundary at
     the end of the preceding frame. */
  if( rzx_spin_tape_save_compat && remaining == 1 && IFF1 &&
      !rzx_spin_tape_save_compat_active ) {
    error = libspectrum_rzx_playback( rzx, &rzx_spin_carried_input );
    if( error ) return error;
    rzx_spin_input_carry = 1;
    if( !rzx_spin_input_carry_warned ) {
      ui_error_console(
        UI_ERROR_WARNING,
        "Applying SPIN 0.5 RZX input-boundary compatibility" );
      rzx_spin_input_carry_warned = 1;
    }
  }

  error = libspectrum_rzx_playback_frame( rzx, &finished, &snap );
  if( error ) {
    rzx_spectaculator_cpu_hint();
#ifdef ENABLE_AUTOMATION
    automation_rzx_desynchronised();
#endif
    return rzx_stop_playback( 0 );
  }

  if( finished ) {
    ui_error( UI_ERROR_INFO, "Finished RZX playback" );
#ifdef ENABLE_AUTOMATION
    automation_rzx_completed();
#endif
    return rzx_stop_playback( 0 );
  }

  /* Move the RZX sentinel back out to 79000 tstates; the addition of
     the frame length is because everything is reduced by that in
     event_frame() */
  event_remove_type( sentinel_event );
  event_add( RZX_SENTINEL_TIME + tstates, sentinel_event );

  if( snap ) {
    error = snapshot_copy_from( snap );
    if( error ) {
#ifdef ENABLE_AUTOMATION
      automation_rzx_snapshot_error();
#endif
      return rzx_stop_playback( 0 );
    }

    if( creator_has_broken_r7_restore( libspectrum_rzx_creator( rzx ) ) )
      R7 = 0;
  }

  /* If we've got another frame to do, fetch the new instruction count and
     continue */
  rzx_instruction_count = libspectrum_rzx_instructions( rzx );
  rzx_counter_reset();

  return 0;
}

/* Reset the RZX counter; also, take this opportunity to normalise the
   R register */
int
rzx_counter_reset( void )
{
  R &= Z80_R_LOWER_BITS;	/* Normalise R to its lower seven bits */
  rzx_instructions_offset = -R; /* Gives us a zero count */

  return 0;
}

static void
rzx_end( void )
{
  if( rzx_recording ) rzx_stop_recording();
  if( rzx_playback  ) rzx_stop_playback( 0 );
}

void
rzx_register_startup( void )
{
  startup_manager_module dependencies[] = {
    STARTUP_MANAGER_MODULE_DEBUGGER,
    STARTUP_MANAGER_MODULE_EVENT,
    STARTUP_MANAGER_MODULE_MACHINE,
    STARTUP_MANAGER_MODULE_SETUID,
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_RZX, dependencies,
                            ARRAY_SIZE( dependencies ), rzx_init, NULL,
                            rzx_end );
}

static void
rzx_sentinel( libspectrum_dword ts GCC_UNUSED, int type GCC_UNUSED,
              void *user_data GCC_UNUSED )
{
  if( !sentinel_warning ) {
    /* This message could pop up very often. Limited to once per playback */
    ui_error_console( UI_ERROR_WARNING,
                      "RZX frame is longer than %u tstates",
                      RZX_SENTINEL_TIME );
    sentinel_warning = 1;
  }

  general_sound_frame( RZX_SENTINEL_TIME_REDUCE );

  tstates -= RZX_SENTINEL_TIME_REDUCE;
  z80.interrupts_enabled_at -= RZX_SENTINEL_TIME_REDUCE;

  /* Add another sentinel event in case this frame continues a lot more after
     this */
  event_add( RZX_SENTINEL_TIME, sentinel_event );
}
