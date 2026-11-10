/* tape_block.c: tape block presentation routines
   Copyright (c) 1999-2017 Philip Kendall, Darren Salt, Witold Filipczyk
   Copyright (c) 2015-2018 UB880D
   Copyright (c) 2016-2026 Fredrick Meunier

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

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "libspectrum.h"

#include "fuse.h"
#include "tape.h"

/* Length of a standard ZX Spectrum ROM tape header block (flag byte +
   1 type + 10 name + 2 length + 2 param1 + 2 param2 + 1 parity = 19 bytes) */
#define TAPE_ROM_HEADER_LEN 19

/* Flag byte for a ZX Spectrum ROM tape header block */
#define TAPE_ROM_HEADER_FLAG 0x00

typedef void (*tape_block_formatter)( char *buffer, size_t length,
                                     libspectrum_tape_block *block );

typedef struct tape_block_format_entry {
  libspectrum_tape_type type;
  tape_block_formatter formatter;
} tape_block_format_entry;

static void
format_byte_count( char *buffer, size_t length,
                   libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%zu bytes",
            libspectrum_tape_block_data_length( block ) );
}

static const char *
rom_header_type( libspectrum_byte type )
{
  static const char * const types[] = {
    "Program", "Number array", "Character array", "Bytes"
  };

  return type < 4 ? types[ type ] : NULL;
}

static void
format_rom_block( char *buffer, size_t length,
                  libspectrum_tape_block *block )
{
  libspectrum_byte *data;
  const char *type;
  char name[ 10 * 9 + 1 ];

  if( libspectrum_tape_block_data_length( block ) != TAPE_ROM_HEADER_LEN ) {
    format_byte_count( buffer, length, block );
    return;
  }

  data = libspectrum_tape_block_data( block );
  if( data[0] != TAPE_ROM_HEADER_FLAG ) {
    format_byte_count( buffer, length, block );
    return;
  }

  type = rom_header_type( data[1] );
  if( !type ||
      libspectrum_zx_string_to_utf8( name, sizeof( name ), &data[2], 10 ) ) {
    format_byte_count( buffer, length, block );
    return;
  }

  snprintf( buffer, length, "%s: \"%s\"", type, name );
}

static void
format_tone( char *buffer, size_t length, libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%" PRIu32 " tstates",
            libspectrum_tape_block_pulse_length( block ) );
}

static void
format_pulses( char *buffer, size_t length, libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%zu pulses",
            libspectrum_tape_block_count( block ) );
}

/* RLE pulse data uses the same encoding as CSW/TZX RLE streams: a pulse of
   n samples is stored as a single byte if n <= 255, or as a zero marker
   followed by a little-endian dword when n > 255. */
static void
format_rle_pulse( char *buffer, size_t length,
                  libspectrum_tape_block *block )
{
  libspectrum_byte *data = libspectrum_tape_block_data( block );
  size_t i, data_length = libspectrum_tape_block_data_length( block );
  unsigned long pulses = 0;

  if( data && data_length ) {
    for( i = 0; i < data_length; i++ ) {
      if( data[ i ] ) {
        pulses++;                       /* one byte: pulse <= 255 samples */
      } else {
        i += 4;                         /* marker byte plus dword length */
        pulses++;
      }
    }
  }

  /* Mirror the TZX CSW recording listing: report an exact stored sample
     rate when the block carries one, and just the pulse count otherwise. */
  if( libspectrum_tape_block_sample_rate( block ) ) {
    snprintf( buffer, length, "%lu pulses, %" PRIu32 " Hz", pulses,
              libspectrum_tape_block_sample_rate( block ) );
  } else {
    snprintf( buffer, length, "%lu pulses", pulses );
  }
}

static void
format_tzx_csw( char *buffer, size_t length, libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%" PRIu32 " pulses, %" PRIu32 " Hz",
            libspectrum_tape_block_csw_pulses( block ),
            libspectrum_tape_block_sample_rate( block ) );
}

static void
format_pulse_sequence( char *buffer, size_t length,
                       libspectrum_tape_block *block )
{
  size_t i;
  unsigned long total_pulses = 0;

  for( i = 0; i < libspectrum_tape_block_count( block ); i++ )
    total_pulses += libspectrum_tape_block_pulse_repeats( block, i );

  snprintf( buffer, length, "%lu pulses", total_pulses );
}

static void
format_pause( char *buffer, size_t length, libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%" PRIu32 " ms",
            libspectrum_tape_block_pause( block ) );
}

static void
format_text( char *buffer, size_t length, libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%s", libspectrum_tape_block_text( block ) );
}

static void
format_jump( char *buffer, size_t length, libspectrum_tape_block *block )
{
  int offset = libspectrum_tape_block_offset( block );

  if( offset > 0 )
    snprintf( buffer, length, "Forward %d blocks", offset );
  else
    snprintf( buffer, length, "Backward %d blocks", -offset );
}

static void
format_iterations( char *buffer, size_t length,
                   libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%zu iterations",
            libspectrum_tape_block_count( block ) );
}

static void
format_options( char *buffer, size_t length, libspectrum_tape_block *block )
{
  snprintf( buffer, length, "%zu options",
            libspectrum_tape_block_count( block ) );
}

static void
format_data_symbols( char *buffer, size_t length,
                     libspectrum_tape_block *block )
{
  libspectrum_dword symbols =
    libspectrum_tape_generalised_data_symbol_table_symbols_in_block(
      libspectrum_tape_block_data_table( block ) );

  snprintf( buffer, length, "%" PRIu32 " data symbols", symbols );
}

static const tape_block_format_entry formatters[] = {
  { LIBSPECTRUM_TAPE_BLOCK_ROM, format_rom_block },
  { LIBSPECTRUM_TAPE_BLOCK_DATA_BLOCK, format_rom_block },
  { LIBSPECTRUM_TAPE_BLOCK_TURBO, format_byte_count },
  { LIBSPECTRUM_TAPE_BLOCK_PURE_DATA, format_byte_count },
  { LIBSPECTRUM_TAPE_BLOCK_RAW_DATA, format_byte_count },
  { LIBSPECTRUM_TAPE_BLOCK_PURE_TONE, format_tone },
  { LIBSPECTRUM_TAPE_BLOCK_PULSES, format_pulses },
  { LIBSPECTRUM_TAPE_BLOCK_PULSE_SEQUENCE, format_pulse_sequence },
  { LIBSPECTRUM_TAPE_BLOCK_PAUSE, format_pause },
  { LIBSPECTRUM_TAPE_BLOCK_GROUP_START, format_text },
  { LIBSPECTRUM_TAPE_BLOCK_COMMENT, format_text },
  { LIBSPECTRUM_TAPE_BLOCK_MESSAGE, format_text },
  { LIBSPECTRUM_TAPE_BLOCK_CUSTOM, format_text },
  { LIBSPECTRUM_TAPE_BLOCK_JUMP, format_jump },
  { LIBSPECTRUM_TAPE_BLOCK_LOOP_START, format_iterations },
  { LIBSPECTRUM_TAPE_BLOCK_SELECT, format_options },
  { LIBSPECTRUM_TAPE_BLOCK_GENERALISED_DATA, format_data_symbols },
  { LIBSPECTRUM_TAPE_BLOCK_RLE_PULSE, format_rle_pulse },
  { LIBSPECTRUM_TAPE_BLOCK_TZX_CSW, format_tzx_csw },
};

int
tape_block_details( char **details, libspectrum_tape_block *block )
{
  libspectrum_tape_type type = libspectrum_tape_block_type( block );
  /* Bounded descriptions only: ROM names expand to at most 90 UTF-8 bytes
     plus a type label and quotes; numeric descriptions are shorter. */
  char buffer[128] = "";
  const char *text = buffer;

  for( size_t i = 0; i < ARRAY_SIZE( formatters ); i++ ) {
    if( formatters[i].type == type ) {
      if( formatters[i].formatter == format_text )
        text = libspectrum_tape_block_text( block );
      else
        formatters[i].formatter( buffer, sizeof( buffer ), block );
      break;
    }
  }

  if( !text ) text = "";
  *details = libspectrum_new( char, strlen( text ) + 1 );
  strcpy( *details, text );
  return 0;
}
