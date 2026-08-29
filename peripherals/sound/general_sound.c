/* general_sound.c: Routines for handling the General Sound card
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
#include "general_sound.h"
#include "gs_z80.h"
#include "machine.h"
#include "infrastructure/startup_manager.h"
#include "module.h"
#include "options.h"
#include "periph.h"
#include "settings.h"
#include "sound.h"
#include "spectrum.h"
#include "ui/ui.h"
#include "utils.h"

/* Status bits shared by the host and the card. Bit 7 marks a byte waiting
   in the data latch, bit 0 a byte waiting in the command latch; each side
   sets the bit when it writes and clears it when it reads. Bits 1 to 6 are
   not driven and read back high. */
#define GS_STATUS_DATA    0x80
#define GS_STATUS_COMMAND 0x01
#define GS_STATUS_UNDRIVEN 0x7e

#define GS_PAGE_KB   16
#define GS_PAGE_SIZE ( GS_PAGE_KB * 1024 )

/* The card carries 32 pages of ROM whatever the RAM size */
#define GS_ROM_PAGES     32
#define GS_ROM_PAGE_MASK ( GS_ROM_PAGES - 1 )
#define GS_ROM_SIZE      ( GS_ROM_PAGES * GS_PAGE_SIZE )

/* GSCFG0 */
#define GS_CFG0_NOROM 0x01
#define GS_CFG0_RAMRO 0x02
#define GS_CFG0_EXPAG 0x08
/* Eight-channel mode belongs to the NGS extension and is not emulated, so
   the bit reads back clear. */
#define GS_CFG0_8CHANS 0x04

/* A channel swings +-128 against a six-bit volume, so a side reaches
   about +-12000 before scaling; this puts the card in the same range as
   the other digitized sources. */
#define GS_MIX_SCALE 2

static libspectrum_byte gs_status;
static libspectrum_byte gs_command;
static libspectrum_byte gs_data_to_host;
static libspectrum_byte gs_data_from_host;

/* The card's own memory. The ROM is a fixed 32 pages; the RAM size is a
   user setting. Both are held flat and mapped into the Z80's four banks
   through the pointer tables below, with a NULL write pointer meaning the
   bank is read-only. */
static libspectrum_byte *gs_rom;
static libspectrum_byte *gs_ram;
static size_t gs_ram_pages;
static libspectrum_byte gs_ram_page_mask;

libspectrum_byte *general_sound_bank_read[4];
libspectrum_byte *general_sound_bank_write[4];

/* Page selected by MPAG, and the page bank 3 takes. Outside extended
   paging a single MPAG write sets both, the second to the odd page above
   the first. */
static libspectrum_byte gs_page;
static libspectrum_byte gs_page_ext;
static libspectrum_byte gs_cfg0;

/* Six-bit attenuation per channel, written by the card's own Z80. */
static libspectrum_byte gs_volume[4];

/* Last byte each channel's DAC was handed. The card powers up with all
   four at mid-scale. */
static libspectrum_byte gs_dac[4] = { 0x80, 0x80, 0x80, 0x80 };

/* The card's absolute T-state count at the start of the current host
   frame. Its clock runs at a fixed ratio to the host's, so a flush maps
   the host T-state into the card's timebase in one division from here,
   rather than accumulating a rounding error per flush. */
static libspectrum_qword gs_frame_base;

/* The part of a frame's conversion that did not divide out, carried into
   the next one. */
static libspectrum_dword gs_frame_remainder;

/* Clear while the card has no usable ROM: it responds to nothing and its
   Z80 does not run. */
static int gs_available;

/* Whether the user has the card fitted. Loading a snapshot clears every
   optional peripheral's setting before asking the modules to restore what
   the snapshot says; nothing here is stored in a snapshot, so the card
   restores what the user chose. */
static int gs_fitted;

/* What the last build was attempted with, successful or not. */
static char *gs_loaded_rom;
static size_t gs_loaded_ram_pages;

static void gs_update_mem_mapping( void );
static int gs_allocate( void );
static void gs_flush( void );
static void gs_apply_settings( int force );
static void gs_mix( void );

/* The card's paging registers rotate their operand left by one bit. */
#define GS_ROL8( v ) ( (libspectrum_byte)( ( (v) << 1 ) | ( (v) >> 7 ) ) )

static void gs_reset( int hard_reset );
static void gs_reset_card( void );
static void gs_enabled_snapshot( libspectrum_snap *snap );
static void gs_resync_sound( void );
static void gs_activate( void );

static libspectrum_byte gs_read( libspectrum_word port,
					    libspectrum_byte *attached );
static void gs_write( libspectrum_word port,
				 libspectrum_byte val );
static void gs_control_write( libspectrum_word port,
					 libspectrum_byte val );

static module_info_t gs_module_info = {

  /* .reset = */ gs_reset,
  /* .romcs = */ NULL,
  /* .snapshot_enabled = */ gs_enabled_snapshot,
  /* .snapshot_from = */ NULL,
  /* .snapshot_to = */ NULL,

};

/* 0x33 is write-only. The 0xf7 mask covers 0xb3 and 0xbb alike; the
   handlers tell them apart on bit 3. */
static const periph_port_t gs_ports[] = {
  { 0x00ff, 0x0033, NULL, gs_control_write },
  { 0x00f7, 0x00b3, gs_read, gs_write },
  { 0, 0, NULL, NULL }
};

static const periph_t gs_periph = {
  /* .option = */ &settings_current.general_sound,
  /* .ports = */ gs_ports,
  /* .hard_reset = */ 1,
  /* .activate = */ gs_activate,
};

static int
gs_init( void *context )
{
  module_register( &gs_module_info );
  periph_register( PERIPH_TYPE_GENERAL_SOUND, &gs_periph );

  return 0;
}

static void
gs_end( void )
{
  libspectrum_free( gs_rom );
  libspectrum_free( gs_ram );
  libspectrum_free( gs_loaded_rom );
  gs_rom = gs_ram = NULL;
  gs_loaded_rom = NULL;
  gs_loaded_ram_pages = 0;
  gs_available = 0;
}

void
general_sound_register_startup( void )
{
  startup_manager_module dependencies[] = { STARTUP_MANAGER_MODULE_SETUID };
  startup_manager_register( STARTUP_MANAGER_MODULE_GENERAL_SOUND, dependencies,
			    ARRAY_SIZE( dependencies ), gs_init,
			    NULL, gs_end );
}

/* Two unset paths count as the same, so a card with no ROM chosen does not
   read as a change of settings. */
static int
gs_same_path( const char *a, const char *b )
{
  if( !a || !b ) return a == b;
  return !strcmp( a, b );
}

/* Build or rebuild the card whenever the ROM path or the amount of memory
   differs from the last attempt's. Swapping either is the equivalent of
   changing the hardware, so the card comes up reset. A failed attempt is
   recorded like any other, which keeps an unreadable ROM out of the frame
   path; force rebuilds whatever the last attempt was. */
static void
gs_apply_settings( int force )
{
  size_t wanted = ( 512 << option_enumerate_peripherals_sound_general_sound_ram() )
		  / GS_PAGE_KB;

  if( !force &&
      gs_loaded_ram_pages == wanted &&
      gs_same_path( gs_loaded_rom, settings_current.rom_general_sound ) )
    return;

  libspectrum_free( gs_loaded_rom );
  gs_loaded_rom = utils_safe_strdup( settings_current.rom_general_sound );
  gs_loaded_ram_pages = wanted;

  gs_available = !gs_allocate();
  if( gs_available ) {
    gs_z80_reset_clock();
    gs_frame_base = 0;
    gs_frame_remainder = 0;
    gs_reset_card();
  }
}

static void
gs_activate( void )
{
  /* A machine reset deactivates and reactivates every peripheral, so this
     runs far more often than the user fitting the card. A card that is up
     stays as it is; one with no ROM tries again. */
  gs_apply_settings( !gs_available );
}

/* Read the ROM image and size the RAM. Returns non-zero on failure, having
   already told the user why; the card then stays inert rather than
   pretending to work. */
static int
gs_allocate( void )
{
  utils_file rom;
  int error;

  error = utils_read_auxiliary_file( settings_current.rom_general_sound, &rom,
				     UTILS_AUXILIARY_ROM );
  if( error == -1 ) {
    ui_error( UI_ERROR_ERROR, "General Sound: couldn't find ROM '%s'",
	      settings_current.rom_general_sound );
    return 1;
  }
  if( error ) return 1;

  if( rom.length != GS_ROM_SIZE ) {
    ui_error( UI_ERROR_ERROR,
	      "General Sound: ROM '%s' is %lu bytes long; expected %lu bytes",
	      settings_current.rom_general_sound,
	      (unsigned long)rom.length, (unsigned long)GS_ROM_SIZE );
    utils_close_file( &rom );
    return 1;
  }

  libspectrum_free( gs_rom );
  gs_rom = libspectrum_new( libspectrum_byte, GS_ROM_SIZE );
  memcpy( gs_rom, rom.buffer, GS_ROM_SIZE );
  utils_close_file( &rom );

  /* 512 KB, 1 MB, 2 MB or 4 MB, per the combo's index */
  gs_ram_pages = ( 512 << option_enumerate_peripherals_sound_general_sound_ram() )
		 / GS_PAGE_KB;
  gs_ram_page_mask = ( gs_ram_pages - 1 ) & 0xff;

  libspectrum_free( gs_ram );
  gs_ram = libspectrum_new0( libspectrum_byte, gs_ram_pages * GS_PAGE_SIZE );

  return 0;
}

/* Bank 1 always holds RAM page 3. GSCFG0 bit 0 swaps the other three
   between ROM and RAM wholesale; bit 1 then write-protects RAM pages 0 and
   1 wherever they are mapped. */
static void
gs_update_mem_mapping( void )
{
  if( gs_cfg0 & GS_CFG0_NOROM ) {
    libspectrum_byte page2 = gs_page & gs_ram_page_mask;
    libspectrum_byte page3 = gs_page_ext & gs_ram_page_mask;

    general_sound_bank_read[0] = general_sound_bank_write[0] = gs_ram;
    general_sound_bank_read[1] = general_sound_bank_write[1] = gs_ram + 3 * GS_PAGE_SIZE;
    general_sound_bank_read[2] = general_sound_bank_write[2] = gs_ram + page2 * GS_PAGE_SIZE;
    general_sound_bank_read[3] = general_sound_bank_write[3] = gs_ram + page3 * GS_PAGE_SIZE;

    if( gs_cfg0 & GS_CFG0_RAMRO ) {
      if( page2 == 0 || page2 == 1 ) general_sound_bank_write[2] = NULL;
      if( page3 == 0 || page3 == 1 ) general_sound_bank_write[3] = NULL;
    }
  } else {
    general_sound_bank_read[0] = gs_rom;
    general_sound_bank_read[1] = gs_ram + 3 * GS_PAGE_SIZE;
    general_sound_bank_read[2] = gs_rom + ( gs_page & GS_ROM_PAGE_MASK ) * GS_PAGE_SIZE;
    general_sound_bank_read[3] = gs_rom + ( gs_page_ext & GS_ROM_PAGE_MASK ) * GS_PAGE_SIZE;

    general_sound_bank_write[0] = NULL;
    general_sound_bank_write[1] = general_sound_bank_read[1];
    general_sound_bank_write[2] = NULL;
    general_sound_bank_write[3] = NULL;
  }
}

static void
gs_reset_card( void )
{
  gs_fitted = settings_current.general_sound;

  gs_status = 0;
  gs_command = 0;
  gs_data_to_host = 0;
  gs_data_from_host = 0;

  if( !gs_available ) return;

  /* Bank 3 comes up on ROM page 1, so extended paging starts at 1 */
  gs_page = 0;
  gs_page_ext = 1;
  gs_cfg0 = 0;
  memset( gs_volume, 0, sizeof( gs_volume ) );
  memset( gs_dac, 0x80, sizeof( gs_dac ) );
  gs_update_mem_mapping();
  gs_z80_reset();

  /* Return the output to silence rather than leaving the last level
     standing in the mixer for as long as the card is stopped. */
  gs_mix();
}

/* The module system's reset hook. The card's own reset is separate so that
   internal callers reach it without the ignored argument. */
static void
gs_reset( int hard_reset GCC_UNUSED )
{
  gs_reset_card();
  gs_resync_sound();
}

/* Fitting or removing the card changes whether the output has a right
   channel, which sound_init settles once. There is no deactivation hook,
   so this runs from the module reset that follows either change. */
static void
gs_resync_sound( void )
{
  if( !sound_layout_stale() ) return;

  sound_end();
  sound_init( settings_current.sound_device );
}

/* Channels 0 and 1 feed the left output, 2 and 3 the right, and each side
   is then mixed into the other at half level. The samples go in at the
   host T-state the card has reached, which is where they belong in the
   output buffer. */
static void
gs_mix( void )
{
  int v[4], l, r, i;
  libspectrum_dword at;

  for( i = 0; i < 4; i++ )
    v[i] = ( (int)gs_dac[i] - 0x80 ) * gs_volume[i];

  l = ( ( v[0] + v[1] ) + ( v[2] + v[3] ) / 2 ) / 2;
  r = ( ( v[2] + v[3] ) + ( v[0] + v[1] ) / 2 ) / 2;

  at = (libspectrum_dword)
       ( ( gs_z80_now() - gs_frame_base ) *
         machine_current->timings.processor_speed / GS_Z80_CLOCK_SPEED );

  /* A whole instruction can carry the card past the point the host has
     reached; the samples belong no later than the frame it is in. */
  if( at >= machine_current->timings.tstates_per_frame )
    at = machine_current->timings.tstates_per_frame - 1;

  sound_generalsound_write( at, l * GS_MIX_SCALE, r * GS_MIX_SCALE );
}

/* A span of host T-states as a count of the card's. */
static libspectrum_qword
gs_host_to_card( libspectrum_dword host_tstates )
{
  return ( (libspectrum_qword)host_tstates * GS_Z80_CLOCK_SPEED ) /
	 machine_current->timings.processor_speed;
}

/* Bring the card up to the host's current T-state. Its 24MHz clock is a
   fixed ratio of the host's, so the conversion is exact arithmetic on the
   frame-relative offset. */
static void
gs_flush( void )
{
  if( !gs_available || !periph_is_active( PERIPH_TYPE_GENERAL_SOUND ) ) return;

  gs_z80_run( gs_frame_base + gs_host_to_card( tstates ) );
}

/* Called at the end of each host frame, before the host rebases its own
   T-state count. */
void
general_sound_frame( libspectrum_dword frame_length )
{
  libspectrum_qword scaled;

  if( !periph_is_active( PERIPH_TYPE_GENERAL_SOUND ) ) return;

  /* Preferences can change the ROM or the RAM size without the card being
     switched off and on again, and that is not an activation edge. */
  gs_apply_settings( 0 );

  if( !gs_available ) return;

  gs_flush();

  /* Advance by exactly what the host is about to subtract from its own
     T-state count, which is not the nominal frame under RZX playback. The
     division does not always come out whole, so what is left of it is
     carried rather than dropped a frame at a time. */
  scaled = (libspectrum_qword)frame_length * GS_Z80_CLOCK_SPEED +
	   gs_frame_remainder;
  gs_frame_base += scaled / machine_current->timings.processor_speed;
  gs_frame_remainder = scaled % machine_current->timings.processor_speed;
}

static libspectrum_byte
gs_read( libspectrum_word port, libspectrum_byte *attached )
{
  if( !gs_available ) return 0xff;

  *attached = 0xff;

  gs_flush();

  if( port & 0x08 ) {		/* 0xbb: status */
    return gs_status | GS_STATUS_UNDRIVEN;
  }

  /* 0xb3: take the byte the card left for us */
  gs_status &= ~GS_STATUS_DATA;
  return gs_data_to_host;
}

static void
gs_write( libspectrum_word port, libspectrum_byte val )
{
  if( !gs_available ) return;

  gs_flush();

  if( port & 0x08 ) {		/* 0xbb: command */
    gs_command = val;
    gs_status |= GS_STATUS_COMMAND;
  } else {			/* 0xb3: data */
    gs_data_from_host = val;
    gs_status |= GS_STATUS_DATA;
  }
}

static void
gs_control_write( libspectrum_word port GCC_UNUSED,
			     libspectrum_byte val )
{
  if( !gs_available ) return;

  /* As on the card, the reset and NMI lines act before it is run on to
     the current host T-state. */
  if( val & 0x80 ) {
    gs_reset_card();
    gs_flush();
    return;
  }

  if( val & 0x40 ) {
    gs_z80_nmi();
    gs_flush();
    return;
  }
}

static void
gs_enabled_snapshot( libspectrum_snap *snap GCC_UNUSED )
{
  settings_current.general_sound = gs_fitted;
}

/* The DACs are not written through a port: the card reads its sample
   bytes out of 0x6000-0x7fff, and address bits 9 and 8 pick the channel.
   Called for every read in that window, instruction fetches included. */
void
general_sound_dac_read( libspectrum_word address, libspectrum_byte b )
{
  gs_dac[ ( address >> 8 ) & 3 ] = b;
  gs_mix();
}

/* Re-anchor the card's frame origin on the host's T-state count, for the
   points where the host resets it outside a frame boundary. The card keeps
   the time it has reached; only the origin moves, so the count the host has
   just adopted maps to where the card already is. */
void
general_sound_reanchor( void )
{
  libspectrum_qword now, offset;

  if( !gs_available ) return;

  now = gs_z80_now();
  offset = gs_host_to_card( tstates );

  gs_frame_base = now > offset ? now - offset : 0;
  gs_frame_remainder = 0;
}

/* --- the card's internal I/O map ------------------------------------- */

/* Ports as seen by the card's own Z80. Ports 0x01-0x05 are the other end
   of the host handshake on 0xb3/0xbb; 0x06-0x09 are the channel volumes;
   0x00 and 0x10 page memory; 0x0f is the configuration register.
   Unimplemented ports read back as 0xff. */
#define GS_PORT_MPAG   0x00
#define GS_PORT_CMD    0x01
#define GS_PORT_DATA   0x02
#define GS_PORT_PUT    0x03
#define GS_PORT_STAT   0x04
#define GS_PORT_CLRCMD 0x05
#define GS_PORT_VOL1   0x06
#define GS_PORT_VOL4   0x09
#define GS_PORT_PGSTAT 0x0a
#define GS_PORT_VOLBIT 0x0b
#define GS_PORT_CFG0   0x0f
#define GS_PORT_MPAGEX 0x10

libspectrum_byte
general_sound_port_read( libspectrum_word port )
{
  switch( port & 0xff ) {

  case GS_PORT_CMD:
    return gs_command;

  case GS_PORT_DATA:
    gs_status &= ~GS_STATUS_DATA;
    return gs_data_from_host;

  case GS_PORT_PUT:
    gs_status |= GS_STATUS_DATA;
    gs_data_to_host = 0xff;
    return 0xff;

  case GS_PORT_STAT:
    return gs_status;

  case GS_PORT_CLRCMD:
    gs_status &= ~GS_STATUS_COMMAND;
    return 0xff;

  /* The card reads its own paging and volume bits back through the
     status register rather than on the data bus. */
  case GS_PORT_PGSTAT:
    gs_status = ( gs_status & ~GS_STATUS_DATA ) |
		( ( gs_page & 1 ) ? GS_STATUS_DATA : 0 );
    return 0xff;

  case GS_PORT_VOLBIT:
    gs_status = ( gs_status & ~GS_STATUS_COMMAND ) |
		( ( gs_volume[0] >> 5 ) & 1 );
    return 0xff;

  case GS_PORT_CFG0:
    return gs_cfg0;

  }

  return 0xff;
}

void
general_sound_port_write( libspectrum_word port, libspectrum_byte b )
{
  switch( port & 0xff ) {

  case GS_PORT_MPAG:
  {
    libspectrum_byte rotated = GS_ROL8( b ) & gs_ram_page_mask;

    /* Outside extended paging one write sets both switchable banks: an
       even page below, the odd page above it. */
    if( gs_cfg0 & GS_CFG0_EXPAG ) {
      gs_page = rotated;
    } else {
      gs_page = rotated & 0xfe;
      gs_page_ext = rotated | 1;
    }
    gs_update_mem_mapping();
    return;
  }

  case GS_PORT_DATA:
    gs_status &= ~GS_STATUS_DATA;
    return;

  case GS_PORT_PUT:
    gs_status |= GS_STATUS_DATA;
    gs_data_to_host = b;
    return;

  case GS_PORT_CLRCMD:
    gs_status &= ~GS_STATUS_COMMAND;
    return;

  case GS_PORT_PGSTAT:
    gs_status = ( gs_status & ~GS_STATUS_DATA ) |
		( ( gs_page & 1 ) ? GS_STATUS_DATA : 0 );
    return;

  case GS_PORT_VOLBIT:
    gs_status = ( gs_status & ~GS_STATUS_COMMAND ) |
		( ( gs_volume[0] >> 5 ) & 1 );
    return;

  case GS_PORT_CFG0:
    gs_cfg0 = b & 0x3f & ~GS_CFG0_8CHANS;
    gs_update_mem_mapping();
    return;

  case GS_PORT_MPAGEX:
    gs_page_ext = GS_ROL8( b ) & gs_ram_page_mask;
    gs_update_mem_mapping();
    return;

  }

  if( ( port & 0xff ) >= GS_PORT_VOL1 && ( port & 0xff ) <= GS_PORT_VOL4 ) {
    gs_volume[ ( port & 0xff ) - GS_PORT_VOL1 ] = b & 0x3f;
    gs_mix();
  }
}
