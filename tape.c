/* tape.c: tape handling routines
   Copyright (c) 1999-2017 Philip Kendall, Darren Salt, Witold Filipczyk
   Copyright (c) 2015-2018 UB880D
   Copyright (c) 2016-2021 Fredrick Meunier
   Copyright (c) 2026 Alberto Garcia

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

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "libspectrum.h"

#include "debugger/debugger.h"
#include "event.h"
#include "fuse.h"
#include "infrastructure/startup_manager.h"
#include "loader.h"
#include "machine.h"
#include "memory_pages.h"
#include "phantom_typist.h"
#include "rzx.h"
#include "settings.h"
#include "sound.h"
#include "snapshot.h"
#include "tape.h"
#include "tape_internals.h"
#include "timer/timer.h"
#include "ui/ui.h"
#include "utils.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"

/* The current tape */
libspectrum_tape *tape;

/* Has the current tape been modified since it was last loaded/saved? */
int tape_modified;

/* Is the emulated tape deck playing? */
int tape_playing;

/* Do we have to stop the tape on the next edge? */
int tape_stop_pending = 0;

/* Was the tape playing started automatically? */
int tape_autoplay;

/* Is playback positioned at a pause reached by a tape trap? */
int trap_resume_pending;
/* DATA1 also describes mid-byte playback; require a known block boundary. */
int trap_pzx_ready;

/* Has the tape reached a point which requires an explicit user action? */
int tape_autoplay_blocked;

/* Is there a high input to the EAR socket? */
int tape_microphone;

/* Debugger integration */
static const char * const debugger_type_string = "tape";

static const char * const play_event_detail_string = "play",
  * const stop_event_detail_string = "stop";
static int play_event, stop_event = -1;

static const char * const microphone_variable_name = "microphone";

/* Spectrum events */
int tape_edge_event;
static int tape_mic_off_event;

static libspectrum_dword next_tape_edge_tstates;
/* Edge durations from libspectrum are in 3.5 MHz units. Pending events and
   saved partial intervals are in machine units. */
static libspectrum_dword tape_clock;
static libspectrum_qword tape_clock_remainder;

/* Function prototypes */

static void tape_stop_mic_off( libspectrum_dword last_tstates, int type,
                               void *user_data );

/* Function definitions */

static libspectrum_dword
get_microphone( void )
{
  return tape_microphone;
}

static void
next_edge( libspectrum_dword last_tstates, int type, void *user_data )
{
  tape_next_edge( last_tstates, 0 );
}

static int
tape_init( void *context )
{
  tape = libspectrum_tape_alloc();

  play_event = debugger_event_register( debugger_type_string,
					play_event_detail_string );
  stop_event = debugger_event_register( debugger_type_string,
					stop_event_detail_string );

  debugger_system_variable_register( debugger_type_string,
      microphone_variable_name, get_microphone, NULL );

  tape_edge_event = event_register( next_edge, "Tape edge" );
  tape_mic_off_event = event_register( tape_stop_mic_off, "Tape stop MIC off" );
  tape_record_init( tape );

  tape_modified = 0;

  /* Don't call tape_stop() here as the UI hasn't been initialised yet,
     so we can't update the statusbar */
  tape_playing = 0;
  tape_microphone = 0;
  tape_stop_pending = 0;
  tape_autoplay_blocked = 0;
  trap_resume_pending = 0;
  trap_pzx_ready = 0;

  next_tape_edge_tstates = 0;
  
  return 0;
}

static void
tape_end( void )
{
  if( tape_recording ) tape_record_stop();
  libspectrum_tape_free( tape );
  tape = NULL;
}

void
tape_register_startup( void )
{
  startup_manager_module dependencies[] = {
    STARTUP_MANAGER_MODULE_DEBUGGER,
    STARTUP_MANAGER_MODULE_EVENT,
    STARTUP_MANAGER_MODULE_SETUID,
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_TAPE, dependencies,
                            ARRAY_SIZE( dependencies ), tape_init, NULL,
                            tape_end );
}

int
tape_open( const char *filename, int autoload )
{
  utils_file file;
  int error;

  error = utils_read_file( filename, &file );
  if( error ) return error;

  error = tape_read_buffer( file.buffer, file.length, LIBSPECTRUM_ID_UNKNOWN,
			    filename, autoload );
  if( error ) { utils_close_file( &file ); return error; }

  utils_close_file( &file );

  return 0;
}

/* Use an already open tape file as the current tape */
int
tape_read_buffer( unsigned char *buffer, size_t length, libspectrum_id_t type,
		  const char *filename, int autoload )
{
  int error;

  if( libspectrum_tape_present( tape ) ) {
    error = tape_close(); if( error ) return error;
  }

  error = libspectrum_tape_read( tape, buffer, length, type, filename );
  if( error ) return error;

  tape_autoplay_blocked = 0;
  trap_resume_pending = 0;
  trap_pzx_ready = 0;
  tape_modified = 0;
  tape_clock_remainder = 0;
  next_tape_edge_tstates = 0;
  ui_tape_browser_update( UI_TAPE_BROWSER_NEW_TAPE, NULL );

  if( autoload ) {
    error = tape_autoload( machine_current->machine );
    if( error ) return error;
  }

  return 0;
}

/* Close the active tape file */
int
tape_close( void )
{
  int error;
  ui_confirm_save_t confirm;

  if( tape_recording ) {
    error = tape_record_stop();
    if( error ) return error;
  }

  /* If the tape has been modified, check if we want to do this */
  if( tape_modified ) {

    confirm =
      ui_confirm_save( "Tape has been modified.\nDo you want to save it?" );
    switch( confirm ) {

    case UI_CONFIRM_SAVE_SAVE:
      error = ui_tape_write(); if( error ) return error;
      break;

    case UI_CONFIRM_SAVE_DONTSAVE: break;
    case UI_CONFIRM_SAVE_CANCEL: return 1;

    }
  }

  /* Stop the tape if it's currently playing */
  if( tape_playing ) {
    error = tape_stop();
    if( error ) return error;
  }

  /* And then remove it from memory */
  error = libspectrum_tape_clear( tape );
  if( error ) return error;

  tape_modified = 0;
  trap_pzx_ready = 0;
  trap_resume_pending = 0;
  tape_clock_remainder = 0;
  next_tape_edge_tstates = 0;
  ui_tape_browser_update( UI_TAPE_BROWSER_NEW_TAPE, NULL );

  return 0;
}

/* Rewind to block 0, if any */
int
tape_rewind( void )
{
  if( !libspectrum_tape_present( tape ) ) return 0;

  return tape_select_block( 0 );
}

/* Select the nth block on the tape; 0 => 1st block */
int
tape_select_block( size_t n )
{
  int error;

  error = tape_select_block_no_update( n ); if( error ) return error;

  tape_autoplay_blocked = 0;
  ui_tape_browser_update( UI_TAPE_BROWSER_SELECT_BLOCK, NULL );

  return 0;
}

/* The same, but without updating the browser display */
int
tape_select_block_no_update( size_t n )
{
  trap_pzx_ready = 0;
  trap_resume_pending = 0;
  tape_clock_remainder = 0;
  next_tape_edge_tstates = 0;
  return libspectrum_tape_nth_block( tape, n );
}

/* Which block is current? */
int
tape_get_current_block( void )
{
  int n;
  libspectrum_error error;

  if( !libspectrum_tape_present( tape ) ) return -1;

  error = libspectrum_tape_position( &n, tape );
  if( error ) return -1;

  return n;
}

/* Write the current in-memory tape file out to disk */
int
tape_write( const char* filename )
{
  libspectrum_id_t type;
  libspectrum_class_t class;
  libspectrum_byte *buffer; size_t length;

  int error;

  /* Work out what sort of file we want from the filename; default to
     .tzx if we couldn't guess */
  error = libspectrum_identify_file_with_class( &type, &class, filename, NULL,
						0 );
  if( error ) return error;

  if( class != LIBSPECTRUM_CLASS_TAPE || type == LIBSPECTRUM_ID_UNKNOWN )
    type = LIBSPECTRUM_ID_TAPE_TZX;

  if( tape_recording ) {
    error = tape_record_stop();
    if( error ) return error;
  }
  length = 0;

  error = libspectrum_tape_write( &buffer, &length, tape, type );
  if( error != LIBSPECTRUM_ERROR_NONE ) return error;

  error = utils_write_file( filename, buffer, length );
  if( error ) { libspectrum_free( buffer ); return error; }

  tape_modified = 0;
  ui_tape_browser_update( UI_TAPE_BROWSER_MODIFIED, NULL );

  libspectrum_free( buffer );

  return 0;
}

int tape_can_autoload( void )
{
  return auto_load_is_enabled();
}

void
tape_update_microphone( const libspectrum_tape_edge *edge )
{
  tape_microphone = edge->level;
}

int
tape_play( int autoplay )
{
  if( !libspectrum_tape_present( tape ) ) return 1;
  if( autoplay && tape_autoplay_blocked ) return 0;
  if( !autoplay ) tape_autoplay_blocked = 0;
  
  /* A stopped partial interval is already converted. Rescale it only if
     the machine clock changed while playback was stopped. */
  if( tape_clock != machine_current->timings.processor_speed ) {
    if( tape_clock )
      next_tape_edge_tstates =
        (libspectrum_qword)next_tape_edge_tstates *
        machine_current->timings.processor_speed / tape_clock;
    tape_clock = machine_current->timings.processor_speed;
    tape_clock_remainder = 0;
  }

  /* Otherwise, start the tape going */
  tape_playing = 1;
  trap_pzx_ready = 0;
  tape_autoplay = autoplay && !trap_resume_pending;
  if( trap_resume_pending &&
      tape_trap_pzx_data( libspectrum_tape_current_block( tape ) ) )
    trap_resume_pending = 0;
  {
    libspectrum_tape_signal_level level;
    if( !libspectrum_tape_signal_level_get( &level, tape ) )
      tape_microphone = level;
  }
  tape_stop_pending = 0;

  event_remove_type( tape_mic_off_event );

  /* Update the status bar */
  ui_statusbar_update( UI_STATUSBAR_ITEM_TAPE, UI_STATUSBAR_STATE_ACTIVE );

  timer_start_fastloading();

  loader_tape_play();

  event_add( tstates + next_tape_edge_tstates, tape_edge_event );
  next_tape_edge_tstates = 0;

  /* Once the tape has started, the phantom typist has done its job so
     cancel any pending actions */
  phantom_typist_deactivate();

  debugger_event( play_event );

  return 0;
}

int
tape_do_play( int autoplay )
{
  if( !tape_playing ) {
    return tape_play( autoplay );
  } else {
    return 0;
  }
}

int
tape_toggle_play( int autoplay )
{
  if( tape_playing ) {
    return tape_stop();
  } else {
    return tape_play( autoplay );
  }
}

static void
save_next_tape_edge( gpointer data, gpointer user_data )
{
  event_t *ptr = data;

  if( ptr->type == tape_edge_event ) {
    next_tape_edge_tstates = ptr->tstates - tstates;
  }
}

static void
tape_save_next_edge( void )
{
  event_foreach( save_next_tape_edge, NULL );
}

int
tape_stop( void )
{
  if( tape_playing ) {

    tape_playing = 0;
    tape_stop_pending = 0;
    ui_statusbar_update( UI_STATUSBAR_ITEM_TAPE, UI_STATUSBAR_STATE_INACTIVE );
    loader_tape_stop();

    timer_stop_fastloading();

    tape_save_next_edge();
    event_remove_type( tape_edge_event );

    /* Turn off any lingering MIC level in a second (some loaders like Alkatraz
       seem to check the MIC level soon after loading is finished, presumably as
       a copy protection check */
    event_add( tstates + machine_current->timings.tstates_per_frame,
               tape_mic_off_event );
  }

  if( stop_event != -1 ) debugger_event( stop_event );

  return 0;
}

int
tape_is_playing( void )
{
  return tape_playing;
}

int
tape_present( void )
{
  return libspectrum_tape_present( tape );
}

int
tape_edge_requests_stop( const libspectrum_tape_edge *edge )
{
  int is_48k =
    !( libspectrum_machine_capabilities( machine_current->machine ) &
       LIBSPECTRUM_MACHINE_CAPABILITY_128_MEMORY );

  return ( edge->flags & LIBSPECTRUM_TAPE_FLAGS_STOP ) ||
         ( ( edge->flags & LIBSPECTRUM_TAPE_FLAGS_STOP48 ) && is_48k );
}

void
tape_handle_stop_request( const libspectrum_tape_edge *edge )
{
  if( !tape_edge_requests_stop( edge ) ) return;

  /* Defer stopping so this final edge, such as an embedded pause at the end
     of the tape, is still played. */
  tape_stop_pending = 1;

  /* At end-of-tape, STOP and BLOCK are returned together. Do not let loader
     detection immediately start the automatically rewound tape; inserting,
     selecting or manually playing a tape makes autoplay eligible again.
     Explicit stop blocks remain eligible because multiload tapes use them
     between levels. */
  if( ( edge->flags & LIBSPECTRUM_TAPE_FLAGS_STOP ) &&
      ( edge->flags & LIBSPECTRUM_TAPE_FLAGS_BLOCK ) )
    tape_autoplay_blocked = 1;
}

int
tape_should_stop_for_rom_block( libspectrum_tape_block *block )
{
  return tape_autoplay && settings_current.tape_traps &&
         !rzx_recording && !rzx_playback &&
         ( libspectrum_tape_block_type( block ) == LIBSPECTRUM_TAPE_BLOCK_ROM ||
           tape_trap_pzx_data( block ) );
}

static int
tape_handle_block_end( const libspectrum_tape_edge *edge )
{
  libspectrum_tape_block *block;

  /* At end-of-tape both STOP and BLOCK are set. Skip the trap check because
     it could undo the deferred stop and drop the final edge. */
  if( !( edge->flags & LIBSPECTRUM_TAPE_FLAGS_BLOCK ) || tape_stop_pending )
    return 0;

  trap_resume_pending = 0;
  ui_tape_browser_update( UI_TAPE_BROWSER_SELECT_BLOCK, NULL );

  /* Automatically played tapes stop before a new ROM block so the tape trap
     can load it without scheduling another edge. */
  block = libspectrum_tape_current_block( tape );
  if( tape_should_stop_for_rom_block( block ) ) {
    tape_stop();
    trap_pzx_ready = tape_trap_pzx_data( block );
    return 1;
  }
  return 0;
}

void
tape_schedule_edge( libspectrum_dword last_tstates,
                    const libspectrum_tape_edge *edge,
                    int from_acceleration )
{
  libspectrum_qword duration;

  /* Schedule relative to the last edge rather than the current time, since
     events are only processed between instructions. Convert once here so
     normal and accelerated playback share the same clock domain. RZX frame
     lengths affect event rebasing, not the duration of a tape pulse. */
  if( tape_clock != machine_current->timings.processor_speed ) {
    tape_clock = machine_current->timings.processor_speed;
    tape_clock_remainder = 0;
  }
  duration = (libspectrum_qword)edge->tstates * tape_clock +
             tape_clock_remainder;
  tape_clock_remainder = duration % 3500000;
  event_add( last_tstates + duration / 3500000, tape_edge_event );
  loader_set_acceleration_flags( edge->flags, from_acceleration );
}

void
tape_next_edge( libspectrum_dword last_tstates, int from_acceleration )
{
  libspectrum_tape_edge edge;
  libspectrum_error error;

  if( tape_stop_pending ) {
    tape_stop();
    return;
  }
  if( !tape_playing ) return;

  error = libspectrum_tape_get_next_edge( &edge, tape );
  if( error != LIBSPECTRUM_ERROR_NONE ) return;

  tape_update_microphone( &edge );
  sound_tape( last_tstates );
  tape_handle_stop_request( &edge );
  if( tape_handle_block_end( &edge ) ) return;
  tape_schedule_edge( last_tstates, &edge, from_acceleration );
}

static void
tape_stop_mic_off( libspectrum_dword last_tstates, int type, void *user_data )
{
  tape_microphone = 0;
}

/* Call a user-supplied function for every block in the current tape */
int
tape_foreach( void (*function)( libspectrum_tape_block *block,
				void *user_data),
	      void *user_data )
{
  libspectrum_tape_block *block;
  libspectrum_tape_iterator iterator;

  for( block = libspectrum_tape_iterator_init( &iterator, tape );
       block;
       block = libspectrum_tape_iterator_next( &iterator ) )
    function( block, user_data );

  return 0;
}
