/* tape.c: tape unit tests
   Copyright (c) 1999-2017 Philip Kendall, Darren Salt, Witold Filipczyk
   Copyright (c) 2015-2018 UB880D
   Copyright (c) 2016-2026 Fredrick Meunier
   Copyright (c) 2026 Alberto Garcia

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
*/

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libspectrum.h"

#include "compat.h"
#include "event.h"
#include "fuse.h"
#include "machine.h"
#include "memory_pages.h"
#include "periph.h"
#include "peripherals/ula.h"
#include "rzx.h"
#include "settings.h"
#include "snapshot.h"
#include "spectrum.h"
#include "tape.h"
#include "tape_internals.h"
#include "utils.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"

typedef struct tape_test_fixture {
  libspectrum_tape *saved_tape;
  libspectrum_tape *test_tape;
  libspectrum_tape_block *rom;
  libspectrum_tape_block *following;
  libspectrum_byte *data;
  int saved_microphone;
} tape_test_fixture;

static int
tape_test_create_rom_pause_tape( tape_test_fixture *fixture )
{
  fixture->test_tape = libspectrum_tape_alloc();
  if( !fixture->test_tape ) return 1;

  fixture->rom =
    libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_ROM );
  fixture->following =
    libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PAUSE );
  fixture->data = libspectrum_new( libspectrum_byte, 2 );
  if( !fixture->rom || !fixture->following || !fixture->data ) return 1;

  fixture->data[0] = 0x80;
  fixture->data[1] = 0x80;
  libspectrum_tape_block_set_data_length( fixture->rom, 2 );
  libspectrum_tape_block_set_data( fixture->rom, fixture->data );
  fixture->data = NULL;
  libspectrum_tape_block_set_pause_tstates( fixture->rom, 3500000 );
  libspectrum_tape_block_set_pause_tstates( fixture->following, 1 );

  if( libspectrum_tape_append_block( fixture->test_tape, fixture->rom ) )
    return 1;
  fixture->rom = NULL;
  if( libspectrum_tape_append_block( fixture->test_tape,
                                     fixture->following ) )
    return 1;
  fixture->following = NULL;
  return 0;
}

static int
check_trap_pause_position( void )
{
  libspectrum_tape_signal_level level;
  int position;

  return tape_trap_finish_rom_block() ||
         libspectrum_tape_state( tape ) != LIBSPECTRUM_TAPE_STATE_PAUSE ||
         libspectrum_tape_position( &position, tape ) || position != 0 ||
         libspectrum_tape_signal_level_get( &level, tape ) ||
         level != LIBSPECTRUM_TAPE_SIGNAL_LOW || tape_microphone != 1;
}

static int
check_pause_playback( void )
{
  libspectrum_tape_edge edge;
  int position;

  /* Previewing must not consume the pause: normal playback returns the same
     high pause interval and only then selects the following block. */
  return libspectrum_tape_get_next_edge( &edge, tape ) ||
         edge.tstates != 3500000 ||
         edge.level != LIBSPECTRUM_TAPE_SIGNAL_HIGH ||
         !( edge.flags & LIBSPECTRUM_TAPE_FLAGS_BLOCK ) ||
         libspectrum_tape_position( &position, tape ) || position != 1;
}

static void
tape_test_cleanup( tape_test_fixture *fixture )
{
  tape = fixture->saved_tape;
  tape_microphone = fixture->saved_microphone;
  if( fixture->data ) libspectrum_free( fixture->data );
  if( fixture->rom ) libspectrum_tape_block_free( fixture->rom );
  if( fixture->following )
    libspectrum_tape_block_free( fixture->following );
  libspectrum_tape_free( fixture->test_tape );
}

static int
check_block_details( libspectrum_tape_block *block, const char *expected )
{
  char *details;
  if( tape_block_details( &details, block ) ) return 1;
  int error = strcmp( details, expected ) != 0;
  if( error )
    printf( "tape block detail: expected '%s', got '%s'\n", expected, details );
  libspectrum_free( details );
  return error;
}

static int
tape_block_details_unittest( void )
{
  libspectrum_tape_block *block;
  libspectrum_byte *data;
  libspectrum_dword *lengths;
  size_t *repeats;
  char *text;
  int error = 0;

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_ROM );
  if( !block ) return 1;
  data = libspectrum_new( libspectrum_byte, 19 );
  if( !data ) { libspectrum_tape_block_free( block ); return 1; }
  memset( data, ' ', 19 );
  data[0] = 0x00; data[1] = 0x03;
  memcpy( &data[2], "TEST      ", 10 );
  libspectrum_tape_block_set_data_length( block, 19 );
  libspectrum_tape_block_set_data( block, data );
  error |= check_block_details( block, "Bytes: \"TEST\"" );
  data[2] = 0x60; /* Spectrum pound sign becomes UTF-8. */
  error |= check_block_details( block, "Bytes: \"\xc2\xa3" "EST\"" );
  data[0] = 0xff;
  error |= check_block_details( block, "19 bytes" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_TURBO );
  libspectrum_tape_block_set_data_length( block, 42 );
  error |= check_block_details( block, "42 bytes" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PURE_TONE );
  libspectrum_tape_block_set_pulse_length( block, 2168 );
  error |= check_block_details( block, "2168 tstates" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PULSES );
  libspectrum_tape_block_set_count( block, 7 );
  error |= check_block_details( block, "7 pulses" );
  libspectrum_tape_block_free( block );

  block =
    libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PULSE_SEQUENCE );
  if( !block ) return 1;
  lengths = libspectrum_new( libspectrum_dword, 2 );
  repeats = libspectrum_new( size_t, 2 );
  if( !lengths || !repeats ) {
    if( lengths ) libspectrum_free( lengths );
    if( repeats ) libspectrum_free( repeats );
    libspectrum_tape_block_free( block );
    return 1;
  }
  lengths[0] = 100; lengths[1] = 200;
  repeats[0] = 2; repeats[1] = 3;
  libspectrum_tape_block_set_count( block, 2 );
  libspectrum_tape_block_set_pulse_lengths( block, lengths );
  libspectrum_tape_block_set_pulse_repeats( block, repeats );
  error |= check_block_details( block, "5 pulses" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PAUSE );
  libspectrum_tape_block_set_pause( block, 1000 );
  error |= check_block_details( block, "1000 ms" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_COMMENT );
  if( !block ) return 1;
  text = libspectrum_new( char, 8 );
  if( !text ) { libspectrum_tape_block_free( block ); return 1; }
  strcpy( text, "comment" );
  libspectrum_tape_block_set_text( block, text );
  error |= check_block_details( block, "comment" );
  libspectrum_tape_block_free( block );

  /* Metadata is returned in full, well beyond the old 30/80/128-byte limits. */
  const libspectrum_tape_type text_types[] = {
    LIBSPECTRUM_TAPE_BLOCK_COMMENT, LIBSPECTRUM_TAPE_BLOCK_GROUP_START,
    LIBSPECTRUM_TAPE_BLOCK_MESSAGE
  };
  const char sequence[] = "A\xc2\xa3\xe2\x82\xac\xf0\x9f\x98\x80";
  for( size_t i = 0; i < sizeof( text_types ) / sizeof( text_types[0] ); i++ ) {
    block = libspectrum_tape_block_alloc( text_types[i] );
    if( !block ) return 1;
    text = libspectrum_new( char, 1001 );
    for( size_t j = 0; j < 100; j++ )
      memcpy( text + j * 10, sequence, 10 );
    text[1000] = '\0';
    libspectrum_tape_block_set_text( block, text );
    error |= check_block_details( block, text );
    char *details;
    if( tape_block_details( &details, block ) ) {
      error++;
    } else {
      text[0] = 'Z';
      error |= details[0] != 'A'; /* The result is owned, not borrowed. */
      libspectrum_free( details );
    }
    libspectrum_tape_block_free( block );
  }

  /* Even malformed CUSTOM identifiers are copied without rewriting bytes. */
  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_CUSTOM );
  if( !block ) return 1;
  text = libspectrum_new( char, 4 );
  strcpy( text, "A\xff\xc2" );
  libspectrum_tape_block_set_text( block, text );
  error |= check_block_details( block, "A\xff\xc2" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_JUMP );
  libspectrum_tape_block_set_offset( block, 3 );
  error |= check_block_details( block, "Forward 3 blocks" );
  libspectrum_tape_block_set_offset( block, -2 );
  error |= check_block_details( block, "Backward 2 blocks" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_LOOP_START );
  libspectrum_tape_block_set_count( block, 4 );
  error |= check_block_details( block, "4 iterations" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_SELECT );
  libspectrum_tape_block_set_count( block, 6 );
  error |= check_block_details( block, "6 options" );
  /* No option arrays are needed for formatting, so keep block cleanup empty. */
  libspectrum_tape_block_set_count( block, 0 );
  libspectrum_tape_block_free( block );

  /* RLE pulse data counts pulses using the CSW/TZX RLE encoding: a pulse of
     n samples is one byte if n <= 255, otherwise a zero marker followed by a
     little-endian dword (here 0x0000012c = 300 samples). */
  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_RLE_PULSE );
  if( !block ) return 1;
  data = libspectrum_new( libspectrum_byte, 6 );
  if( !data ) { libspectrum_tape_block_free( block ); return 1; }
  memset( data, 0, 6 );
  data[0] = 100;
  data[2] = 0x2c; data[3] = 1;
  libspectrum_tape_block_set_data_length( block, 6 );
  libspectrum_tape_block_set_data( block, data );
  error |= check_block_details( block, "2 pulses" );

  /* A block that carries an exact sample rate (for example one loaded from
     a standalone CSW file) reports the rate next to the pulse count, like
     the TZX CSW recording listing. */
  if( libspectrum_tape_block_set_sample_rate( block, 44100 ) ) {
    libspectrum_tape_block_free( block );
    return 1;
  }
  error |= check_block_details( block, "2 pulses, 44100 Hz" );
  libspectrum_tape_block_set_sample_rate( block, 0 );
  error |= check_block_details( block, "2 pulses" );
  libspectrum_tape_block_free( block );

  /* The TZX CSW recording listing carries its pulse count and stored rate
     directly, as written by the reader from the block header. */
  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_TZX_CSW );
  if( !block ) return 1;
  if( libspectrum_tape_block_set_csw_pulses( block, 11852 ) ||
      libspectrum_tape_block_set_sample_rate( block, 44100 ) ) {
    libspectrum_tape_block_free( block );
    return 1;
  }
  error |= check_block_details( block, "11852 pulses, 44100 Hz" );
  libspectrum_tape_block_free( block );

  /* A custom-ROM data block formats like a ROM block: the 19-byte header
     decodes to the block type and name, and anything else falls back to a
     byte count. */
  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_DATA_BLOCK );
  if( !block ) return 1;
  data = libspectrum_new( libspectrum_byte, 19 );
  if( !data ) { libspectrum_tape_block_free( block ); return 1; }
  memset( data, ' ', 19 );
  data[0] = 0x00; data[1] = 0x03;
  memcpy( &data[2], "TEST      ", 10 );
  libspectrum_tape_block_set_data_length( block, 19 );
  libspectrum_tape_block_set_data( block, data );
  error |= check_block_details( block, "Bytes: \"TEST\"" );
  data[0] = 0xff;
  error |= check_block_details( block, "19 bytes" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PURE_DATA );
  if( !block ) return 1;
  libspectrum_tape_block_set_data_length( block, 42 );
  error |= check_block_details( block, "42 bytes" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_RAW_DATA );
  if( !block ) return 1;
  libspectrum_tape_block_set_data_length( block, 7 );
  error |= check_block_details( block, "7 bytes" );
  libspectrum_tape_block_free( block );

  /* Group starts, messages and custom blocks all list their text. */
  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_GROUP_START );
  if( !block ) return 1;
  text = libspectrum_new( char, 6 );
  if( !text ) { libspectrum_tape_block_free( block ); return 1; }
  strcpy( text, "group" );
  libspectrum_tape_block_set_text( block, text );
  error |= check_block_details( block, "group" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_MESSAGE );
  if( !block ) return 1;
  text = libspectrum_new( char, 8 );
  if( !text ) { libspectrum_tape_block_free( block ); return 1; }
  strcpy( text, "message" );
  libspectrum_tape_block_set_text( block, text );
  error |= check_block_details( block, "message" );
  libspectrum_tape_block_free( block );

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_CUSTOM );
  if( !block ) return 1;
  text = libspectrum_new( char, 7 );
  if( !text ) { libspectrum_tape_block_free( block ); return 1; }
  strcpy( text, "custom" );
  libspectrum_tape_block_set_text( block, text );
  error |= check_block_details( block, "custom" );
  libspectrum_tape_block_free( block );

  return error;
}

static libspectrum_tape_block *
make_rom_block( const libspectrum_byte *source, size_t length )
{
  libspectrum_tape_block *block =
    libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_ROM );
  libspectrum_byte *data;

  if( !block ) return NULL;
  if( !length ) return block;
  data = libspectrum_new( libspectrum_byte, length );
  memcpy( data, source, length );
  libspectrum_tape_block_set_data_length( block, length );
  libspectrum_tape_block_set_data( block, data );
  return block;
}

static int
run_trap_load( const libspectrum_byte *data, size_t length, int verify,
               libspectrum_word requested, libspectrum_word count,
               size_t *consumed )
{
  libspectrum_tape_block *block = make_rom_block( data, length );
  int error;

  if( !block ) return 1;
  A_ = requested;
  F_ = verify ? 0 : FLAG_C;
  DE = count;
  IX = 0x8000;
  error = tape_trap_load_block( block, consumed );
  libspectrum_tape_block_free( block );
  return error;
}

static int
trap_load_special_cases_unittest( const libspectrum_byte *valid,
                                   size_t valid_length )
{
  static const libspectrum_byte bad_parity[] = { 0xff, 0x12, 0x34, 0xda };
  size_t consumed;
  int error = 0;

  error |= run_trap_load( bad_parity, sizeof( bad_parity ), 0, 0xff, 2,
                          &consumed );
  error |= consumed != 4 || ( F & FLAG_C );

  error |= run_trap_load( valid, valid_length, 0, 0xff, 1, &consumed );
  error |= consumed != 3 || DE != 0 || IX != 0x8001;

  error |= run_trap_load( NULL, 0, 0, 0xff, 2, &consumed );
  error |= consumed != 0 || L != 1 || F_ != 1 || ( F & FLAG_C );

  error |= run_trap_load( valid, valid_length, 0, 0xff, 0, &consumed );
  error |= consumed != 1 || DE != 0 || IX != 0x8000 || B != 0xb0;

  /* A one-byte ROM block contains only the flag and no data bytes. */
  static const libspectrum_byte single[] = { 0xff };
  error |= run_trap_load( single, sizeof( single ), 0, 0xff, 2, &consumed );
  error |= consumed != 1 || DE != 2 || IX != 0x8000 || L != 1 ||
           ( F & FLAG_C );
  return error;
}

static int
pzx_trap_unittest( void )
{
  libspectrum_tape *saved_tape = tape, *test = libspectrum_tape_alloc();
  libspectrum_tape_block *pilot, *data, *pause, *leading_pause;
  libspectrum_tape_cursor *reference;
  libspectrum_tape_edge expected, actual;
  libspectrum_dword *lengths = libspectrum_new( libspectrum_dword, 3 );
  size_t *repeats = libspectrum_new( size_t, 3 );
  libspectrum_word *zero = libspectrum_new( libspectrum_word, 2 );
  libspectrum_word *one = libspectrum_new( libspectrum_word, 2 );
  libspectrum_byte *bytes = libspectrum_new( libspectrum_byte, 4 );
  libspectrum_word saved_af = AF, saved_af_ = AF_, saved_bc = BC;
  libspectrum_word saved_de = DE, saved_hl = HL, saved_ix = IX, saved_pc = PC;
  int saved_traps = settings_current.tape_traps;
  int saved_ready = trap_pzx_ready, saved_resume = trap_resume_pending;
  int saved_mic = tape_microphone, error = 0;
  libspectrum_byte saved_memory[2];

  saved_memory[0] = readbyte_internal( 0x8000 );
  saved_memory[1] = readbyte_internal( 0x8001 );
  pilot = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PULSE_SEQUENCE );
  data = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_DATA_BLOCK );
  pause = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PAUSE );
  leading_pause = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PAUSE );
  libspectrum_tape_block_set_pause_tstates( leading_pause, 1234 );
  lengths[0] = 2168; lengths[1] = 667; lengths[2] = 735;
  repeats[0] = 3223; repeats[1] = repeats[2] = 1;
  libspectrum_tape_block_set_count( pilot, 3 );
  libspectrum_tape_block_set_pulse_lengths( pilot, lengths );
  libspectrum_tape_block_set_pulse_repeats( pilot, repeats );
  zero[0] = zero[1] = 855;
  one[0] = one[1] = 1710;
  bytes[0] = 0xff; bytes[1] = 0x12; bytes[2] = 0x34; bytes[3] = 0xd9;
  libspectrum_tape_block_set_data( data, bytes );
  libspectrum_tape_block_set_data_length( data, 4 );
  libspectrum_tape_block_set_bits_in_last_byte( data, 8 );
  libspectrum_tape_block_set_bit0_pulse_count( data, 2 );
  libspectrum_tape_block_set_bit1_pulse_count( data, 2 );
  libspectrum_tape_block_set_bit0_pulses( data, zero );
  libspectrum_tape_block_set_bit1_pulses( data, one );
  libspectrum_tape_block_set_level( data, 1 );
  libspectrum_tape_block_set_tail_length( data, 945 );
  libspectrum_tape_block_set_pause_tstates( pause, 3500000 );
  error |= !tape_trap_pzx_data( data ) || tape_trap_pzx_data( pilot ) ||
           tape_trap_pzx_data( NULL );
  libspectrum_tape_block_set_data_length( data, 1 );
  error |= tape_trap_pzx_data( data );
  libspectrum_tape_block_set_data_length( data, 4 );
  libspectrum_tape_block_set_bits_in_last_byte( data, 7 );
  error |= tape_trap_pzx_data( data );
  libspectrum_tape_block_set_bits_in_last_byte( data, 8 );
  zero[1] = 856;
  error |= tape_trap_pzx_data( data );
  zero[1] = 855;
  libspectrum_tape_block_set_bit1_pulse_count( data, 1 );
  error |= tape_trap_pzx_data( data );
  libspectrum_tape_block_set_bit1_pulse_count( data, 2 );
  libspectrum_tape_append_block( test, leading_pause );
  libspectrum_tape_append_block( test, pilot );
  libspectrum_tape_append_block( test, data );
  libspectrum_tape_append_block( test, pause );
  tape = test;
  libspectrum_tape_nth_block( tape, 0 );
  reference = libspectrum_tape_cursor_capture( tape );
  for( size_t i = 0; i < 1 + 3225 + 64; i++ )
    error |= libspectrum_tape_cursor_get_next_edge( &expected, reference );
  error |= libspectrum_tape_cursor_get_next_edge( &expected, reference );
  settings_current.tape_traps = 1;
  trap_pzx_ready = trap_resume_pending = 0;
  A_ = 0xff; F_ = FLAG_C; DE = 2; IX = 0x8000;
  error |= tape_load_trap();
  error |= DE != 0 || IX != 0x8002 || !( F & FLAG_C ) ||
           readbyte_internal( 0x8000 ) != 0x12 ||
           readbyte_internal( 0x8001 ) != 0x34 || !trap_resume_pending;
  error |= libspectrum_tape_current_block( tape ) != data;
  error |= libspectrum_tape_get_next_edge( &actual, tape );
  error |= actual.tstates != expected.tstates || actual.level != expected.level ||
           actual.flags != expected.flags;
  libspectrum_tape_cursor_free( reference );
  tape = saved_tape;
  libspectrum_tape_free( test );
  settings_current.tape_traps = saved_traps;
  trap_pzx_ready = saved_ready; trap_resume_pending = saved_resume;
  tape_microphone = saved_mic;
  writebyte_internal( 0x8000, saved_memory[0] );
  writebyte_internal( 0x8001, saved_memory[1] );
  AF = saved_af; AF_ = saved_af_; BC = saved_bc;
  DE = saved_de; HL = saved_hl; IX = saved_ix; PC = saved_pc;
  if( error ) printf( "pzx_trap_unittest failed\n" );
  return error;
}

static int
trap_load_unittest( void )
{
  static const libspectrum_byte valid[] = { 0xff, 0x12, 0x34, 0xd9 };
  libspectrum_byte saved[2];
  libspectrum_word saved_af = AF, saved_af_ = AF_, saved_bc = BC;
  libspectrum_word saved_de = DE, saved_hl = HL, saved_ix = IX;
  size_t consumed;
  int error = 0;

  saved[0] = readbyte_internal( 0x8000 );
  saved[1] = readbyte_internal( 0x8001 );

  error |= run_trap_load( valid, sizeof( valid ), 0, 0xff, 2, &consumed );
  error |= consumed != 4 || DE != 0 || IX != 0x8002 || !( F & FLAG_C ) ||
           readbyte_internal( 0x8000 ) != 0x12 ||
           readbyte_internal( 0x8001 ) != 0x34;

  error |= run_trap_load( valid, sizeof( valid ), 1, 0xff, 2, &consumed );
  error |= consumed != 4 || DE != 0 || IX != 0x8002 || !( F & FLAG_C );

  writebyte_internal( 0x8000, 0x00 );
  error |= run_trap_load( valid, sizeof( valid ), 1, 0xff, 2, &consumed );
  error |= consumed != 2 || DE != 2 || IX != 0x8000 || ( F & FLAG_C );

  error |= run_trap_load( valid, sizeof( valid ), 0, 0x00, 2, &consumed );
  error |= consumed != 1 || DE != 2 || IX != 0x8000 || ( F & FLAG_C );

  error |= trap_load_special_cases_unittest( valid, sizeof( valid ) );

  writebyte_internal( 0x8000, saved[0] );
  writebyte_internal( 0x8001, saved[1] );
  AF = saved_af; AF_ = saved_af_; BC = saved_bc;
  DE = saved_de; HL = saved_hl; IX = saved_ix;
  return error;
}

static int
tape_record_unittest( void )
{
  libspectrum_tape *test_tape, *read_back;
  libspectrum_tape_block *block;
  libspectrum_byte *buffer = NULL;
  libspectrum_dword saved_time = tstates;
  libspectrum_dword saved_speed = machine_current->timings.processor_speed;
  libspectrum_byte saved_ula = ula_last_byte();
  int saved_modified = tape_modified;
  int error = 0, high, model;
  size_t length, i;

  for( model = 0; model < 2; model++ ) {
    libspectrum_dword speed = model ? 3546900 : 3500000;
    machine_current->timings.processor_speed = speed;
    for( high = 0; high < 2; high++ ) {
      libspectrum_qword total = 0, expected_total = 0;
      libspectrum_tape_edge edge;
      const libspectrum_dword intervals[] = { 4, 7, 70915, 200000, 19 };
      test_tape = libspectrum_tape_alloc();
      read_back = libspectrum_tape_alloc();
      tape_record_set_tape( test_tape );
      tstates = 0;
      writeport_internal( 0xfe, high ? ULA_PORT_MIC_BIT : 0 );
      tape_record_start();
      tape_record_start(); /* idempotent */
      for( i = 0; i < 4; i++ ) {
        tstates += intervals[i];
        /* A non-MIC write must not split the interval. */
        writeport_internal( 0xfe, ula_last_byte() ^ 1 );
        writeport_internal( 0xfe, ula_last_byte() ^ ULA_PORT_MIC_BIT );
        /* Exercise timestamp rebasing with different frame lengths. This
           tests bookkeeping, not recording during RZX (which is blocked). */
        tape_record_frame( tstates );
        tstates = 0;
      }
      tstates += intervals[4];
      error |= tape_record_stop();
      error |= tape_record_stop();
      block = libspectrum_tape_current_block( test_tape );
      error |= !block || libspectrum_tape_block_type( block ) !=
                        LIBSPECTRUM_TAPE_BLOCK_PULSE_SEQUENCE;
      length = 0;
      buffer = NULL;
      error |= libspectrum_tape_write( &buffer, &length, test_tape,
                                        LIBSPECTRUM_ID_TAPE_PZX );
      if( buffer ) {
        error |= libspectrum_tape_read( read_back, buffer, length,
                                       LIBSPECTRUM_ID_TAPE_PZX, NULL );
        libspectrum_free( buffer );
      } else error = 1;
      for( i = 0; i < 5; i++ ) {
        error |= libspectrum_tape_get_next_edge( &edge, read_back );
        expected_total += intervals[i];
        total += edge.tstates;
        error |= total != expected_total * 3500000 / speed;
        error |= edge.level != ( high ^ ( i & 1 ) );
      }
      libspectrum_tape_free( read_back );
      libspectrum_tape_free( test_tape );
    }
  }

  /* A hold longer than PZX's 31-bit duration must not create extra edges. */
  machine_current->timings.processor_speed = 3500000;
  test_tape = libspectrum_tape_alloc();
  tape_record_set_tape( test_tape );
  tstates = 0;
  writeport_internal( 0xfe, ULA_PORT_MIC_BIT );
  tape_record_start();
  tape_record_frame( 0x80000000 );
  tstates = 100;
  writeport_internal( 0xfe, 0 );
  tstates += 5;
  error |= tape_record_stop();
  read_back = libspectrum_tape_alloc();
  buffer = NULL;
  length = 0;
  error |= libspectrum_tape_write( &buffer, &length, test_tape,
                                  LIBSPECTRUM_ID_TAPE_PZX );
  if( buffer ) {
    error |= libspectrum_tape_read( read_back, buffer, length,
                                   LIBSPECTRUM_ID_TAPE_PZX, NULL );
    libspectrum_free( buffer );
  } else error = 1;
  {
    libspectrum_tape_edge edge;
    const libspectrum_dword expected[] = { 0x7fffffff, 101, 5 };
    for( i = 0; i < 3; i++ ) {
      error |= libspectrum_tape_get_next_edge( &edge, read_back );
      error |= edge.tstates != expected[i] || edge.level != ( i < 2 );
    }
  }
  libspectrum_tape_free( test_tape );

  libspectrum_tape_free( read_back );

  /* Grow the capture buffer and coalesce a long train of equal pulses. */
  test_tape = libspectrum_tape_alloc();
  tape_record_set_tape( test_tape );
  writeport_internal( 0xfe, 0 );
  tape_record_start();
  for( i = 0; i < 2000; i++ ) {
    tstates += 2;
    writeport_internal( 0xfe, ula_last_byte() ^ ULA_PORT_MIC_BIT );
  }
  tstates += 3;
  error |= tape_record_stop();
  block = libspectrum_tape_current_block( test_tape );
  error |= libspectrum_tape_block_count( block ) != 1 ||
           libspectrum_tape_block_pulse_lengths( block, 0 ) != 2 ||
           libspectrum_tape_block_pulse_repeats( block, 0 ) != 2000;
  libspectrum_tape_free( test_tape );

  /* Starting and stopping without elapsed time creates no bogus pulse. */
  test_tape = libspectrum_tape_alloc();
  tape_record_set_tape( test_tape );
  tape_record_start();
  error |= tape_record_stop() || libspectrum_tape_present( test_tape );
  libspectrum_tape_free( test_tape );
  tape_record_set_tape( tape );
  machine_current->timings.processor_speed = saved_speed;
  tstates = saved_time;
  writeport_internal( 0xfe, saved_ula );
  tape_modified = saved_modified;
  return error;
}

static int
tape_record_lifecycle_unittest( void )
{
  libspectrum_tape *saved_tape = tape;
  libspectrum_machine saved_machine = machine_current->machine;
  int saved_modified = tape_modified;
  int error = 0, action;
  for( action = 0; action < 4; action++ ) {
    libspectrum_tape_block *block;
    libspectrum_snap *snap = NULL;
    tape = libspectrum_tape_alloc();
    tape_record_set_tape( tape );
    if( action == 2 ) {
      snap = libspectrum_snap_alloc();
      error |= snapshot_copy_to( snap );
    }
    writeport_internal( 0xfe, ULA_PORT_MIC_BIT );
    tape_record_start();
    tstates += 100;
    if( action == 0 ) error |= machine_reset( 0 );
    else if( action == 1 ) error |= machine_select( LIBSPECTRUM_MACHINE_128 );
    else if( action == 2 ) error |= snapshot_copy_from( snap );
    else error |= tape_close();
    error |= tape_recording;
    block = libspectrum_tape_current_block( tape );
    if( action == 3 ) error |= libspectrum_tape_present( tape );
    else error |= !block;
    if( block ) {
      error |= libspectrum_tape_block_type( block ) != LIBSPECTRUM_TAPE_BLOCK_PAUSE;
      error |= libspectrum_tape_block_level( block ) != 1;
      error |= libspectrum_tape_block_pause_tstates( block ) == 0;
    }
    if( snap ) libspectrum_snap_free( snap );
    libspectrum_tape_free( tape );
    tape = saved_tape;
    tape_record_set_tape( tape );
  }
  error |= machine_select( saved_machine );
  tape_modified = saved_modified;
  return error;
}

/* Complete normal frame processing between observations. The DI/HALT return
   stub keeps the result stable for the remainder of the final frame. */
static int
run_rom_tape_routine( libspectrum_word pc )
{
  unsigned int frames;
  writebyte_internal( 0x9000, 0xf3 ); /* DI */
  writebyte_internal( 0x9001, 0x76 ); /* HALT */
  SP = 0xaffc;
  writebyte_internal( SP, 0x00 );
  writebyte_internal( SP + 1, 0x90 );
  PC = pc;
  z80.halted = z80.iff1 = z80.iff2 = 0;
  for( frames = 0; frames < 1200; frames++ ) {
    spectrum_do_frame();
    if( z80.halted && PC == 0x9001 ) return 0;
  }
  printf( "ROM tape routine %04x timed out at PC=%04x tstates=%u BC=%04x HL=%04x RZX=%d/%d\n",
          pc, PC, tstates, BC, HL, rzx_playback, rzx_recording );
  return 1;
}

static int
tape_record_rom_roundtrip_unittest( void )
{
  static const libspectrum_byte payload[] = {
    0x00, 0xff, 0x55, 0xaa, 0x01, 0x80, 0xfe, 0x7f,
    0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x10
  };
  libspectrum_snap *saved_snapshot = libspectrum_snap_alloc();
  libspectrum_tape *saved_tape = tape, *captured = NULL, *loaded = NULL;
  libspectrum_byte *buffer = NULL;
  size_t length, i;
  int saved_traps = settings_current.tape_traps;
  int saved_acceleration = settings_current.accelerate_loader;
  int saved_detection = settings_current.detect_loader;
  int saved_fastload = settings_current.fastload;
  int saved_modified = tape_modified;
  int error = snapshot_copy_to( saved_snapshot ), model;
  settings_current.tape_traps = settings_current.accelerate_loader = 0;
  settings_current.detect_loader = settings_current.fastload = 0;
  for( model = 0; model < 2 && !error; model++ ) {
    error = machine_select( model ? LIBSPECTRUM_MACHINE_128 : LIBSPECTRUM_MACHINE_48 );
    if( error ) break;
    if( model ) writeport_internal( 0x7ffd, 0x10 ); /* 128K's 48 BASIC ROM */
    captured = libspectrum_tape_alloc();
    loaded = libspectrum_tape_alloc();
    tape = captured;
    tape_record_set_tape( captured );
    for( i = 0; i < sizeof( payload ); i++ )
      writebyte_internal( 0x8000 + i, payload[i] );
    IX = 0x8000;
    DE = sizeof( payload );
    IY = 0x5c3a;
    AF = 0xff00;
    tape_record_start();
    error |= run_rom_tape_routine( 0x04c2 ); /* SA-BYTES */
    error |= tape_record_stop();
    /* Capturing real MIC transitions must not have inserted a ROM data
       block through a trap. */
    if( !error ) {
      libspectrum_tape_block *block = libspectrum_tape_current_block( captured );
      error |= !block;
      if( block ) error |= libspectrum_tape_block_type( block ) !=
                           LIBSPECTRUM_TAPE_BLOCK_PULSE_SEQUENCE;
    }
    length = 0;
    buffer = NULL;
    if( !error ) error = libspectrum_tape_write( &buffer, &length, captured,
                                                LIBSPECTRUM_ID_TAPE_PZX );
    if( !error ) error = libspectrum_tape_read( loaded, buffer, length,
                                               LIBSPECTRUM_ID_TAPE_PZX, NULL );
    libspectrum_free( buffer );
    buffer = NULL;
    tape = loaded;
    tape_record_set_tape( loaded );
    for( i = 0; i < sizeof( payload ); i++ ) writebyte_internal( 0x8000 + i, 0 );
    IX = 0x8000;
    DE = sizeof( payload );
    AF = 0xff01; /* expected flag FF, carry set selects LOAD rather than VERIFY */
    if( !error ) error = tape_play( 0 );
    if( !error ) error = run_rom_tape_routine( 0x0556 ); /* LD-BYTES */
    if( !error ) {
      error |= !( F & 1 ) || DE != 0 || IX != 0x8000 + sizeof( payload );
      for( i = 0; i < sizeof( payload ); i++ )
        error |= readbyte_internal( 0x8000 + i ) != payload[i];
    }
    tape_stop();
    libspectrum_tape_free( captured );
    libspectrum_tape_free( loaded );
    captured = loaded = NULL;
    tape = saved_tape;
    tape_record_set_tape( tape );
  }
  tape = saved_tape;
  tape_record_set_tape( tape );
  error |= snapshot_copy_from( saved_snapshot );
  libspectrum_snap_free( saved_snapshot );
  settings_current.tape_traps = saved_traps;
  settings_current.accelerate_loader = saved_acceleration;
  settings_current.detect_loader = saved_detection;
  settings_current.fastload = saved_fastload;
  tape_modified = saved_modified;
  if( error ) printf( "ROM SAVE/PZX/LOAD round trip failed\n" );
  return error;
}

static int
tape_record_rzx_exclusion_unittest( void )
{
  int saved_playback = rzx_playback, saved_recording = rzx_recording;
  libspectrum_tape *target = libspectrum_tape_alloc();
  int error = 0;
  tape_record_set_tape( target );
  rzx_playback = 1;
  rzx_recording = 0;
  tape_record_start();
  error |= tape_recording;
  rzx_playback = 0;
  rzx_recording = 1;
  tape_record_start();
  error |= tape_recording;
  rzx_recording = 0;
  tape_record_start();
  error |= !tape_recording;
  /* Reverse direction must reject before opening/parsing anything. */
  error |= !rzx_start_playback( "missing", 1 );
  error |= !rzx_start_playback_from_buffer( NULL, 0 );
  error |= !rzx_start_recording( "unused.rzx", 1 );
  error |= !rzx_continue_recording( "missing" );
  error |= tape_record_stop();
  rzx_playback = saved_playback;
  rzx_recording = saved_recording;
  libspectrum_tape_free( target );
  tape_record_set_tape( tape );
  return error;
}

static libspectrum_dword edge_test_tstates;

static void
capture_tape_edge_event( gpointer data, gpointer user_data )
{
  event_t *event = data;

  if( event->type == tape_edge_event ) edge_test_tstates = event->tstates;
}

static int
tape_edge_unittest( void )
{
  libspectrum_tape_edge edge = { 0 };
  libspectrum_tape_block *rom, *pause;
  libspectrum_machine saved_machine = machine_current->machine;
  libspectrum_dword saved_speed = machine_current->timings.processor_speed;
  libspectrum_dword total, expected;
  unsigned int i;
  int saved_playing = tape_playing;
  int saved_pending = tape_stop_pending;
  int saved_blocked = tape_autoplay_blocked;
  int saved_autoplay = tape_autoplay;
  int saved_traps = settings_current.tape_traps;
  int saved_recording = rzx_recording;
  int saved_microphone = tape_microphone;
  int error = 0;

  edge.flags = LIBSPECTRUM_TAPE_FLAGS_STOP48;
  machine_current->machine = LIBSPECTRUM_MACHINE_48;
  error |= !tape_edge_requests_stop( &edge );
  machine_current->machine = LIBSPECTRUM_MACHINE_128;
  error |= tape_edge_requests_stop( &edge );

  tape_stop_pending = tape_autoplay_blocked = 0;
  edge.flags = LIBSPECTRUM_TAPE_FLAGS_STOP;
  tape_handle_stop_request( &edge );
  error |= !tape_stop_pending || tape_autoplay_blocked;
  tape_stop_pending = tape_autoplay_blocked = 0;
  edge.flags = LIBSPECTRUM_TAPE_FLAGS_STOP |
               LIBSPECTRUM_TAPE_FLAGS_BLOCK;
  tape_handle_stop_request( &edge );
  error |= !tape_stop_pending || !tape_autoplay_blocked;

  rom = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_ROM );
  pause = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PAUSE );
  if( !rom || !pause ) {
    if( rom ) libspectrum_tape_block_free( rom );
    if( pause ) libspectrum_tape_block_free( pause );
    error = 1;
    goto done;
  }
  tape_autoplay = settings_current.tape_traps = 1;
  rzx_recording = 0;
  error |= !tape_should_stop_for_rom_block( rom );
  error |= tape_should_stop_for_rom_block( pause );
  rzx_recording = 1;
  error |= tape_should_stop_for_rom_block( rom );
  libspectrum_tape_block_free( rom );
  libspectrum_tape_block_free( pause );

  edge.level = LIBSPECTRUM_TAPE_SIGNAL_HIGH;
  tape_update_microphone( &edge );
  error |= tape_microphone != LIBSPECTRUM_TAPE_SIGNAL_HIGH;

  event_remove_type( tape_edge_event );
  machine_current->timings.processor_speed = 3500000;
  edge.tstates = 123;
  edge.flags = LIBSPECTRUM_TAPE_FLAGS_LENGTH_SHORT;
  edge_test_tstates = 0;
  tape_schedule_edge( 1000, &edge, 0 );
  event_foreach( capture_tape_edge_event, NULL );
  error |= edge_test_tstates != 1123;
  event_remove_type( tape_edge_event );

  /* Conversion belongs at scheduling, for both ordinary and accelerated
     edges. Carry fractions across block boundaries and zero-time edges. */
  machine_current->timings.processor_speed = 3546900;
  total = 0;
  edge.tstates = 123;
  for( i = 0; i < 10000; i++ ) {
    tape_schedule_edge( total, &edge, i & 1 );
    event_foreach( capture_tape_edge_event, NULL );
    total = edge_test_tstates;
    event_remove_type( tape_edge_event );
  }
  expected = (libspectrum_qword)123 * 10000 * 3546900 / 3500000;
  error |= total != expected;
  edge.tstates = 0;
  tape_schedule_edge( total, &edge, 0 );
  event_foreach( capture_tape_edge_event, NULL );
  error |= edge_test_tstates != total;
  event_remove_type( tape_edge_event );

  /* A one-second pause has the same physical duration at either clock. */
  machine_current->timings.processor_speed = 3500000;
  edge.tstates = 3500000;
  tape_schedule_edge( 1000, &edge, 0 );
  event_foreach( capture_tape_edge_event, NULL );
  error |= edge_test_tstates != 3501000;
  event_remove_type( tape_edge_event );
  machine_current->timings.processor_speed = 3546900;
  tape_schedule_edge( 1000, &edge, 0 );
  event_foreach( capture_tape_edge_event, NULL );
  error |= edge_test_tstates != 3547900;

  /* Stop/resume must not convert an already scheduled interval again. */
  tape_playing = 1;
  tape_stop();
  tape_play( 0 );
  event_foreach( capture_tape_edge_event, NULL );
  error |= edge_test_tstates != 3547900;
  tape_stop();
  event_remove_type( tape_edge_event );

  /* Changing clock while stopped rescales the saved partial interval. */
  machine_current->timings.processor_speed = 3500000;
  tape_play( 0 );
  event_foreach( capture_tape_edge_event, NULL );
  expected = tstates + (libspectrum_qword)(3547900 - tstates) *
                       3500000 / 3546900;
  error |= edge_test_tstates != expected;
  tape_stop();
  event_remove_type( tape_edge_event );

done:
  machine_current->machine = saved_machine;
  machine_current->timings.processor_speed = saved_speed;
  tape_playing = saved_playing;
  tape_stop_pending = saved_pending;
  tape_autoplay_blocked = saved_blocked;
  tape_autoplay = saved_autoplay;
  settings_current.tape_traps = saved_traps;
  rzx_recording = saved_recording;
  tape_microphone = saved_microphone;
  return error;
}

static int
tape_select_rewind_write_unittest( void )
{
  char filename[ PATH_MAX ];
  libspectrum_tape *saved_tape = tape;
  libspectrum_tape *test_tape = NULL, *read_back = NULL;
  libspectrum_tape_block *rom = NULL, *pause = NULL;
  libspectrum_tape_block *block;
  libspectrum_tape_iterator iterator;
  libspectrum_byte *data = NULL;
  utils_file file;
  int saved_modified = tape_modified;
  int saved_blocked = tape_autoplay_blocked;
  int saved_resume = trap_resume_pending;
  char *saved_autoload = settings_current.phantom_typist_mode;
  int fd, length;
  int file_created = 0;
  int r = 0;

  memset( &file, 0, sizeof( file ) );

  /* Selecting the machine resets the memory map, so the autoload decision
     below is deterministic */
  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) return 1;

  /* A tape without blocks cannot be navigated */
  test_tape = libspectrum_tape_alloc();
  if( !test_tape ) return 1;
  tape = test_tape;
  r |= tape_get_current_block() != -1;
  r |= tape_rewind() != 0;
  r |= tape_get_current_block() != -1;
  r |= tape_select_block( 0 ) == 0;
  libspectrum_tape_free( test_tape );
  test_tape = NULL;
  tape = saved_tape;

  /* Build a two-block tape */
  test_tape = libspectrum_tape_alloc();
  rom = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_ROM );
  pause = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_PAUSE );
  data = libspectrum_new( libspectrum_byte, 2 );
  if( !test_tape || !rom || !pause || !data ) {
    libspectrum_free( data );
    libspectrum_tape_block_free( rom );
    libspectrum_tape_block_free( pause );
    if( test_tape ) libspectrum_tape_free( test_tape );
    test_tape = NULL;
    tape = saved_tape;
    return 1;
  }
  data[0] = data[1] = 0x80;
  libspectrum_tape_block_set_data_length( rom, 2 );
  libspectrum_tape_block_set_data( rom, data );
  libspectrum_tape_block_set_pause_tstates( rom, 3500000 );
  libspectrum_tape_block_set_pause( pause, 1 );
  libspectrum_tape_block_set_level( pause, 0 );
  if( libspectrum_tape_append_block( test_tape, rom ) ) {
    libspectrum_tape_block_free( rom );
    libspectrum_tape_block_free( pause );
    libspectrum_tape_free( test_tape );
    test_tape = NULL;
    tape = saved_tape;
    return 1;
  }
  if( libspectrum_tape_append_block( test_tape, pause ) ) {
    libspectrum_tape_block_free( pause );
    libspectrum_tape_free( test_tape );
    test_tape = NULL;
    tape = saved_tape;
    return 1;
  }
  tape = test_tape;

  /* The first block is selected as soon as the tape has one */
  r |= tape_get_current_block() != 0;

  /* Rewinding reselects the first block */
  r |= tape_rewind() != 0;
  r |= tape_get_current_block() != 0;

  /* Selecting a block clears the autoplay block and any pending trap
     resume; selecting without the browser update keeps the autoplay
     block */
  tape_autoplay_blocked = 1;
  trap_resume_pending = 1;
  r |= tape_select_block( 1 ) != 0;
  r |= tape_get_current_block() != 1 ||
       tape_autoplay_blocked || trap_resume_pending;

  tape_autoplay_blocked = 1;
  r |= tape_select_block_no_update( 0 ) != 0;
  r |= tape_get_current_block() != 0 || !tape_autoplay_blocked ||
       trap_resume_pending;

  /* Selecting past the end of the tape fails and leaves the position */
  r |= tape_select_block( 5 ) == 0;
  r |= tape_get_current_block() != 0;

  /* Autoload follows the phantom typist setting */
  settings_current.phantom_typist_mode = "Auto";
  r |= tape_can_autoload() == 0;
  settings_current.phantom_typist_mode = "Disabled";
  r |= tape_can_autoload() != 0;
  settings_current.phantom_typist_mode = saved_autoload;

  /* Write the tape out to a tape file (the filename has no tape extension,
     so tape_write defaults to TZX) and read it back */
  length = snprintf( filename, sizeof( filename ),
                     "%s%sfuse-tape-write-test-XXXXXX",
                     compat_get_temp_path(), FUSE_DIR_SEP_STR );
  if( length < 0 || (size_t)length >= sizeof( filename ) ) {
    r = 1;
    goto done;
  }
  fd = mkstemp( filename );
  if( fd < 0 ) {
    r = 1;
    goto done;
  }
  file_created = 1;
  if( close( fd ) || tape_write( filename ) ) {
    r = 1;
    goto done;
  }
  r |= tape_modified != 0;
  if( utils_read_file( filename, &file ) ) {
    r = 1;
    goto done;
  }
  read_back = libspectrum_tape_alloc();
  if( !read_back ||
      libspectrum_tape_read( read_back, file.buffer, file.length,
                             LIBSPECTRUM_ID_UNKNOWN, NULL ) ) {
    r = 1;
    goto done;
  }

  r |= libspectrum_tape_count( read_back ) != 2;

  block = libspectrum_tape_iterator_init( &iterator, read_back );
  r |= !block ||
       libspectrum_tape_block_type( block ) != LIBSPECTRUM_TAPE_BLOCK_ROM ||
       libspectrum_tape_block_data_length( block ) != 2 ||
       libspectrum_tape_block_data( block )[0] != 0x80 ||
       libspectrum_tape_block_data( block )[1] != 0x80;

  block = libspectrum_tape_iterator_next( &iterator );
  r |= !block ||
       libspectrum_tape_block_type( block ) != LIBSPECTRUM_TAPE_BLOCK_PAUSE;

done:
  if( file_created && unlink( filename ) ) r = 1;
  if( read_back ) libspectrum_tape_free( read_back );
  utils_file_free( &file );
  if( test_tape ) libspectrum_tape_free( test_tape );
  tape = saved_tape;
  tape_modified = saved_modified;
  tape_autoplay_blocked = saved_blocked;
  trap_resume_pending = saved_resume;
  settings_current.phantom_typist_mode = saved_autoload;
  return r;
}

int
tape_unittest( void )
{
  tape_test_fixture fixture = { 0 };
  int error;

  fixture.saved_tape = tape;
  fixture.saved_microphone = tape_microphone;
  error = tape_test_create_rom_pause_tape( &fixture );

  if( !error ) {
    tape = fixture.test_tape;
    tape_microphone = 0;
    error = check_trap_pause_position();
  }
  if( !error ) error = check_pause_playback();
  if( !error ) error = tape_block_details_unittest();
  if( !error ) error = trap_load_unittest();
  if( !error ) error = pzx_trap_unittest();
  if( !error ) error = tape_record_unittest();
  if( !error ) error = tape_record_lifecycle_unittest();
  if( !error ) error = tape_record_rzx_exclusion_unittest();
  if( !error ) error = tape_record_rom_roundtrip_unittest();
  if( !error ) error = tape_edge_unittest();
  if( !error ) error = tape_select_rewind_write_unittest();

  tape_test_cleanup( &fixture );
  if( error ) printf( "tape_unittest failed\n" );
  return error;
}
