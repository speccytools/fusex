/* dck.c: dock snapshot (Warajevo .DCK) handling routines
   Copyright (c) 2003-2021 Darren Salt, Fredrick Meunier, Philip Kendall
   Copyright (c) 2015 Sergio Baldoví
   Copyright (c) 2015-2021 Fredrick Meunier

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

   Philip: philip-fuse@shadowmagic.org.uk

   Darren: linux@youmustbejoking.demon.co.uk
   Fred: fredm@spamcop.net

*/

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "libspectrum.h"

#include "dck.h"
#include "machine.h"
#include "memory_pages.h"
#include "settings.h"
#include "scld.h"
#include "ui/ui.h"
#include "utils.h"
#include "debugger/debugger.h"

/* Dock cart inserted? */
int dck_active = 0;
static const utils_file *dck_loaded_file;
static memory_rom_bank dck_banks[3][8];

static void
dck_clear_banks( void )
{
  int bank, page;

  for( bank = 0; bank < 3; bank++ )
    for( page = 0; page < 8; page++ )
      memory_rom_bank_clear( &dck_banks[ bank ][ page ] );
}

static memory_rom_bank *
dck_get_bank( libspectrum_dck_bank bank, int page )
{
  if( bank > LIBSPECTRUM_DCK_BANK_EXROM || page < 0 || page >= 8 )
    return NULL;

  return &dck_banks[ bank ][ page ];
}

static int
dck_insert_internal( const char *filename, const utils_file *file )
{
  if ( !( libspectrum_machine_capabilities( machine_current->machine ) &
	  LIBSPECTRUM_MACHINE_CAPABILITY_TIMEX_DOCK ) ) {
    ui_error( UI_ERROR_ERROR, "This machine does not support the dock" );
    return 1;
  }

  settings_set_string( &settings_current.dck_file, filename );
  dck_active = 0;

  dck_loaded_file = file;
  machine_reset( 0 );
  dck_loaded_file = NULL;

  return 0;
}

int
dck_insert( const char *filename )
{
  return dck_insert_internal( filename, NULL );
}

int
dck_insert_loaded( const utils_file *file )
{
  if( !file || !file->filename || !file->buffer ) return 1;
  return dck_insert_internal( file->filename, file );
}

void
dck_eject( void )
{
  if ( !( libspectrum_machine_capabilities( machine_current->machine ) &
	  LIBSPECTRUM_MACHINE_CAPABILITY_TIMEX_DOCK ) ) {
    ui_error( UI_ERROR_ERROR, "This machine does not support the dock" );
    return;
  }

  settings_set_string( &settings_current.dck_file, NULL );

  dck_active = 0;

  ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_DOCK_EJECT, 0 );

  machine_reset( 0 );
}

static memory_page *
dck_get_memory_page( libspectrum_dck_bank bank, size_t index )
{
    switch( bank ) {
    case LIBSPECTRUM_DCK_BANK_HOME:
      return timex_home[ index ];
    case LIBSPECTRUM_DCK_BANK_DOCK:
      return &timex_dock[ index ];
    case LIBSPECTRUM_DCK_BANK_EXROM:
      return &timex_exrom[ index ];
    default:
      return NULL;
    }
}

int
dck_reset( int hard_reset )
{
  if( hard_reset ) dck_clear_banks();
  if( !hard_reset && dck_active ) return 0;
  utils_file file;
  size_t num_block = 0;
  libspectrum_dck *dck;
  int error;

  dck_active = 0;

  if( !settings_current.dck_file ) {
    ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_DOCK_EJECT, 0 );
    return 0;
  }

  dck = libspectrum_dck_alloc();

  if( dck_loaded_file ) {
    error = libspectrum_dck_read2( dck, dck_loaded_file->buffer,
                                   dck_loaded_file->length,
                                   dck_loaded_file->filename );
    if( error ) { libspectrum_dck_free( dck, 0 ); return error; }
  } else {
    error = utils_read_file( settings_current.dck_file, &file );
    if( error ) { libspectrum_dck_free( dck, 0 ); return error; }

    error = libspectrum_dck_read2( dck, file.buffer, file.length,
                                   settings_current.dck_file );
    utils_close_file( &file );
    if( error ) { libspectrum_dck_free( dck, 0 ); return error; }
  }

  while( dck->dck[num_block] != NULL ) {
    memory_page *page;
    int i;
    libspectrum_dck_bank dck_bank = dck->dck[num_block]->bank;

    if( dck_bank != LIBSPECTRUM_DCK_BANK_HOME &&
        dck_bank != LIBSPECTRUM_DCK_BANK_DOCK &&
        dck_bank != LIBSPECTRUM_DCK_BANK_EXROM ) {
      ui_error( UI_ERROR_INFO, "Sorry, bank ID %i is unsupported",
		dck->dck[num_block]->bank );
      libspectrum_dck_free( dck, 0 );
      return 1;
    }

    for( i = 0; i < 8; i++ ) {

      libspectrum_byte *data;
      int j;

      switch( dck->dck[num_block]->access[i] ) {

      case LIBSPECTRUM_DCK_PAGE_NULL:
        break;

      case LIBSPECTRUM_DCK_PAGE_ROM:
      {
        memory_rom_bank *bank = dck_get_bank( dck_bank, i );
        if( !bank || memory_rom_bank_set( bank,
              dck->dck[num_block]->pages[i], 0x2000, 1 ) ) {
          libspectrum_dck_free( dck, 0 );
          return 1;
        }
        data = bank->data;
        for( j = 0; j < MEMORY_PAGES_IN_8K; j++ ) {
          page = dck_get_memory_page( dck_bank, i * MEMORY_PAGES_IN_8K + j);
          page->offset = j * MEMORY_PAGE_SIZE;
          page->writable = 0;
          page->save_to_snapshot = 1;
          page->page = data + page->offset;
        }
        break;
      }

      case LIBSPECTRUM_DCK_PAGE_RAM_EMPTY:
      case LIBSPECTRUM_DCK_PAGE_RAM:
	/* Because the scr and snapshot code depends on the standard
	   memory map being in the RAM[] array, we just copy RAM
	   blocks from the HOME bank into the appropriate page; in
	   other cases, we allocate ourselves a new page to store the
	   contents in */
        if( dck_bank == LIBSPECTRUM_DCK_BANK_HOME && i>1 ) {
          for( j = 0; j < MEMORY_PAGES_IN_8K; j++ ) {
            page = dck_get_memory_page( dck_bank, i * MEMORY_PAGES_IN_8K + j);
            if( dck->dck[num_block]->access[i] == LIBSPECTRUM_DCK_PAGE_RAM ) {
              memcpy( page->page,
                dck->dck[num_block]->pages[i] + j * MEMORY_PAGE_SIZE,
                MEMORY_PAGE_SIZE );
            } else {
              memset( page->page, 0, MEMORY_PAGE_SIZE );
            }
          }
        } else {
          static const libspectrum_byte empty_page[ 0x2000 ];
          memory_rom_bank *bank = dck_get_bank( dck_bank, i );
          const libspectrum_byte *source = empty_page;

          if( dck->dck[num_block]->access[i] == LIBSPECTRUM_DCK_PAGE_RAM )
            source = dck->dck[num_block]->pages[i];

          if( !bank || memory_rom_bank_set( bank, source, 0x2000, 1 ) ) {
            libspectrum_dck_free( dck, 0 );
            return 1;
          }
          data = bank->data;
          for( j = 0; j < MEMORY_PAGES_IN_8K; j++ ) {
            page = dck_get_memory_page( dck_bank, i * MEMORY_PAGES_IN_8K + j);
            page->offset = j * MEMORY_PAGE_SIZE;
            page->writable = 1;
            page->save_to_snapshot = 1;
            page->page = data + page->offset;
          }
        }
        break;

      }
    }
    num_block++;
  }

  dck_active = 1;

  /* Reset contention for pages */
  scld_set_exrom_dock_contention();

  /* Make the menu item to eject the cartridge active */
  ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_DOCK_EJECT, 1 );

  return libspectrum_dck_free( dck, 0 );
}
