/* loader_detector.c: loader byte-pattern detection
   Copyright (c) 2006-2018 Philip Kendall

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

#include "loader_internals.h"
#include "memory_pages.h"
#include "z80/z80.h"

static int
loader_pattern_byte_matches( libspectrum_byte byte,
                             const loader_pattern_byte_t *pattern )
{
  if( !pattern->alternatives ) return 1;

  for( size_t i = 0; i < pattern->alternatives; i++ )
    if( ( byte & pattern->masks[ i ] ) == pattern->values[ i ] ) return 1;

  return 0;
}

int
loader_pattern_matches( libspectrum_word address,
                        const loader_pattern_byte_t *pattern, size_t length )
{
  for( size_t i = 0; i < length; i++ )
    if( !loader_pattern_byte_matches( readbyte_internal( address + i ),
                                      &pattern[ i ] ) ) return 0;

  return 1;
}

int
loader_word_matches( libspectrum_word address, libspectrum_word value )
{
  return readbyte_internal( address ) == value % 0x100 &&
         readbyte_internal( address + 1 ) == value / 0x100;
}

#define LOADER_PATTERN_LENGTH( pattern ) \
  ( sizeof( pattern ) / sizeof( pattern[ 0 ] ) )

#define ROM_LOADER_PREFIX \
  LOADER_PATTERN_BYTE( 0x04 ), LOADER_PATTERN_BYTE( 0xc8 ), \
  LOADER_PATTERN_BYTE( 0x3e ), \
  LOADER_PATTERN_ONE_OF_3( 0x00, 0x7f, 0xff )
#define ROM_LOADER_INPUT \
  LOADER_PATTERN_BYTE( 0xdb ), LOADER_PATTERN_BYTE( 0xfe )
#define ROM_LOADER_EDGE_TEST \
  LOADER_PATTERN_BYTE( 0x1f )
#define ROM_LOADER_LEVEL_TEST \
  LOADER_PATTERN_BYTE( 0xa9 ), LOADER_PATTERN_BYTE( 0xe6 ), \
  LOADER_PATTERN_BYTE( 0x20 ), LOADER_PATTERN_BYTE( 0x28 )
#define ROM_LOADER_EDGE_ACTION \
  LOADER_PATTERN_ONE_OF_4( 0x00, 0xa7, 0xc8, 0xd0 )

static const loader_pattern_byte_t rom_loader[] = {
  ROM_LOADER_PREFIX, ROM_LOADER_INPUT, ROM_LOADER_EDGE_TEST,
  ROM_LOADER_LEVEL_TEST, LOADER_PATTERN_BYTE( 0xf4 ),
};

static const loader_pattern_byte_t rom_loader_with_edge_action[] = {
  ROM_LOADER_PREFIX, ROM_LOADER_INPUT, ROM_LOADER_EDGE_TEST,
  ROM_LOADER_EDGE_ACTION, ROM_LOADER_LEVEL_TEST, LOADER_PATTERN_BYTE( 0xf3 ),
};

#define MICROPROSE_PREFIX \
  ROM_LOADER_PREFIX, LOADER_PATTERN_BYTE( 0x3e ), \
  LOADER_PATTERN_BYTE( 0x7f ), ROM_LOADER_INPUT, ROM_LOADER_EDGE_TEST

static const loader_pattern_byte_t microprose_loader[] = {
  MICROPROSE_PREFIX, ROM_LOADER_LEVEL_TEST, LOADER_PATTERN_BYTE( 0xf2 ),
};

static const loader_pattern_byte_t microprose_loader_with_edge_action[] = {
  MICROPROSE_PREFIX, ROM_LOADER_EDGE_ACTION, ROM_LOADER_LEVEL_TEST,
  LOADER_PATTERN_BYTE( 0xf1 ),
};

static const loader_pattern_byte_t search_loader[] = {
  ROM_LOADER_PREFIX, ROM_LOADER_INPUT, LOADER_PATTERN_BYTE( 0xa9 ),
  LOADER_PATTERN_BYTE( 0xe6 ), LOADER_PATTERN_BYTE( 0x40 ),
  LOADER_PATTERN_BYTE( 0x28 ), LOADER_PATTERN_BYTE( 0xf5 ),
};

static const loader_pattern_byte_t search_loader_with_ret[] = {
  ROM_LOADER_PREFIX, ROM_LOADER_INPUT, LOADER_PATTERN_BYTE( 0xa9 ),
  LOADER_PATTERN_BYTE( 0xe6 ), LOADER_PATTERN_BYTE( 0x40 ),
  LOADER_PATTERN_BYTE( 0xd8 ), LOADER_PATTERN_BYTE( 0x00 ),
  LOADER_PATTERN_BYTE( 0x28 ), LOADER_PATTERN_BYTE( 0xf3 ),
};

#define ALKATRAZ_SUFFIX \
  ROM_LOADER_INPUT, ROM_LOADER_EDGE_TEST, LOADER_PATTERN_BYTE( 0xc8 ), \
  ROM_LOADER_LEVEL_TEST, LOADER_PATTERN_ONE_OF_2( 0xf1, 0xf3 )

static const loader_pattern_byte_t alkatraz_loader[] = {
  LOADER_PATTERN_BYTE( 0x03 ), LOADER_PATTERN_BYTE( 0xc3 ),
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY, ALKATRAZ_SUFFIX,
};

static const loader_pattern_byte_t alkatraz_variant_loader[] = {
  LOADER_PATTERN_BYTE( 0x04 ), LOADER_PATTERN_BYTE( 0x20 ),
  LOADER_PATTERN_BYTE( 0x01 ), LOADER_PATTERN_BYTE( 0xc9 ),
  ALKATRAZ_SUFFIX,
};

static const loader_pattern_byte_t software_projects_loader[] = {
  LOADER_PATTERN_BYTE( 0x47 ), LOADER_PATTERN_BYTE( 0x08 ),
  LOADER_PATTERN_BYTE( 0x3e ), LOADER_PATTERN_BYTE( 0x7f ),
  ROM_LOADER_INPUT, LOADER_PATTERN_BYTE( 0xa9 ),
  LOADER_PATTERN_BYTE( 0xe6 ), LOADER_PATTERN_BYTE( 0x40 ),
  LOADER_PATTERN_BYTE( 0x20 ), LOADER_PATTERN_BYTE( 0x04 ),
  LOADER_PATTERN_BYTE( 0x05 ), LOADER_PATTERN_BYTE( 0x20 ),
  LOADER_PATTERN_BYTE( 0xf4 ),
};

static const loader_pattern_byte_t digital_integration_loader[] = {
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY, LOADER_PATTERN_BYTE( 0x05 ),
  LOADER_PATTERN_BYTE( 0xc8 ), ROM_LOADER_INPUT,
  LOADER_PATTERN_BYTE( 0xa9 ), LOADER_PATTERN_BYTE( 0xe6 ),
  LOADER_PATTERN_BYTE( 0x40 ), LOADER_PATTERN_BYTE( 0xca ),
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY,
};

typedef struct acceleration_pattern_t {
  const loader_pattern_byte_t *bytes;
  size_t length;
  acceleration_mode_t mode;
} acceleration_pattern_t;

#define ACCELERATION_PATTERN( pattern, acceleration_mode ) \
  { pattern, LOADER_PATTERN_LENGTH( pattern ), acceleration_mode }

static const acceleration_pattern_t acceleration_patterns[] = {
  ACCELERATION_PATTERN( rom_loader, ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( rom_loader_with_edge_action,
                        ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( microprose_loader, ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( microprose_loader_with_edge_action,
                        ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( search_loader, ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( search_loader_with_ret,
                        ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( alkatraz_loader, ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( alkatraz_variant_loader,
                        ACCELERATION_MODE_INCREASING ),
  ACCELERATION_PATTERN( software_projects_loader,
                        ACCELERATION_MODE_SOFTWARE_PROJECTS ),
};

static int
digital_integration_loader_matches( libspectrum_word pc )
{
  libspectrum_byte first = readbyte_internal( pc );

  /* These bytes select other state-machine branches in the original
     detector, so they cannot begin a Digital Integration signature. */
  if( first == 0x03 || first == 0x04 || first == 0x47 ) return 0;

  if( !loader_pattern_matches( pc, digital_integration_loader,
                               LOADER_PATTERN_LENGTH(
                                 digital_integration_loader ) ) ) return 0;

  return loader_word_matches( pc + 10, z80.pc.w - 4 );
}

acceleration_mode_t
acceleration_detector( libspectrum_word pc )
{
  if( digital_integration_loader_matches( pc ) )
    return ACCELERATION_MODE_DECREASING;

  for( size_t i = 0; i < LOADER_PATTERN_LENGTH( acceleration_patterns ); i++ )
    if( loader_pattern_matches( pc, acceleration_patterns[ i ].bytes,
                                acceleration_patterns[ i ].length ) )
      return acceleration_patterns[ i ].mode;

  return ACCELERATION_MODE_NONE;
}

static const loader_pattern_byte_t gremlin_rising_loader[] = {
  LOADER_PATTERN_BYTE( 0x2e ), LOADER_PATTERN_BYTE( 0x00 ),
  LOADER_PATTERN_BYTE( 0x2c ), ROM_LOADER_INPUT,
  LOADER_PATTERN_BYTE( 0xa4 ), LOADER_PATTERN_BYTE( 0xca ),
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY,
};

static const loader_pattern_byte_t gremlin_falling_loader[] = {
  LOADER_PATTERN_BYTE( 0x3e ), LOADER_PATTERN_BYTE( 0x08 ),
  LOADER_PATTERN_BYTE( 0xd3 ), LOADER_PATTERN_BYTE( 0xfe ),
  LOADER_PATTERN_BYTE( 0x2c ), ROM_LOADER_INPUT,
  LOADER_PATTERN_BYTE( 0xa4 ), LOADER_PATTERN_BYTE( 0xc2 ),
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY,
};

static acceleration_mode_t
gremlin_acceleration_detector( libspectrum_word pc )
{
  /* Rising edge: LD L,0; INC L; IN A,(FE); AND H; JP Z,<INC L>. */
  if( loader_pattern_matches( pc - 5, gremlin_rising_loader,
                              LOADER_PATTERN_LENGTH(
                                gremlin_rising_loader ) ) &&
      loader_word_matches( pc + 2, pc - 3 ) )
    return ACCELERATION_MODE_GREMLIN_RISING;

  /* Falling edge: LD A,8; OUT (FE),A; INC L; IN A,(FE); AND H;
     JP NZ,<INC L>. */
  if( loader_pattern_matches( pc - 7, gremlin_falling_loader,
                              LOADER_PATTERN_LENGTH(
                                gremlin_falling_loader ) ) &&
      loader_word_matches( pc + 2, pc - 3 ) )
    return ACCELERATION_MODE_GREMLIN_FALLING;

  return ACCELERATION_MODE_NONE;
}

static const loader_pattern_byte_t modified_rom_prefix[] = {
  LOADER_PATTERN_BYTE( 0xd9 ),             /* EXX */
  LOADER_PATTERN_BYTE( 0x11 ),             /* LD DE,nn */
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY,
  LOADER_PATTERN_BYTE( 0x21 ),             /* LD HL,nn */
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY,
  LOADER_PATTERN_BYTE( 0x06 ), LOADER_PATTERN_BYTE( 0x05 ), /* LD B,5 */
  LOADER_PATTERN_BYTE( 0xcd ),             /* CALL callback */
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY,
  LOADER_PATTERN_BYTE( 0xd0 ),             /* RET NC */
  LOADER_PATTERN_BYTE( 0xd9 ),             /* EXX */
};

static const loader_pattern_byte_t modified_rom_edge2_prefix[] = {
  LOADER_PATTERN_BYTE( 0xd9 ),             /* EXX */
  LOADER_PATTERN_BYTE( 0xa7 ),             /* AND A */
};

int
modified_rom_loader_detector( libspectrum_word pc )
{
  /* Technician Ted replaces the ROM EDGE1 delay with useful work, entered
     with EXX and left immediately before the copied EDGE2 sampling loop. The
     work and the real sampling delay form one timing unit, so short-circuiting
     only EDGE2 corrupts the load. Keep the loop as strong autoplay evidence,
     but do not accelerate it. */
  return loader_pattern_matches( pc - 40, modified_rom_prefix,
                                 LOADER_PATTERN_LENGTH(
                                   modified_rom_prefix ) ) &&
         loader_word_matches( pc - 30, pc - 24 ) &&
         loader_pattern_matches( pc - 8, modified_rom_edge2_prefix,
                                 LOADER_PATTERN_LENGTH(
                                   modified_rom_edge2_prefix ) ) &&
         acceleration_detector( pc - 6 ) == ACCELERATION_MODE_INCREASING;
}

acceleration_mode_t
acceleration_detector_at_in( libspectrum_word pc )
{
  acceleration_mode_t mode;

  if( modified_rom_loader_detector( pc ) ) return ACCELERATION_MODE_NONE;

  mode = gremlin_acceleration_detector( pc );
  if( mode ) return mode;

  mode = acceleration_detector( pc - 6 );
  /* Microprose inserts another LD A,0x7f before the IN instruction */
  if( !mode ) mode = acceleration_detector( pc - 8 );

  return mode;
}

static const loader_pattern_byte_t movieload_loader[] = {
  LOADER_PATTERN_BYTE( 0x14 ), LOADER_PATTERN_BYTE( 0xc8 ),
  LOADER_PATTERN_BYTE( 0x3e ), LOADER_PATTERN_BYTE( 0x7f ),
  ROM_LOADER_INPUT, LOADER_PATTERN_BYTE( 0x1f ),
  LOADER_PATTERN_BYTE( 0x00 ), LOADER_PATTERN_BYTE( 0xab ),
  LOADER_PATTERN_BYTE( 0xe6 ), LOADER_PATTERN_BYTE( 0x20 ),
  LOADER_PATTERN_BYTE( 0x28 ), LOADER_PATTERN_BYTE( 0xf3 ),
};

int
movieload_loader_detector( libspectrum_word pc )
{
  /* INC D; RET Z; LD A,7f; IN A,(fe); RRA; NOP; XOR E;
     AND 20; JR Z,<INC D>. Movieload uses D rather than B as its counter. */
  return loader_pattern_matches( pc - 6, movieload_loader,
                                 LOADER_PATTERN_LENGTH( movieload_loader ) );
}

static const loader_pattern_byte_t sign_flag_loader[] = {
  LOADER_PATTERN_BYTE( 0x04 ), LOADER_PATTERN_BYTE( 0xc8 ),
  ROM_LOADER_INPUT, LOADER_PATTERN_BYTE( 0x87 ),
  /* JP P (f2) and JP M (fa) differ only in bit 3. */
  LOADER_PATTERN_MASKED( 0xf2, 0xf7 ),
  LOADER_PATTERN_ANY, LOADER_PATTERN_ANY,
};

int
sign_flag_loader_detector( libspectrum_word pc )
{
  /* INC B; RET Z; IN A,(fe); ADD A,A; JP P/M,<INC B>. The loader tests
     EAR by moving bit 6 into the sign flag. */
  return loader_pattern_matches( pc - 4, sign_flag_loader,
                                 LOADER_PATTERN_LENGTH( sign_flag_loader ) ) &&
         loader_word_matches( pc + 2, pc - 4 );
}

int
loader_loop_detector( libspectrum_word pc )
{
  return modified_rom_loader_detector( pc ) ||
         acceleration_detector_at_in( pc ) != ACCELERATION_MODE_NONE ||
         sign_flag_loader_detector( pc );
}

int
ula_read_is_keyboard_scan( libspectrum_word pc )
{
  /* ROM-style keyboard scanning discards EAR and keeps only the five
     keyboard bits. Match instructions at the post-IN PC, not a ROM address,
     so copied routines and replacement ROMs are covered too. */
  return readbyte_internal( pc ) == 0x2f &&       /* CPL */
         readbyte_internal( pc + 1 ) == 0xe6 && /* AND 1f */
         readbyte_internal( pc + 2 ) == 0x1f;
}

int
ula_read_uses_ear( libspectrum_word pc )
{
  int i;

  /* A loader normally either tests EAR directly with bit 6, or rotates it
     into bit 5 first. Requiring that use prevents keyboard scans from
     satisfying the fallback timing heuristic. */
  if( readbyte_internal( pc ) == 0x1f ) {   /* RRA */
    for( i = 1; i < 6; i++ )
      if( readbyte_internal( pc + i ) == 0xe6 &&
          readbyte_internal( pc + i + 1 ) == 0x20 ) return 1;
  }

  for( i = 0; i < 5; i++ )
    if( readbyte_internal( pc + i ) == 0xe6 &&
        readbyte_internal( pc + i + 1 ) == 0x40 ) return 1;

  return 0;
}
