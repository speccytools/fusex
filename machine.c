/* machine.c: Routines for handling the various machine types
   Copyright (c) 1999-2016 Philip Kendall

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

#include <stdlib.h>
#include <string.h>

#include "event.h"
#include "fuse.h"
#include "infrastructure/startup_manager.h"
#include "machine.h"
#include "peripherals/sound/general_sound.h"
#include "machines/machines.h"
#include "machines/scorpion.h"
#include "machines/spec128.h"
#include "machines/spec48.h"
#include "machines/specplus3.h"
#include "machines/tc2068.h"
#include "memory_pages.h"
#include "module.h"
#include "movie.h"
#include "peripherals/ula.h"
#include "peripherals/dck.h"
#include "pokefinder/pokemem.h"
#include "rzx.h"
#include "debugger/gdbserver.h"
#include "settings.h"
#include "snapshot.h"
#include "sound.h"
#include "tape.h"
#include "timer/timer.h"
#include "ui/ui.h"
#include "ui/uidisplay.h"
#include "utils.h"

fuse_machine_info **machine_types = NULL; /* Array of available machines */
int machine_count = 0;

fuse_machine_info *machine_current = NULL; /* The currently selected machine */
static int machine_location;	/* Where is the current machine in
				   machine_types[...]? */

typedef struct snapshot_rom_bank {
  memory_page *map;
  int page_num;
  memory_rom_bank bank;
} snapshot_rom_bank;

static GSList *snapshot_rom_banks;

static int machine_add_machine( int (*init_function)(fuse_machine_info *machine) );
void machine_clear_snapshot_rom_banks( void );
static int machine_select_machine( fuse_machine_info *machine, libspectrum_snap *snap );
static void machine_set_const_timings( fuse_machine_info *machine );
static void machine_set_variable_timings( fuse_machine_info *machine );

void SetEmulationHz( float hz );

static int
machine_init_machines( void *context )
{
  int error;

  error = machine_add_machine( spec16_init    );
  if (error ) return error;
  error = machine_add_machine( spec48_init    );
  if (error ) return error;
  error = machine_add_machine( spec48_ntsc_init );
  if (error ) return error;
  error = machine_add_machine( spec128_init   );
  if (error ) return error;
  error = machine_add_machine( specplus2_init );
  if (error ) return error;
  error = machine_add_machine( specplus2a_init );
  if( error ) return error;

  error = machine_add_machine( specplus3_init );
  if (error ) return error;

  error = machine_add_machine( specplus3e_init );
  if( error ) return error;

  error = machine_add_machine( tc2048_init );
  if (error ) return error;
  error = machine_add_machine( tc2068_init );
  if( error ) return error;
  error = machine_add_machine( ts2068_init );
  if( error ) return error;
  error = machine_add_machine( pentagon_init );
  if (error ) return error;
  error = machine_add_machine( pentagon512_init );
  if (error ) return error;
  error = machine_add_machine( pentagon1024_init );
  if (error ) return error;
  error = machine_add_machine( scorpion_init );
  if ( error ) return error;
  error = machine_add_machine( spec_se_init );
  if ( error ) return error;

  return 0;
}

static int
machine_add_machine( int (*init_function)( fuse_machine_info *machine ) )
{
  fuse_machine_info *machine;
  int error;

  machine_count++;

  machine_types =
    libspectrum_renew( fuse_machine_info *, machine_types, machine_count );

  machine_types[ machine_count - 1 ] = libspectrum_new( fuse_machine_info, 1 );
  machine = machine_types[ machine_count - 1 ];

  error = init_function( machine ); if( error ) return error;

  machine_set_const_timings( machine );

  machine->capabilities = libspectrum_machine_capabilities( machine->machine );

  return 0;
}

int
machine_select( libspectrum_machine type )
{
  return machine_select_from_snapshot( type, NULL );
}

int
machine_select_from_snapshot( libspectrum_machine type, libspectrum_snap *snap )
{
  int i;

  machine_clear_snapshot_rom_banks();
  int error;

  /* Stop any ongoing RZX */
  rzx_stop_recording();
  rzx_stop_playback( 1 );

  /* We don't want to have to deal with screen size changes in the movie code
     and recording movies where we change machines seems pretty obscure */
  movie_stop();

  for( i=0; i < machine_count; i++ ) {
    if( machine_types[i]->machine == type ) {
      machine_location = i;
      error = machine_select_machine( machine_types[i], snap );

      if( !error ) return 0;

      /* If we couldn't select the new machine type, try falling back
	 to plain old 48K */
      if( type != LIBSPECTRUM_MACHINE_48 ) 
	error = machine_select_from_snapshot( LIBSPECTRUM_MACHINE_48, snap );
	
      /* If that still didn't work, give up */
      if( error ) {
	ui_error( UI_ERROR_ERROR, "can't select 48K machine. Giving up." );
	fuse_abort();
      } else {
	ui_error( UI_ERROR_INFO, "selecting 48K machine" );
	return 0;
      }
      
      return 0;
    }
  }

  ui_error( UI_ERROR_ERROR, "machine type %d unknown", type );
  return 1;
}

int machine_select_id( const char *id )
{
  int i, error;

  for( i=0; i < machine_count; i++ ) {
    if( ! strcmp( machine_types[i]->id, id ) ) {
      error = machine_select_machine( machine_types[i], NULL );
      if( error ) return error;
      return 0;
    }
  }

  ui_error( UI_ERROR_ERROR, "Machine id '%s' unknown", id );
  return 1;
}

int
machine_get_index( libspectrum_machine type )
{
  int i;

  for( i=0; i<machine_count; i++ )
    if( machine_types[i]->machine == type ) return i;

  return -1;
}

const char*
machine_get_id( libspectrum_machine type )
{
  int i = machine_get_index( type );

  return i == -1 ? NULL : machine_types[i]->id;
}

int
machine_get_type( const char* id )
{
  fuse_machine_info* fmi = machine_get_machine_info( id );

  if( fmi ) {
    return fmi->machine;
  }

  return -1;
}

fuse_machine_info*
machine_get_machine_info( const char *id )
{
  int i;

  for( i=0; i < machine_count; i++ ) {
    if( ! strcmp( machine_types[i]->id, id ) ) {
      return machine_types[i];
    }
  }

  return NULL;
}

static int
machine_select_machine( fuse_machine_info *machine, libspectrum_snap *snap )
{
  int width, height;
  int capabilities;

  /* Preserve the pending tape interval in the old clock before clearing
     events and replacing the machine. tape_play() rescales it on resume. */
  if( tape_recording && tape_record_stop() ) return 1;
  if( machine_current ) tape_stop();
  machine_current = machine;

  settings_set_string( &settings_current.start_machine, machine->id );
  
  /* Replacing the frame clock/event queue invalidates pending audio cuts. */
  sound_suspend_subframe();
  tstates = 0;
  general_sound_reanchor();

  /* Reset the event stack */
  event_reset();
  event_add( 0, timer_event );
  event_add( machine->timings.tstates_per_frame, spectrum_frame_event );

  sound_end();

  if( uidisplay_end() ) return 1;

  capabilities = libspectrum_machine_capabilities( machine->machine );

  /* Set screen sizes here */
  if( capabilities & LIBSPECTRUM_MACHINE_CAPABILITY_TIMEX_VIDEO ) {
    width = DISPLAY_SCREEN_WIDTH;
    height = 2*DISPLAY_SCREEN_HEIGHT;
  } else {
    width = DISPLAY_ASPECT_WIDTH;
    height = DISPLAY_SCREEN_HEIGHT;
  }

  if( uidisplay_init( width, height ) ) return 1;

  sound_init( settings_current.sound_device );

  /* Do a hard reset */
  if( machine_reset_from_snapshot( 1, snap ) ) return 1;

  /* And the dock menu item */
  if( capabilities & LIBSPECTRUM_MACHINE_CAPABILITY_TIMEX_DOCK ) {
    ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_DOCK_EJECT, 0 );
  }

  /* Reset any dialogue boxes etc. which contain machine-dependent state */
  ui_widgets_reset();

  return 0;
}

int
machine_load_rom_bank_from_buffer( memory_page* bank_map, int page_num,
  unsigned char *buffer, size_t length, int custom )
{
  size_t offset;
  libspectrum_byte *data = memory_pool_allocate( length );
  memory_page *page;

  memcpy( data, buffer, length );

  for( page = &bank_map[ page_num * MEMORY_PAGES_IN_16K ], offset = 0;
       offset < length;
       page++, offset += MEMORY_PAGE_SIZE ) {
    page->offset = offset;
    page->page_num = page_num;
    page->page = data + offset;
    page->writable = 0;
    page->save_to_snapshot = custom;
  }

  return 0;
}

typedef struct rom_size_spec {
  const size_t *allowed_lengths;
  size_t allowed_length_count;
} rom_size_spec;

typedef enum rom_load_result {
  ROM_LOAD_OK,
  ROM_LOAD_NOT_FOUND,
  ROM_LOAD_BAD_SIZE,
  ROM_LOAD_ERROR,
} rom_load_result;

static int
rom_length_allowed( size_t length, const rom_size_spec *sizes )
{
  size_t i;

  for( i = 0; i < sizes->allowed_length_count; i++ )
    if( length == sizes->allowed_lengths[i] ) return 1;

  return 0;
}

static rom_load_result
machine_load_rom_bank_from_file( memory_page *bank_map, int page_num,
  const char *filename, const rom_size_spec *sizes, int custom,
  size_t *loaded_length )
{
  int error;
  utils_file rom;

  error = utils_read_auxiliary_file( filename, &rom, UTILS_AUXILIARY_ROM );
  if( error == -1 ) return ROM_LOAD_NOT_FOUND;
  if( error ) return ROM_LOAD_ERROR;

  if( !rom_length_allowed( rom.length, sizes ) ) {
    if( loaded_length ) *loaded_length = rom.length;
    utils_close_file( &rom );
    return ROM_LOAD_BAD_SIZE;
  }

  error = machine_load_rom_bank_from_buffer( bank_map, page_num, rom.buffer,
                                             rom.length, custom );
  if( !error && loaded_length ) *loaded_length = rom.length;
  utils_close_file( &rom );

  return error ? ROM_LOAD_ERROR : ROM_LOAD_OK;
}

void
machine_clear_snapshot_rom_bank( memory_page *bank_map, int page_num )
{
  GSList *ptr;

  for( ptr = snapshot_rom_banks; ptr; ptr = ptr->next ) {
    snapshot_rom_bank *bank = ptr->data;
    if( bank->map == bank_map && bank->page_num == page_num ) {
      memory_rom_bank_clear( &bank->bank );
      snapshot_rom_banks = g_slist_remove( snapshot_rom_banks, bank );
      libspectrum_free( bank );
      return;
    }
  }
}

static snapshot_rom_bank *
snapshot_rom_bank_find( memory_page *map, int page_num )
{
  GSList *ptr;
  for( ptr = snapshot_rom_banks; ptr; ptr = ptr->next ) {
    snapshot_rom_bank *bank = ptr->data;
    if( bank->map == map && bank->page_num == page_num ) return bank;
  }
  return NULL;
}

void
machine_clear_snapshot_rom_banks( void )
{
  while( snapshot_rom_banks ) {
    snapshot_rom_bank *bank = snapshot_rom_banks->data;
    memory_rom_bank_clear( &bank->bank );
    snapshot_rom_banks = g_slist_delete_link( snapshot_rom_banks,
                                               snapshot_rom_banks );
    libspectrum_free( bank );
  }
}

int
machine_load_rom_bank_from_snapshot( memory_page *bank_map, int page_num,
  unsigned char *buffer, size_t length, int custom )
{
  snapshot_rom_bank *bank = snapshot_rom_bank_find( bank_map, page_num );

  if( !bank ) {
    bank = libspectrum_new0( snapshot_rom_bank, 1 );
    bank->map = bank_map;
    bank->page_num = page_num;
    snapshot_rom_banks = g_slist_prepend( snapshot_rom_banks, bank );
  }

  if( memory_rom_bank_set( &bank->bank, buffer, length, custom ) ) return 1;
  memory_rom_bank_map( &bank->bank, bank_map, page_num );
  return 0;
}

static int
machine_load_rom_bank_internal( memory_page *bank_map, int page_num,
  const char *filename, const char *fallback, const rom_size_spec *sizes,
  size_t *loaded_length, libspectrum_snap *loading_snapshot )
{
  snapshot_rom_bank *snapshot_bank;
  rom_load_result result;
  const char *name = filename;
  size_t actual_length = 0;
  int custom;

  if( loaded_length ) *loaded_length = 0;
  if( !sizes->allowed_lengths || !sizes->allowed_length_count ) return 1;

  /* Machine ROMs are requested during the machine reset, before modules can
     restore their snapshot state. Resolve their snapshot banks on demand. */
  if( bank_map == memory_map_rom && loading_snapshot &&
      libspectrum_snap_custom_rom( loading_snapshot ) &&
      page_num >= 0 && page_num < 4 &&
      page_num < libspectrum_snap_custom_rom_pages( loading_snapshot ) &&
      libspectrum_snap_roms( loading_snapshot, page_num ) &&
      !snapshot_rom_bank_find( bank_map, page_num ) ) {
    if( machine_load_rom_bank_from_snapshot( bank_map, page_num,
          libspectrum_snap_roms( loading_snapshot, page_num ),
          libspectrum_snap_rom_length( loading_snapshot, page_num ), 1 ) )
      return 1;
  }

  snapshot_bank = snapshot_rom_bank_find( bank_map, page_num );
  if( snapshot_bank ) {
    if( !rom_length_allowed( snapshot_bank->bank.length, sizes ) ) {
      ui_error( UI_ERROR_ERROR,
                "snapshot ROM is %ld bytes long; unsupported size",
                (unsigned long)snapshot_bank->bank.length );
      return 1;
    }

    memory_rom_bank_map( &snapshot_bank->bank, bank_map, page_num );
    if( loaded_length ) *loaded_length = snapshot_bank->bank.length;
    return 0;
  }

  custom = fallback && strcmp( filename, fallback );
  result = machine_load_rom_bank_from_file( bank_map, page_num, name, sizes,
                                            custom, &actual_length );

  if( result != ROM_LOAD_OK && fallback && custom ) {
    name = fallback;
    result = machine_load_rom_bank_from_file( bank_map, page_num, name, sizes,
                                              0, &actual_length );
  }

  if( result == ROM_LOAD_OK ) {
    if( loaded_length ) *loaded_length = actual_length;
    return 0;
  }

  if( result == ROM_LOAD_NOT_FOUND ) {
    ui_error( UI_ERROR_ERROR, "couldn't find ROM '%s'", name );
  } else if( result == ROM_LOAD_BAD_SIZE ) {
    if( sizes->allowed_length_count == 1 )
      ui_error( UI_ERROR_ERROR,
                "ROM '%s' is %ld bytes long; expected %ld bytes", name,
                (unsigned long)actual_length,
                (unsigned long)sizes->allowed_lengths[0] );
    else
      ui_error( UI_ERROR_ERROR,
                "ROM '%s' is %ld bytes long; unsupported size", name,
                (unsigned long)actual_length );
  }

  return 1;
}

int
machine_load_rom_bank( memory_page *bank_map, int page_num,
  const char *filename, const char *fallback, size_t expected_length )
{
  const rom_size_spec sizes = { &expected_length, 1 };

  return machine_load_rom_bank_internal( bank_map, page_num, filename,
                                         fallback, &sizes, NULL, NULL );
}

int
machine_load_rom_bank_with_sizes( memory_page *bank_map, int page_num,
  const char *filename, const char *fallback, const size_t *allowed_lengths,
  size_t allowed_length_count, size_t *loaded_length )
{
  const rom_size_spec sizes = { allowed_lengths, allowed_length_count };

  return machine_load_rom_bank_internal( bank_map, page_num, filename,
                                         fallback, &sizes, loaded_length, NULL );
}

int
machine_load_rom( int page_num, const char *filename, const char *fallback,
  size_t expected_length, libspectrum_snap *snap )
{
  const rom_size_spec sizes = { &expected_length, 1 };
  return machine_load_rom_bank_internal( memory_map_rom, page_num, filename,
                                         fallback, &sizes, NULL, snap );
}

int
machine_reset( int hard_reset )
{
  return machine_reset_from_snapshot( hard_reset, NULL );
}

int
machine_reset_from_snapshot( int hard_reset, libspectrum_snap *snap )
{
  size_t i;
  int error;

  if( tape_recording && tape_record_stop() ) return 1;
  if( hard_reset ) machine_clear_snapshot_rom_banks();

  /* Clear poke list (undoes effects of active pokes on Spectrum memory) */
  pokemem_clear();

  sound_ay_reset();

  tape_stop();

  memory_pool_free();

  machine_current->ram.romcs = 0;

  machine_set_variable_timings( machine_current );

  memory_reset();

  /* Snapshot cartridge pages replace, rather than overlay, inserted media. */
  if( snap ) dck_discard();

  /* Do the machine-specific bits, including loading the ROMs */
  error = machine_current->reset( hard_reset, snap ); if( error ) return error;

  module_reset( hard_reset, snap );

  error = machine_current->memory_map(); if( error ) return error;

  /* Select 50 or 60 Hz emulation timer */
  SetEmulationHz( (float)machine_current->timings.processor_speed /
                  machine_current->timings.tstates_per_frame );

  /* Set up the contention array */
  for( i = 0; i < machine_current->timings.tstates_per_frame; i++ ) {
    ula_contention[ i ] = machine_current->ram.contend_delay( i );
    ula_contention_no_mreq[ i ] = machine_current->ram.contend_delay_no_mreq( i );
  }

  /* Update the disk menu items */
  ui_menu_disk_update();

  /* clear out old display image ready for new one */
  display_refresh_all();

  gdbserver_on_machine_reset();

  return 0;
}

static void
machine_set_const_timings( fuse_machine_info *machine )
{
  /* Pull timings we use repeatedly out of libspectrum and store them
     for ourself */
  machine->timings.processor_speed =
    libspectrum_timings_processor_speed( machine->machine );
  machine->timings.left_border =
    libspectrum_timings_left_border( machine->machine );
  machine->timings.horizontal_screen =
    libspectrum_timings_horizontal_screen( machine->machine );
  machine->timings.right_border =
    libspectrum_timings_right_border( machine->machine );
  machine->timings.tstates_per_line =
    libspectrum_timings_tstates_per_line( machine->machine );
  machine->timings.interrupt_length =
    libspectrum_timings_interrupt_length( machine->machine );
  machine->timings.tstates_per_frame =
    libspectrum_timings_tstates_per_frame( machine->machine );
}

static void
machine_set_variable_timings( fuse_machine_info *machine )
{
  size_t y;

  /* Magic number alert

     libspectrum_timings_top_left_pixel gives us the number of tstates
     after the interrupt at which the top-left pixel of the screen is
     displayed. However, what's more useful for Fuse is when the first
     pixel of the top line of the border is displayed.

     Fuse shows DISPLAY_BORDER_HEIGHT lines of top border and
     DISPLAY_BORDER_WIDTH_COLS columns of left border, so we subtract
     the appropriate offset to get when the top-left pixel of the
     border is displayed.
  */

  machine->line_times[0]=
    libspectrum_timings_top_left_pixel( machine->machine ) -
    /* DISPLAY_BORDER_HEIGHT lines of top border */
    DISPLAY_BORDER_HEIGHT * machine->timings.tstates_per_line -
    4 * DISPLAY_BORDER_WIDTH_COLS; /* Left border at 4 tstates per column */

  if( settings_current.late_timings ) machine->line_times[0]++;

  for( y=1; y<DISPLAY_SCREEN_HEIGHT+1; y++ ) {
    machine->line_times[y] = machine->line_times[y-1] + 
                             machine->timings.tstates_per_line;
  }
}

static void
machine_end( void )
{
  int i;

  for( i=0; i<machine_count; i++ ) {
    if( machine_types[i]->shutdown ) machine_types[i]->shutdown();
    libspectrum_free( machine_types[i] );
  }

  libspectrum_free( machine_types );
}

void
machine_register_startup( void )
{
  startup_manager_module dependencies[] = {
    STARTUP_MANAGER_MODULE_MEMORY,
    STARTUP_MANAGER_MODULE_SETUID
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_MACHINE, dependencies,
                            ARRAY_SIZE( dependencies ), machine_init_machines,
                            NULL, machine_end );
}

/* The Cocoa app implements this in fusepb/FuseMenus.m; every other build,
   macOS autotools included, needs the stub. */
#ifndef UI_COCOA
void
SetEmulationHz( float hz )
{
  (void)hz;
}
#endif				/* #ifndef UI_COCOA */
