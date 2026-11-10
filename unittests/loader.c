/* loader.c: loader detection unit tests
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

#include <stdio.h>

#include "loader.h"
#include "loader_internals.h"
#include "machine.h"
#include "memory_pages.h"
#include "settings.h"
#include "tape.h"
#include "z80/z80.h"

#define LOADER_TEST_BASE 0x8000
#define LOADER_TEST_MAX_LENGTH 52

typedef struct loader_test_memory_t {
  libspectrum_byte saved[ LOADER_TEST_MAX_LENGTH ];
  size_t length;
} loader_test_memory_t;

typedef struct loader_test_state_t {
  libspectrum_byte b, c, a_, a, f, l;
  libspectrum_word pc, sp;
  size_t acceleration_pc;
  acceleration_mode_t acceleration_mode;
} loader_test_state_t;

static void
loader_test_install( loader_test_memory_t *memory,
                     const libspectrum_byte *bytes, size_t length )
{
  size_t i;

  memory->length = length;
  for( i = 0; i < length; i++ ) {
    memory->saved[ i ] = readbyte_internal( LOADER_TEST_BASE + i );
    writebyte_internal( LOADER_TEST_BASE + i, bytes ? bytes[ i ] : 0x00 );
  }
}

static void
loader_test_restore_memory( const loader_test_memory_t *memory )
{
  size_t i;

  for( i = 0; i < memory->length; i++ )
    writebyte_internal( LOADER_TEST_BASE + i, memory->saved[ i ] );
}

static int
loader_test_pattern_matcher( void )
{
  static const libspectrum_byte bytes[] = { 0xaa, 0x31, 0xfa, 0xcc };
  static const loader_pattern_byte_t pattern[] = {
    LOADER_PATTERN_BYTE( 0xaa ),
    LOADER_PATTERN_ANY,
    /* JP P (f2) and JP M (fa) differ only in bit 3. */
    LOADER_PATTERN_MASKED( 0xf2, 0xf7 ),
    LOADER_PATTERN_BYTE( 0xcc ),
  };
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, bytes, sizeof( bytes ) );
  if( !loader_pattern_matches( LOADER_TEST_BASE, pattern,
                               sizeof( pattern ) / sizeof( pattern[ 0 ] ) ) )
    error++;

  writebyte_internal( LOADER_TEST_BASE, 0xab );
  if( loader_pattern_matches( LOADER_TEST_BASE, pattern,
                              sizeof( pattern ) / sizeof( pattern[ 0 ] ) ) )
    error++;

  writebyte_internal( LOADER_TEST_BASE, 0x34 );
  writebyte_internal( LOADER_TEST_BASE + 1, 0x92 );
  if( !loader_word_matches( LOADER_TEST_BASE, 0x9234 ) ) error++;
  if( loader_word_matches( LOADER_TEST_BASE, 0x9235 ) ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

typedef struct loader_detector_test_case_t {
  libspectrum_byte bytes[ 15 ];
  size_t length;
  size_t reject_offset;
  acceleration_mode_t mode;
} loader_detector_test_case_t;

static int
loader_test_acceleration_patterns( void )
{
  static const loader_detector_test_case_t tests[] = {
    { { 0x04, 0xc8, 0x3e, 0x00, 0xdb, 0xfe, 0x1f, 0xa9, 0xe6, 0x20,
        0x28, 0xf4 }, 12, 9, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0x7f, 0xdb, 0xfe, 0x1f, 0x00, 0xa9, 0xe6,
        0x20, 0x28, 0xf3 }, 13, 7, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0xff, 0xdb, 0xfe, 0x1f, 0xa7, 0xa9, 0xe6,
        0x20, 0x28, 0xf3 }, 13, 7, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0x7f, 0xdb, 0xfe, 0x1f, 0xc8, 0xa9, 0xe6,
        0x20, 0x28, 0xf3 }, 13, 7, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0x7f, 0xdb, 0xfe, 0x1f, 0xd0, 0xa9, 0xe6,
        0x20, 0x28, 0xf3 }, 13, 7, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0x00, 0x3e, 0x7f, 0xdb, 0xfe, 0x1f, 0xa9,
        0xe6, 0x20, 0x28, 0xf2 }, 14, 5, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0xff, 0x3e, 0x7f, 0xdb, 0xfe, 0x1f, 0x00,
        0xa9, 0xe6, 0x20, 0x28, 0xf1 }, 15, 9,
      ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0x00, 0xdb, 0xfe, 0xa9, 0xe6, 0x40, 0x28,
        0xf5 }, 11, 8, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0xc8, 0x3e, 0x7f, 0xdb, 0xfe, 0xa9, 0xe6, 0x40, 0xd8,
        0x00, 0x28, 0xf3 }, 13, 10, ACCELERATION_MODE_INCREASING },
    { { 0x03, 0xc3, 0x34, 0x12, 0xdb, 0xfe, 0x1f, 0xc8, 0xa9, 0xe6,
        0x20, 0x28, 0xf1 }, 13, 7, ACCELERATION_MODE_INCREASING },
    { { 0x03, 0xc3, 0x78, 0x56, 0xdb, 0xfe, 0x1f, 0xc8, 0xa9, 0xe6,
        0x20, 0x28, 0xf3 }, 13, 7, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0x20, 0x01, 0xc9, 0xdb, 0xfe, 0x1f, 0xc8, 0xa9, 0xe6,
        0x20, 0x28, 0xf1 }, 13, 2, ACCELERATION_MODE_INCREASING },
    { { 0x04, 0x20, 0x01, 0xc9, 0xdb, 0xfe, 0x1f, 0xc8, 0xa9, 0xe6,
        0x20, 0x28, 0xf3 }, 13, 2, ACCELERATION_MODE_INCREASING },
    { { 0x47, 0x08, 0x3e, 0x7f, 0xdb, 0xfe, 0xa9, 0xe6, 0x40, 0x20,
        0x04, 0x05, 0x20, 0xf4 }, 14, 10,
      ACCELERATION_MODE_SOFTWARE_PROJECTS },
  };
  int error = 0;

  for( size_t i = 0; i < sizeof( tests ) / sizeof( tests[ 0 ] ); i++ ) {
    loader_test_memory_t memory;
    loader_test_install( &memory, tests[ i ].bytes, tests[ i ].length );
    if( acceleration_detector( LOADER_TEST_BASE ) != tests[ i ].mode ) error++;
    writebyte_internal( LOADER_TEST_BASE + tests[ i ].reject_offset,
                        tests[ i ].bytes[ tests[ i ].reject_offset ] ^ 0x01 );
    if( acceleration_detector( LOADER_TEST_BASE ) != ACCELERATION_MODE_NONE )
      error++;
    loader_test_restore_memory( &memory );
  }

  return error;
}

static int
loader_test_digital_integration( void )
{
  static const libspectrum_byte loader[] = {
    0x00, 0x3f, 0x05, 0xc8, 0xdb, 0xfe, 0xa9, 0xe6, 0x40, 0xca,
    0xfc, 0x7f
  };
  loader_test_memory_t memory;
  libspectrum_word saved_pc = z80.pc.w;
  int error = 0;

  loader_test_install( &memory, loader, sizeof( loader ) );
  z80.pc.w = LOADER_TEST_BASE;
  if( acceleration_detector( LOADER_TEST_BASE ) !=
      ACCELERATION_MODE_DECREASING ) error++;

  /* The absolute branch must target the active sampling loop. */
  writebyte_internal( LOADER_TEST_BASE + 10, 0xfb );
  if( acceleration_detector( LOADER_TEST_BASE ) != ACCELERATION_MODE_NONE )
    error++;
  writebyte_internal( LOADER_TEST_BASE + 10, 0xfc );

  /* Opcodes which selected another original state-machine branch are invalid. */
  writebyte_internal( LOADER_TEST_BASE, 0x04 );
  if( acceleration_detector( LOADER_TEST_BASE ) != ACCELERATION_MODE_NONE )
    error++;

  z80.pc.w = saved_pc;
  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_microprose( void )
{
  static const libspectrum_byte loader[] = {
    0x04, 0xc8, 0x3e, 0x7f, 0x3e, 0x7f, 0xdb, 0xfe,
    0x1f, 0x00, 0xa9, 0xe6, 0x20, 0x28, 0xf1
  };
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, loader, sizeof( loader ) );
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 8 ) !=
      ACCELERATION_MODE_INCREASING ) error++;

  /* Do not accept an arbitrary second immediate value. */
  writebyte_internal( LOADER_TEST_BASE + 5, 0x00 );
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 8 ) !=
      ACCELERATION_MODE_NONE ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_rom_acceleration( void )
{
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, NULL, 2 );
  writebyte_internal( LOADER_TEST_BASE, 0x34 );
  writebyte_internal( LOADER_TEST_BASE + 1, 0x12 );
  z80.bc.b.l = 0xff;
  z80.af.b.l = 0x00;
  z80.sp.w = LOADER_TEST_BASE;
  acceleration_mode = ACCELERATION_MODE_INCREASING;
  accelerate_loader( 1 );
  if( z80.bc.b.h != 0xfe || z80.pc.w != 0x1234 ||
      z80.sp.w != LOADER_TEST_BASE + 2 ) error++;
  if( z80.bc.b.l != ( tape_microphone ? 0xdf : 0xff ) ||
      !( z80.af.b.l & 0x01 ) ) error++;

  z80.sp.w = LOADER_TEST_BASE;
  acceleration_mode = ACCELERATION_MODE_DECREASING;
  accelerate_loader( 1 );
  if( z80.bc.b.h != 0x00 || z80.pc.w != 0x1234 ||
      z80.sp.w != LOADER_TEST_BASE + 2 ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_software_projects( void )
{
  static const libspectrum_byte loader[] = {
    0x47, 0x08, 0x3e, 0x7f, 0xdb, 0xfe, 0xa9,
    0xe6, 0x40, 0x20, 0x04, 0x05, 0x20, 0xf4
  };
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, loader, sizeof( loader ) );
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 6 ) !=
      ACCELERATION_MODE_SOFTWARE_PROJECTS ) error++;

  /* The branch must return to the start of this specific sampling loop. */
  writebyte_internal( LOADER_TEST_BASE + 13, 0xf5 );
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 6 ) !=
      ACCELERATION_MODE_NONE ) error++;
  writebyte_internal( LOADER_TEST_BASE + 13, loader[ 13 ] );

  acceleration_pc = LOADER_TEST_BASE + 6;
  acceleration_mode = ACCELERATION_MODE_SOFTWARE_PROJECTS;
  z80.af_.b.h = 0x24;
  accelerate_loader( 0 );
  if( z80.bc.b.h != 0x1a || z80.pc.w != LOADER_TEST_BASE + 15 ) error++;
  accelerate_loader( 1 );
  if( z80.bc.b.h != 0x10 || z80.pc.w != LOADER_TEST_BASE + 15 ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_gremlin( void )
{
  static const libspectrum_byte loader[] = {
    0x2e, 0x00, 0x2c, 0xdb, 0xfe, 0xa4, 0xca, 0x02, 0x80,
    0x3e, 0x08, 0xd3, 0xfe, 0x2c, 0xdb, 0xfe, 0xa4, 0xc2, 0x0d, 0x80,
    0x7d, 0xc9
  };
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, loader, sizeof( loader ) );
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 5 ) !=
      ACCELERATION_MODE_GREMLIN_RISING ) error++;
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 16 ) !=
      ACCELERATION_MODE_GREMLIN_FALLING ) error++;

  writebyte_internal( LOADER_TEST_BASE + 12, 0xff );
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 16 ) !=
      ACCELERATION_MODE_NONE ) error++;
  writebyte_internal( LOADER_TEST_BASE + 12, loader[ 12 ] );

  /* Do not accept a falling-edge branch to a different loop. */
  writebyte_internal( LOADER_TEST_BASE + 18, 0x0c );
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 16 ) !=
      ACCELERATION_MODE_NONE ) error++;
  writebyte_internal( LOADER_TEST_BASE + 18, loader[ 18 ] );

  z80.hl.b.l = 1;             /* First INC L has executed. */
  acceleration_mode = ACCELERATION_MODE_GREMLIN_RISING;
  acceleration_pc = LOADER_TEST_BASE + 5;
  accelerate_loader( 0 );
  if( z80.hl.b.l != 24 || z80.pc.w != LOADER_TEST_BASE + 9 ) error++;
  z80.hl.b.l++;                /* INC L in the falling-edge loop. */
  acceleration_mode = ACCELERATION_MODE_GREMLIN_FALLING;
  acceleration_pc = LOADER_TEST_BASE + 16;
  accelerate_loader( 0 );
  if( z80.hl.b.l != 48 || z80.af.b.h != 48 ||
      z80.pc.w != LOADER_TEST_BASE + 20 ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_ear_use( void )
{
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, NULL, 22 );
  /* A keyboard scan's AND 1f does not consume the EAR input. */
  writebyte_internal( LOADER_TEST_BASE + 5, 0xdb );
  writebyte_internal( LOADER_TEST_BASE + 6, 0xfe );
  writebyte_internal( LOADER_TEST_BASE + 7, 0x2f );
  writebyte_internal( LOADER_TEST_BASE + 8, 0xe6 );
  writebyte_internal( LOADER_TEST_BASE + 9, 0x1f );
  if( ula_read_uses_ear( LOADER_TEST_BASE + 7 ) ) error++;
  if( !ula_read_is_keyboard_scan( LOADER_TEST_BASE + 7 ) ) error++;

  /* A loader which preserves EAR must not be excluded by the keyboard
     detector, even if it also complements the input. */
  writebyte_internal( LOADER_TEST_BASE + 9, 0x40 );
  if( ula_read_is_keyboard_scan( LOADER_TEST_BASE + 7 ) ) error++;
  if( !ula_read_uses_ear( LOADER_TEST_BASE + 7 ) ) error++;
  writebyte_internal( LOADER_TEST_BASE + 9, 0x1f );
  writebyte_internal( LOADER_TEST_BASE + 7, 0x1f ); /* RRA, not CPL */
  if( ula_read_is_keyboard_scan( LOADER_TEST_BASE + 7 ) ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_movieload( void )
{
  static const libspectrum_byte loader[] = {
    0x14, 0xc8, 0x3e, 0x7f, 0xdb, 0xfe, 0x1f,
    0x00, 0xab, 0xe6, 0x20, 0x28, 0xf3
  };
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, loader, sizeof( loader ) );
  if( !movieload_loader_detector( LOADER_TEST_BASE + 6 ) ) error++;

  /* The branch must return to this sampling loop. */
  writebyte_internal( LOADER_TEST_BASE + 12, 0xf2 );
  if( movieload_loader_detector( LOADER_TEST_BASE + 6 ) ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_sign_flag( void )
{
  static const libspectrum_byte loader[] = {
    0x04, 0xc8, 0xdb, 0xfe, 0x87, 0xfa, 0x00, 0x80
  };
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, loader, sizeof( loader ) );
  if( !sign_flag_loader_detector( LOADER_TEST_BASE + 4 ) ) error++;
  writebyte_internal( LOADER_TEST_BASE + 5, 0xf2 );
  if( !sign_flag_loader_detector( LOADER_TEST_BASE + 4 ) ) error++;
  writebyte_internal( LOADER_TEST_BASE + 5, loader[ 5 ] );
  writebyte_internal( LOADER_TEST_BASE + 4, 0x86 );
  if( sign_flag_loader_detector( LOADER_TEST_BASE + 4 ) ) error++;
  writebyte_internal( LOADER_TEST_BASE + 4, loader[ 4 ] );
  writebyte_internal( LOADER_TEST_BASE + 6, 0x01 );
  if( sign_flag_loader_detector( LOADER_TEST_BASE + 4 ) ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static int
loader_test_modified_rom( void )
{
  static const libspectrum_byte loader[] = {
    0xd9, 0x11, 0x6b, 0x80, 0x21, 0x29, 0x80, 0x06,
    0x05, 0xcd, 0x10, 0x80, 0xd0, 0xd9, 0x06, 0x06,
    0x7e, 0x91, 0x77, 0x7d, 0x38, 0x03, 0x00, 0x18,
    0x02, 0x12, 0x1c, 0xc6, 0x06, 0x6f, 0x10, 0xf0,
    0xd9, 0xa7, 0x04, 0xc8, 0x3e, 0x7f, 0xdb, 0xfe,
    0x1f, 0xd0, 0xa9, 0xe6, 0x20, 0x28, 0xf3, 0x79,
    0x2f, 0x4f, 0x37, 0xc9
  };
  loader_test_memory_t memory;
  int error = 0;

  loader_test_install( &memory, loader, sizeof( loader ) );
  if( !modified_rom_loader_detector( LOADER_TEST_BASE + 40 ) ) error++;
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 40 ) !=
      ACCELERATION_MODE_NONE ) error++;
  if( !loader_loop_detector( LOADER_TEST_BASE + 40 ) ) error++;

  writebyte_internal( LOADER_TEST_BASE, 0xd8 );
  if( modified_rom_loader_detector( LOADER_TEST_BASE + 40 ) ) error++;
  writebyte_internal( LOADER_TEST_BASE, loader[ 0 ] );

  /* A call elsewhere is not the custom EDGE1 timing context. */
  writebyte_internal( LOADER_TEST_BASE + 10, 0x11 );
  if( modified_rom_loader_detector( LOADER_TEST_BASE + 40 ) ) error++;
  if( acceleration_detector_at_in( LOADER_TEST_BASE + 40 ) !=
      ACCELERATION_MODE_INCREASING ) error++;

  loader_test_restore_memory( &memory );
  return error;
}

static loader_test_state_t
loader_test_save_state( void )
{
  loader_test_state_t state;

  state.b = z80.bc.b.h;
  state.c = z80.bc.b.l;
  state.a_ = z80.af_.b.h;
  state.a = z80.af.b.h;
  state.f = z80.af.b.l;
  state.l = z80.hl.b.l;
  state.pc = z80.pc.w;
  state.sp = z80.sp.w;
  state.acceleration_pc = acceleration_pc;
  state.acceleration_mode = acceleration_mode;
  return state;
}

static void
loader_test_restore_state( const loader_test_state_t *state )
{
  z80.bc.b.h = state->b;
  z80.bc.b.l = state->c;
  z80.af_.b.h = state->a_;
  z80.af.b.h = state->a;
  z80.af.b.l = state->f;
  z80.hl.b.l = state->l;
  z80.pc.w = state->pc;
  z80.sp.w = state->sp;
  acceleration_pc = state->acceleration_pc;
  acceleration_mode = state->acceleration_mode;
}

static int
loader_test_keyboard_scan_grace( void )
{
  loader_test_memory_t memory;
  loader_test_state_t state = loader_test_save_state();
  libspectrum_dword saved_tstates = tstates;
  int saved_playing = tape_playing;
  int saved_detect = settings_current.detect_loader;
  int saved_accelerate = settings_current.accelerate_loader;
  int saved_fastload = settings_current.fastload;
  int error = 0;
  int row;

  loader_test_install( &memory, NULL, 52 );
  settings_current.detect_loader = 1;
  settings_current.accelerate_loader = 0;
  /* Keep tape_stop() from initialising the sound subsystem before the
     sound tests run. */
  settings_current.fastload = 0;

  /* A ROM-style keyboard scan (CPL; AND 1f; RRA) whose rows move B must
     not stop the tape through the timing fallback before the bounded
     grace period (LOADER_STOP_GRACE_FRAMES in loader.c) expires. */
  writebyte_internal( LOADER_TEST_BASE + 40, 0x2f );
  writebyte_internal( LOADER_TEST_BASE + 41, 0xe6 );
  writebyte_internal( LOADER_TEST_BASE + 42, 0x1f );
  z80.pc.w = LOADER_TEST_BASE + 40;
  loader_tape_play();
  tape_playing = 1;
  tstates = 0;

  for( row = 0; row < 250; row++ ) {
    z80.bc.b.h = 0x40 - ( row & 7 );	/* every scan row changes B */
    loader_detect_loader();
    if( !tape_playing ) {
      printf( "keyboard-scan grace: tape stopped at frame %d\n", row );
      error++;
    }
    loader_frame( machine_current->timings.tstates_per_frame );
  }
  error |= !tape_playing;		/* grace not yet expired */

  z80.bc.b.h = 0x40;
  loader_detect_loader();
  error |= tape_playing;		/* ...but the next keyboard-only read stops it */

  /* A recognised loader read resets the grace period: after the reset,
     keyboard-only activity again gets the full grace before stopping. */
  writebyte_internal( LOADER_TEST_BASE + 0, 0x04 );
  writebyte_internal( LOADER_TEST_BASE + 1, 0xc8 );
  writebyte_internal( LOADER_TEST_BASE + 2, 0x3e );
  writebyte_internal( LOADER_TEST_BASE + 3, 0x00 );
  writebyte_internal( LOADER_TEST_BASE + 4, 0xdb );
  writebyte_internal( LOADER_TEST_BASE + 5, 0xfe );
  writebyte_internal( LOADER_TEST_BASE + 6, 0x1f );
  writebyte_internal( LOADER_TEST_BASE + 7, 0xa9 );
  writebyte_internal( LOADER_TEST_BASE + 8, 0xe6 );
  writebyte_internal( LOADER_TEST_BASE + 9, 0x20 );
  writebyte_internal( LOADER_TEST_BASE + 10, 0x28 );
  writebyte_internal( LOADER_TEST_BASE + 11, 0xf4 );
  z80.pc.w = LOADER_TEST_BASE + 6;
  tape_playing = 1;
  loader_detect_loader();
  error |= !tape_playing;

  z80.pc.w = LOADER_TEST_BASE + 40;
  for( row = 0; row < 250; row++ ) {
    z80.bc.b.h = 0x40 - ( row & 7 );
    loader_detect_loader();
    if( !tape_playing ) {
      printf( "keyboard-scan grace: tape stopped at frame %d after reset\n",
              row );
      error++;
    }
    loader_frame( machine_current->timings.tstates_per_frame );
  }
  error |= !tape_playing;		/* the restarted grace still holds */

  z80.bc.b.h = 0x40;
  loader_detect_loader();
  error |= tape_playing;		/* ...and stops again at the renewed limit */

  /* Non-loader reads still stop the tape through the normal
     consecutive-read limit (LOADER_STOP_NON_EAR_READS in loader.c). */
  loader_tape_play();
  z80.pc.w = LOADER_TEST_BASE + 44;
  tape_playing = 1;
  tstates = 0;
  for( row = 0; row < 9; row++ ) {
    tstates += 60000;			/* implausible counter reads */
    z80.bc.b.h = 0x40;
    loader_detect_loader();
    if( !tape_playing ) {
      printf( "non-loader stop: tape stopped after %d reads\n", row );
      error++;
    }
  }
  error |= !tape_playing;

  tstates += 60000;
  loader_detect_loader();
  error |= tape_playing;

  tstates = saved_tstates;
  tape_playing = saved_playing;
  settings_current.detect_loader = saved_detect;
  settings_current.accelerate_loader = saved_accelerate;
  settings_current.fastload = saved_fastload;
  loader_test_restore_state( &state );
  loader_test_restore_memory( &memory );
  return error;
}

int
loader_unittest( void )
{
  loader_test_state_t state = loader_test_save_state();
  int error = 0;

  error += loader_test_pattern_matcher();
  error += loader_test_acceleration_patterns();
  error += loader_test_digital_integration();
  error += loader_test_microprose();
  error += loader_test_rom_acceleration();
  error += loader_test_software_projects();
  error += loader_test_gremlin();
  error += loader_test_ear_use();
  error += loader_test_movieload();
  error += loader_test_sign_flag();
  error += loader_test_modified_rom();
  error += loader_test_keyboard_scan_grace();

  loader_test_restore_state( &state );
  if( error ) printf( "loader_unittest failed\n" );
  return error;
}
