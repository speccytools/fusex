/* rzx_recording.c: .rzx files recording functionality
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

#include "fuse.h"
#include "machine.h"
#include "movie.h"
#include "rzx.h"
#include "rzx_internal.h"
#include "settings.h"
#include "snapshot.h"
#include "tape.h"
#include "timer/timer.h"
#include "ui/ui.h"
#include "utils.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"

/* By how much may speed deviate from 100% in competition mode? */
static const float SPEED_TOLERANCE = 5;

/* How often will we create an autosave file? */
static const size_t AUTOSAVE_INTERVAL = 5 * 50;

size_t rzx_in_count;
libspectrum_byte *rzx_in_bytes;
size_t rzx_in_allocated;
int rzx_recording;
int rzx_competition_mode;

static size_t autosave_frame_count;
static int autosave_pending;
static char *rzx_filename;

libspectrum_rzx_dsa_key rzx_key = {
  "A9E3BD74E136A9ABD41E614383BB1B01EB24B2CD7B920ED6A62F786A879AC8B00F2FF318BF96F81654214B1A064889FF6D8078858ED00CF61D2047B2AAB7888949F35D166A2BBAAE23A331BD4728A736E76901D74B195B68C4A2BBFB9F005E3655BDE8256C279A626E00C7087A2D575F78D7DC5CA6E392A535FFE47A816BA503", /* p */
  "FE8D540EED2CAE1983690E2886259F8956FB5A19",	       /* q */
  "9680ABFFB98EF2021945ADDF86C21D6EE3F7C8777FB0A0220AB59E9DFA3A3338611B32CFD1F22F8F26547858754ED93BFBDD87DC13C09F42B42A36B2024467D98EB754DEB2847FCA7FC60C81A99CF95133847EA38AD9D037AFE9DD189E9F0EE47624848CEE840D7E3724A39681E71B97ECF777383DC52A48C0A2C93BADA93F4C", /* g */
  "46605F0514D56BC0B4207A350367A5038DBDD4DD62B7C997D26D0ADC5BE42D01F852C199E34553BCBCE5955FF80E3B402B55316606D7E39C0F500AE5EE41A7B7A4DCE78EC19072C21FCC7BA48DFDC830C17B72BCAA2B2D70D9DFC0AAD9B7E73F7AEB6241E54D55C33E41AB749CAAFBE7AB00F2D74C500E5F5DD63BD299C65778", /* y */
  "948744AA7A1D1BE9EE65150B0A95A678B4181F0E"	       /* x */
};

static int rzx_add_snap( libspectrum_rzx *to_rzx, int automatic );
static void start_recording( libspectrum_rzx *to_rzx, int competition_mode );
static void autosave_prune( void );
static void autosave_frame( void );
static void autosave_reset( void );

void
rzx_recording_init( void )
{
  rzx_recording = 0;
  autosave_pending = 0;
  rzx_in_bytes = NULL;
  rzx_in_allocated = 0;
}

static int
rzx_add_snap( libspectrum_rzx *to_rzx, int automatic )
{
  int error;
  libspectrum_snap *snap = libspectrum_snap_alloc();

  error = snapshot_copy_to( snap );
  if( error ) {
    libspectrum_snap_free( snap );
    return error;
  }

  error = libspectrum_rzx_add_snap( to_rzx, snap, automatic );
  if( error ) {
    libspectrum_snap_free( snap );
    return error;
  }

  return 0;
}

int rzx_start_recording( const char *filename, int embed_snapshot )
{
  int error;

  if( rzx_playback || tape_recording ) return 1;

  rzx = libspectrum_rzx_alloc();

  /* Store the filename */
  rzx_filename = utils_safe_strdup( filename );

  /* If we're embedding a snapshot, create it now */
  if( embed_snapshot ) {
    error = rzx_add_snap( rzx, 0 );

    if( error ) {
      libspectrum_free( rzx_filename );
      libspectrum_rzx_free( rzx );
      return error;
    }
  }

  start_recording( rzx, settings_current.competition_mode );

  return 0;
}

int rzx_stop_recording( void )
{
  libspectrum_byte *buffer; size_t length;
  libspectrum_error libspec_error; int error;

  if( !rzx_recording ) return 0;

  /* Stop recording data */
  rzx_recording = 0;
  autosave_pending = 0;
  if( settings_current.movie_stop_after_rzx ) movie_stop();

  /* Embed final snapshot */
  if( !rzx_competition_mode ) rzx_add_snap( rzx, 0 );

  libspectrum_free( rzx_in_bytes );
  rzx_in_bytes = NULL;
  rzx_in_allocated = 0;

  ui_menu_activate( UI_MENU_ITEM_RECORDING, 0 );
  ui_menu_activate( UI_MENU_ITEM_RECORDING_ROLLBACK, 0 );

  libspectrum_creator_set_competition_code(
    fuse_creator, settings_current.competition_code
  );

  length = 0;
  buffer = NULL;
  libspec_error = libspectrum_rzx_write(
    &buffer, &length, rzx, LIBSPECTRUM_ID_SNAPSHOT_SZX, fuse_creator,
    settings_current.rzx_compression, rzx_competition_mode ? &rzx_key : NULL
  );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) {
    libspectrum_free( rzx_filename );
    libspectrum_rzx_free( rzx );
    return libspec_error;
  }

  error = utils_write_file( rzx_filename, buffer, length );
  libspectrum_free( rzx_filename );
  if( error ) {
    libspectrum_free( buffer );
    libspectrum_rzx_free( rzx );
    return error;
  }

  libspectrum_free( buffer );

  libspec_error = libspectrum_rzx_free( rzx );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) return libspec_error;

  return 0;
}

static void
start_recording( libspectrum_rzx *to_rzx, int competition_mode )
{
  libspectrum_rzx_start_input( to_rzx, tstates );

  rzx_counter_reset();
  rzx_in_count = 0;
  autosave_frame_count = 0;
  autosave_pending = 0;

  rzx_recording = 1;

  ui_menu_activate( UI_MENU_ITEM_RECORDING, 1 );

  if( competition_mode ) {

    if( !libspectrum_gcrypt_version() )
      ui_error( UI_ERROR_WARNING,
                "gcrypt not available: recording will NOT be signed" );

    settings_current.emulation_speed = 100;
    rzx_competition_mode = 1;

  } else {

    ui_menu_activate( UI_MENU_ITEM_RECORDING_ROLLBACK, 1 );
    rzx_competition_mode = 0;

  }
}

int
rzx_continue_recording( const char *filename )
{
  utils_file file;
  libspectrum_error libspec_error; int error;
  libspectrum_snap* snap = NULL;
  libspectrum_rzx_iterator last_it = NULL;

  if( rzx_recording || rzx_playback ) return 1;

  if( tape_recording ) return 1;

  /* Store the filename */
  rzx_filename = utils_safe_strdup( filename );

  error = utils_read_file( filename, &file );
  if( error ) return error;

  rzx = libspectrum_rzx_alloc();

  libspec_error = libspectrum_rzx_read( rzx, file.buffer, file.length );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) {
    utils_close_file( &file );
    return libspec_error;
  }

  utils_close_file( &file );

  /* Get final snapshot */
  last_it = libspectrum_rzx_iterator_last( rzx );
  if( last_it ) snap = libspectrum_rzx_iterator_get_snap( last_it );

  if( snap ) {
    error = snapshot_copy_from( snap );
    if( error ) return error;
  } else {
    libspectrum_free( rzx_filename );
    libspectrum_rzx_free( rzx );
    return 1;
  }

  start_recording( rzx, 0 );

  return 0;
}

int
rzx_finalise_recording( const char *filename )
{
  libspectrum_byte *buffer; size_t length;
  libspectrum_error libspec_error; int error;
  utils_file file;

  if( rzx_recording || rzx_playback ) return 1;

  error = utils_read_file( filename, &file );
  if( error ) return error;

  rzx = libspectrum_rzx_alloc();

  libspec_error = libspectrum_rzx_read( rzx, file.buffer, file.length );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) {
    utils_close_file( &file );
    libspectrum_rzx_free( rzx );
    return libspec_error;
  }

  utils_close_file( &file );

  libspec_error = libspectrum_rzx_finalise( rzx );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) {
    libspectrum_rzx_free( rzx );
    return libspec_error;
  }

  /* Write the file */
  length = 0;
  buffer = NULL;
  libspec_error = libspectrum_rzx_write(
    &buffer, &length, rzx, LIBSPECTRUM_ID_SNAPSHOT_SZX, fuse_creator,
    settings_current.rzx_compression, rzx_competition_mode ? &rzx_key : NULL
  );
  if( libspec_error != LIBSPECTRUM_ERROR_NONE ) {
    libspectrum_rzx_free( rzx );
    return libspec_error;
  }

  error = utils_write_file( filename, buffer, length );
  if( error ) {
    libspectrum_free( buffer );
    libspectrum_rzx_free( rzx );
    return error;
  }

  libspectrum_free( buffer );
  libspectrum_rzx_free( rzx );

  return 0;
}

typedef struct prune_info_t {
  libspectrum_rzx_iterator it;
  size_t frames;
} prune_info_t;

static void
autosave_prune( void )
{
  GArray *autosaves = g_array_new( FALSE, FALSE, sizeof( prune_info_t ) );
  libspectrum_rzx_iterator it;
  size_t i, frames = 0;

  for( it = libspectrum_rzx_iterator_begin( rzx );
       it;
       it = libspectrum_rzx_iterator_next( it ) ) {

    libspectrum_rzx_block_id id = libspectrum_rzx_iterator_get_type( it );

    switch( id ) {

    case LIBSPECTRUM_RZX_INPUT_BLOCK:
      frames += libspectrum_rzx_iterator_get_frames( it ); break;

    case LIBSPECTRUM_RZX_SNAPSHOT_BLOCK:
      if( libspectrum_rzx_iterator_snap_is_automatic( it ) ) {
        prune_info_t info = { it, frames };
	g_array_append_val( autosaves, info );
      }
      break;

    default:
      break;
    }
  }

  /* Convert 'time from start' into 'time before now' */
  for( i = 0; i < autosaves->len; i++ ) {
    prune_info_t *info = &( g_array_index( autosaves, prune_info_t, i ) );
    info->frames = frames - info->frames;
  }

  for( i = autosaves->len - 1; i > 0; i-- ) {
    prune_info_t save1 = g_array_index( autosaves, prune_info_t, i ),
      save2 = g_array_index( autosaves, prune_info_t, i - 1 );

    if( ( save1.frames == 15 * 50 ||
          save1.frames == 60 * 50 ||
	  save1.frames == 300 * 50   ) &&
	save2.frames < 2 * save1.frames
      )
      /* FIXME: could possibly merge adjacent IRBs here */
      libspectrum_rzx_iterator_delete( rzx, save1.it );
  }

  g_array_free( autosaves, TRUE );
}

static void
autosave_frame( void )
{
  if( ++autosave_frame_count % AUTOSAVE_INTERVAL ) return;

  autosave_pending = 1;
}

int
rzx_frame_interrupt_complete( void )
{
  int error;

  if( !rzx_recording || !autosave_pending ) return 0;

  /* EI can postpone this frame's interrupt until after the next
     instruction. Leave the autosave pending for that interrupt callback. */
  if( IFF1 && tstates == z80.interrupts_enabled_at ) return 0;

  autosave_pending = 0;

  error = rzx_add_snap( rzx, 1 );
  if( error ) {
    rzx_stop_recording();
    return error;
  }

  libspectrum_rzx_start_input( rzx, tstates );
  autosave_prune();

  return 0;
}

static void
autosave_reset( void )
{
  libspectrum_rzx_iterator it;
  size_t frames = 0;

  for( it = libspectrum_rzx_iterator_begin( rzx );
       it;
       it = libspectrum_rzx_iterator_next( it ) ) {

    libspectrum_rzx_block_id id = libspectrum_rzx_iterator_get_type( it );

    switch( id ) {

    case LIBSPECTRUM_RZX_INPUT_BLOCK:
      frames += libspectrum_rzx_iterator_get_frames( it );
      break;

    case LIBSPECTRUM_RZX_SNAPSHOT_BLOCK:
      if( libspectrum_rzx_iterator_snap_is_automatic( it ) ) {
        frames = 0;
      }
      break;

    default:
      break;
    }
  }

  /* Reset the frame count. Allow to prune previous points after rolling back */
  autosave_frame_count = frames % AUTOSAVE_INTERVAL;
}

int
rzx_recording_frame( void )
{
  libspectrum_error error;

  error = libspectrum_rzx_store_frame( rzx, R + rzx_instructions_offset,
				       rzx_in_count, rzx_in_bytes );
  if( error ) {
    rzx_stop_recording();
    return error;
  }

  /* Reset the instruction counter */
  rzx_in_count = 0; rzx_counter_reset();

  /* If we're in competition mode, check we're running at close to 100%
     speed */
  if( rzx_competition_mode &&
      fabs( current_speed - 100.0 ) > SPEED_TOLERANCE ) {

    rzx_stop_recording();

    ui_error(
      UI_ERROR_INFO,
      "emulator speed is %d%%: stopping competition mode RZX recording",
      (int)( current_speed )
    );

  }

  if( !rzx_competition_mode && settings_current.rzx_autosaves )
    autosave_frame();

  return 0;
}

int rzx_store_byte( libspectrum_byte value )
{
  /* Get more space if we need it; allocate twice as much as we currently
     have, with a minimum of 50 */
  if( rzx_in_count == rzx_in_allocated ) {

    libspectrum_byte *ptr; size_t new_allocated;

    new_allocated = rzx_in_allocated >= 25 ? 2 * rzx_in_allocated : 50;
    ptr = libspectrum_renew( libspectrum_byte, rzx_in_bytes, new_allocated );

    rzx_in_bytes = ptr;
    rzx_in_allocated = new_allocated;
  }

  rzx_in_bytes[ rzx_in_count++ ] = value;

  return 0;
}

GSList*
rzx_get_rollback_list( libspectrum_rzx *from_rzx )
{
  libspectrum_rzx_iterator it;
  GSList *rollback_points;
  size_t frames;

  it = libspectrum_rzx_iterator_begin( from_rzx );
  rollback_points = NULL;
  frames = 0;

  while( it ) {
    libspectrum_rzx_block_id id = libspectrum_rzx_iterator_get_type( it );

    switch( id ) {

    case LIBSPECTRUM_RZX_INPUT_BLOCK:
      frames += libspectrum_rzx_iterator_get_frames( it ); break;

    case LIBSPECTRUM_RZX_SNAPSHOT_BLOCK:
      rollback_points = g_slist_append( rollback_points,
					GINT_TO_POINTER( frames ) );
      break;

    default:
      break;
    }

    it = libspectrum_rzx_iterator_next( it );
  }

  /* Add the final IRB in, if any */
  if( frames )
    rollback_points = g_slist_append( rollback_points,
				      GINT_TO_POINTER( frames ) );

  return rollback_points;
}

int
rzx_start_after_rollback( libspectrum_snap *snap )
{
  int error;

  autosave_pending = 0;

  error = snapshot_copy_from( snap );
  if( error ) return error;

  libspectrum_rzx_start_input( rzx, tstates );

  error = rzx_counter_reset();
  if( error ) return error;

  if( settings_current.rzx_autosaves )
    autosave_reset();

  return 0;
}

int
rzx_rollback( void )
{
  libspectrum_snap *snap;
  int error;

  error = libspectrum_rzx_rollback( rzx, &snap );
  if( error ) return error;

  error = rzx_start_after_rollback( snap );
  if( error ) return error;

  return 0;
}

int
rzx_rollback_to( void )
{
  GSList *rollback_points;
  libspectrum_snap *snap;
  int which, error;

  rollback_points = rzx_get_rollback_list( rzx );

  which = ui_get_rollback_point( rollback_points );

  if( which == -1 ) return 1;

  error = libspectrum_rzx_rollback_to( rzx, &snap, which );
  if( error ) return error;

  error = rzx_start_after_rollback( snap );
  if( error ) return error;

  return 0;
}

