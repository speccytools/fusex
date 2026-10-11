/* ulaplus.c: ULA+ palette extension
   Copyright (c) 2026 The FuseX authors

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

*/

#include "config.h"

#include <string.h>

#include "libspectrum.h"

#include "compat.h"
#include "display.h"
#include "infrastructure/startup_manager.h"
#include "module.h"
#include "periph.h"
#include "settings.h"
#include "ulaplus.h"

#define ULAPLUS_REGISTER_PORT 0xbf3b
#define ULAPLUS_DATA_PORT     0xff3b

#define ULAPLUS_GROUP_PALETTE 0
#define ULAPLUS_GROUP_MODE    1

/* The 16 standard Spectrum colours as GGGRRRBB: the eight normal colours
   followed by the eight bright ones */
static const libspectrum_byte standard_colours[16] = {
  0x00, 0x02, 0x10, 0x12, 0x80, 0x82, 0x90, 0x92,
  0x00, 0x03, 0x1c, 0x1f, 0xe0, 0xe3, 0xfc, 0xff,
};

static libspectrum_byte palette[ ULAPLUS_PALETTE_SIZE ];

/* The last values written to the register and data ports */
static libspectrum_byte register_value;
static libspectrum_byte data_value;

static int mode_enabled;

static void ulaplus_reset( int hard_reset );
static void ulaplus_enabled_snapshot( libspectrum_snap *snap );
static void ulaplus_from_snapshot( libspectrum_snap *snap );
static void ulaplus_to_snapshot( libspectrum_snap *snap );
static libspectrum_byte ulaplus_data_read( libspectrum_word port,
                                           libspectrum_byte *attached );
static void ulaplus_register_write( libspectrum_word port,
                                    libspectrum_byte data );
static void ulaplus_data_write( libspectrum_word port, libspectrum_byte data );

static module_info_t ulaplus_module_info = {

  /* .reset = */ ulaplus_reset,
  /* .romcs = */ NULL,
  /* .snapshot_enabled = */ ulaplus_enabled_snapshot,
  /* .snapshot_from = */ ulaplus_from_snapshot,
  /* .snapshot_to = */ ulaplus_to_snapshot,

};

static const periph_port_t ulaplus_ports[] = {
  { 0xffff, ULAPLUS_REGISTER_PORT, NULL, ulaplus_register_write },
  { 0xffff, ULAPLUS_DATA_PORT, ulaplus_data_read, ulaplus_data_write },
  { 0, 0, NULL, NULL }
};

static const periph_t ulaplus_periph = {
  /* .option = */ &settings_current.ulaplus,
  /* .ports = */ ulaplus_ports,
  /* .hard_reset = */ 0,
  /* .activate = */ NULL,
};

static int
ulaplus_init( void *context )
{
  module_register( &ulaplus_module_info );
  periph_register( PERIPH_TYPE_ULAPLUS, &ulaplus_periph );

  return 0;
}

void
ulaplus_register_startup( void )
{
  startup_manager_module dependencies[] = { STARTUP_MANAGER_MODULE_SETUID };
  startup_manager_register( STARTUP_MANAGER_MODULE_ULAPLUS, dependencies,
                            ARRAY_SIZE( dependencies ), ulaplus_init, NULL,
                            NULL );
}

int
ulaplus_is_enabled( void )
{
  /* Test the mode first: the display code asks before the peripherals have
     started up, when periph_is_active() cannot be called */
  return mode_enabled && periph_is_active( PERIPH_TYPE_ULAPLUS );
}

libspectrum_byte
ulaplus_get_colour( int entry )
{
  return palette[ entry & ( ULAPLUS_PALETTE_SIZE - 1 ) ];
}

/* Load the standard colours into each of the four colour lookup tables. Bit 0
   of the table number selects the bright colours */
static void
load_standard_colours( void )
{
  int clut, i;

  for( clut = 0; clut < 4; clut++ ) {
    const libspectrum_byte *colours = standard_colours + 8 * ( clut & 1 );
    for( i = 0; i < 8; i++ ) {
      palette[ clut * 16 + i ] = colours[ i ];
      palette[ clut * 16 + 8 + i ] = colours[ i ];
    }
  }
}

static void
ulaplus_reset( int hard_reset GCC_UNUSED )
{
  register_value = 0;
  data_value = 0;
  mode_enabled = 0;
  load_standard_colours();

  display_border_recheck();
}

static void
ulaplus_register_write( libspectrum_word port GCC_UNUSED,
                        libspectrum_byte data )
{
  register_value = data;
}

static libspectrum_byte
ulaplus_data_read( libspectrum_word port GCC_UNUSED,
                   libspectrum_byte *attached )
{
  libspectrum_byte group = register_value >> 6;

  *attached = 0xff;

  if( group == ULAPLUS_GROUP_PALETTE )
    return palette[ register_value & ( ULAPLUS_PALETTE_SIZE - 1 ) ];

  if( group == ULAPLUS_GROUP_MODE ) return mode_enabled ? 1 : 0;

  return 0xff;
}

static void
ulaplus_data_write( libspectrum_word port GCC_UNUSED, libspectrum_byte data )
{
  libspectrum_byte group = register_value >> 6;

  data_value = data;

  if( group == ULAPLUS_GROUP_PALETTE ) {
    libspectrum_byte *entry =
      &palette[ register_value & ( ULAPLUS_PALETTE_SIZE - 1 ) ];

    if( *entry != data ) {
      if( mode_enabled ) display_palette_changed();
      *entry = data;
      display_border_recheck();
    }
  } else if( group == ULAPLUS_GROUP_MODE ) {
    if( mode_enabled != ( data & 1 ) ) {
      display_palette_changed();
      mode_enabled = data & 1;
      display_border_recheck();
    }
  }
}

static void
ulaplus_enabled_snapshot( libspectrum_snap *snap )
{
  settings_current.ulaplus = libspectrum_snap_ulaplus_active( snap );
}

static void
ulaplus_from_snapshot( libspectrum_snap *snap )
{
  libspectrum_byte *snap_palette;

  if( !libspectrum_snap_ulaplus_active( snap ) ) return;

  snap_palette = libspectrum_snap_ulaplus_palette( snap, 0 );
  if( snap_palette ) memcpy( palette, snap_palette, ULAPLUS_PALETTE_SIZE );

  mode_enabled = libspectrum_snap_ulaplus_palette_enabled( snap );
  register_value = libspectrum_snap_ulaplus_current_register( snap );
  data_value = libspectrum_snap_ulaplus_ff_register( snap );

  display_refresh_all();
}

static void
ulaplus_to_snapshot( libspectrum_snap *snap )
{
  libspectrum_byte *snap_palette;

  if( !periph_is_active( PERIPH_TYPE_ULAPLUS ) ) return;

  libspectrum_snap_set_ulaplus_active( snap, 1 );
  libspectrum_snap_set_ulaplus_palette_enabled( snap, mode_enabled );
  libspectrum_snap_set_ulaplus_current_register( snap, register_value );
  libspectrum_snap_set_ulaplus_ff_register( snap, data_value );

  snap_palette = libspectrum_new( libspectrum_byte, ULAPLUS_PALETTE_SIZE );
  memcpy( snap_palette, palette, ULAPLUS_PALETTE_SIZE );
  libspectrum_snap_set_ulaplus_palette( snap, 0, snap_palette );
}
