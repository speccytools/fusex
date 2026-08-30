/* if2.c: Interface 2 cartridge handling routines
   Copyright (c) 2003-2021 Darren Salt, Fredrick Meunier, Philip Kendall

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

   Darren: linux@youmustbejoking.demon.co.uk
   Fred: fredm@spamcop.net

*/

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "if2.h"
#include "infrastructure/startup_manager.h"
#include "machine.h"
#include "memory_pages.h"
#include "module.h"
#include "periph.h"
#include "settings.h"
#include "ui/ui.h"
#include "unittests/unittests.h"
#include "utils.h"

/* A 16KB memory chunk accessible by the Z80 when /ROMCS is low */
static memory_page if2_memory_map_romcs[MEMORY_PAGES_IN_16K];

/* IF2 cart inserted? */
int if2_active = 0;

/* IF2 memory source */
static int if2_memory_source;
static const utils_file *if2_loaded_file;
static memory_rom_bank if2_snapshot_cartridge;

static void if2_end( void );

static void if2_reset( int hard_reset );
static void if2_memory_map( void );
static void if2_enabled_snapshot( libspectrum_snap *snap );
static void if2_from_snapshot( libspectrum_snap *snap );
static void if2_to_snapshot( libspectrum_snap *snap );

static module_info_t if2_module_info = {

  /* .reset = */ if2_reset,
  /* .romcs = */ if2_memory_map,
  /* .snapshot_enabled = */ if2_enabled_snapshot,
  /* .snapshot_from = */ if2_from_snapshot,
  /* .snapshot_to = */ if2_to_snapshot,

};

static const periph_t if2_periph = {
  /* .option = */ &settings_current.interface2,
  /* .ports = */ NULL,
  /* .hard_reset = */ 0,
  /* .activate = */ NULL,
};

static int
if2_init( void *context )
{
  int i;
  int if2_source;

  module_register( &if2_module_info );

  if2_source = memory_source_register( "If2" );
  for( i = 0; i < MEMORY_PAGES_IN_16K; i++ )
    if2_memory_map_romcs[i].source = if2_source;

  periph_register( PERIPH_TYPE_INTERFACE2, &if2_periph );

  return 0;
}

void
if2_register_startup( void )
{
  startup_manager_module dependencies[] = {
    STARTUP_MANAGER_MODULE_MEMORY,
    STARTUP_MANAGER_MODULE_SETUID,
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_IF2, dependencies,
                            ARRAY_SIZE( dependencies ), if2_init, NULL,
                            if2_end );
}

static void
if2_end( void )
{
  memory_rom_bank_clear( &if2_snapshot_cartridge );
}

static int
if2_insert_internal( const char *filename, const utils_file *file )
{
  if ( !periph_is_active( PERIPH_TYPE_INTERFACE2 ) ) {
    ui_error( UI_ERROR_ERROR,
	      "This machine does not support the Interface 2" );
    return 1;
  }

  settings_set_string( &settings_current.if2_file, filename );
  memory_rom_bank_clear( &if2_snapshot_cartridge );

  if2_loaded_file = file;
  machine_reset( 0 );
  if2_loaded_file = NULL;

  return 0;
}

int
if2_insert( const char *filename )
{
  return if2_insert_internal( filename, NULL );
}

int
if2_insert_loaded( const utils_file *file )
{
  if( !file || !file->filename || !file->buffer ) return 1;
  return if2_insert_internal( file->filename, file );
}

void
if2_eject( void )
{
  if ( !periph_is_active( PERIPH_TYPE_INTERFACE2 ) ) {
    ui_error( UI_ERROR_ERROR,
	      "This machine does not support the Interface 2" );
    return;
  }

  if( settings_current.if2_file ) libspectrum_free( settings_current.if2_file );
  settings_current.if2_file = NULL;
  memory_rom_bank_clear( &if2_snapshot_cartridge );

  machine_current->ram.romcs = 0;

  ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_IF2_EJECT, 0 );

  machine_reset( 0 );
}

static void
if2_reset( int hard_reset )
{
  if2_active = 0;
  if( hard_reset ) memory_rom_bank_clear( &if2_snapshot_cartridge );

  if( !periph_is_active( PERIPH_TYPE_INTERFACE2 ) ) return;

  if( if2_snapshot_cartridge.data ) {
    memory_rom_bank_map( &if2_snapshot_cartridge, if2_memory_map_romcs, 0 );
    if2_active = 1;
    machine_current->ram.romcs = 1;
    ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_IF2_EJECT, 1 );
    return;
  }

  if( !settings_current.if2_file ) {
    ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_IF2_EJECT, 0 );
    return;
  }

  if( if2_loaded_file ) {
    if( if2_loaded_file->length != 0x4000 ||
        machine_load_rom_bank_from_snapshot( if2_memory_map_romcs, 0,
                                           if2_loaded_file->buffer,
                                           if2_loaded_file->length, 1 ) )
      return;
  } else if( machine_load_rom_bank( if2_memory_map_romcs, 0,
                                    settings_current.if2_file,
                                    NULL, 0x4000 ) )
    return;

  machine_current->ram.romcs = 1;

  if2_active = 1;

  ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_IF2_EJECT, 1 );
}

static void
if2_memory_map( void )
{
  if( !if2_active ) return;

  memory_map_romcs_full( if2_memory_map_romcs );
}

static void
if2_enabled_snapshot( libspectrum_snap *snap )
{
  settings_current.interface2 = libspectrum_snap_interface2_active( snap );
}

static void
if2_from_snapshot( libspectrum_snap *snap )
{
  if( !libspectrum_snap_interface2_active( snap ) ) return;

  if2_active = 1;
  machine_current->ram.romcs = 1;

  if( !libspectrum_snap_interface2_rom( snap, 0 ) ||
      memory_rom_bank_set( &if2_snapshot_cartridge,
                           libspectrum_snap_interface2_rom( snap, 0 ),
                           0x4000, 1 ) )
    return;

  memory_rom_bank_map( &if2_snapshot_cartridge, if2_memory_map_romcs, 0 );

  ui_menu_activate( UI_MENU_ITEM_MEDIA_CARTRIDGE_IF2_EJECT, 1 );

  machine_current->memory_map();
}

static void
if2_to_snapshot( libspectrum_snap *snap )
{
  libspectrum_byte *buffer;
  int i;

  if( !if2_active ) return;

  libspectrum_snap_set_interface2_active( snap, 1 );

  buffer = libspectrum_new( libspectrum_byte, 0x4000 );

  for( i = 0; i < MEMORY_PAGES_IN_16K; i++ )
    memcpy( buffer + i * MEMORY_PAGE_SIZE,
            if2_memory_map_romcs[ i ].page, MEMORY_PAGE_SIZE );
  libspectrum_snap_set_interface2_rom( snap, 0, buffer );
}

int
if2_unittest( void )
{
  libspectrum_snap *snap;
  libspectrum_byte *rom;
  int saved_interface2 = settings_current.interface2;
  int r = 0;

  if2_active = 1;
  machine_current->memory_map();

  r += unittests_assert_16k_page( 0x0000, if2_memory_source, 0 );
  r += unittests_assert_16k_ram_page( 0x4000, 5 );
  r += unittests_assert_16k_ram_page( 0x8000, 2 );
  r += unittests_assert_16k_ram_page( 0xc000, 0 );

  if2_active = 0;
  machine_current->memory_map();

  snap = libspectrum_snap_alloc();
  if( !snap ) {
    fprintf( stderr, "Couldn't allocate Interface 2 unit test snapshot\n" );
    return r + 1;
  }

  rom = libspectrum_new( libspectrum_byte, 0x4000 );
  memset( rom, 0xa5, 0x4000 );
  libspectrum_snap_set_interface2_active( snap, 1 );
  libspectrum_snap_set_interface2_rom( snap, 0, rom );
  if2_enabled_snapshot( snap );
  if2_from_snapshot( snap );

  if( machine_reset( 0 ) || !if2_active ||
      if2_memory_map_romcs[ 0 ].page[ 0 ] != 0xa5 ) {
    fprintf( stderr, "Interface 2 snapshot cartridge was not preserved by soft reset\n" );
    r++;
  }

  if( machine_reset( 1 ) || if2_active ) {
    fprintf( stderr, "Interface 2 snapshot cartridge survived hard reset\n" );
    r++;
  }

  if( libspectrum_snap_free( snap ) ) {
    fprintf( stderr, "Couldn't free Interface 2 unit test snapshot\n" );
    r++;
  }

  if2_active = 0;
  settings_current.interface2 = saved_interface2;
  machine_current->ram.romcs = 0;
  machine_current->memory_map();

  r += unittests_paging_test_48( 2 );

  return r;
}
