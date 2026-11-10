/* tape_traps.c: ROM tape trap handling routines
   Copyright (c) 1999-2017 Philip Kendall, Darren Salt, Witold Filipczyk
   Copyright (c) 2015-2018 UB880D
   Copyright (c) 2016-2021 Fredrick Meunier
   Copyright (c) 2026 Alberto Garcia

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "config.h"

#include "libspectrum.h"

#include "machine.h"
#include "memory_pages.h"
#include "phantom_typist.h"
#include "rzx.h"
#include "settings.h"
#include "tape.h"
#include "tape_internals.h"
#include "ui/ui.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"

/* Length of a standard ZX Spectrum ROM tape header block (flag byte +
   1 type + 10 name + 2 length + 2 param1 + 2 param2 + 1 parity = 19 bytes) */
#define TAPE_ROM_HEADER_LEN 19

/* Flag byte for a ZX Spectrum ROM tape header block */
#define TAPE_ROM_HEADER_FLAG 0x00

/* Pause appended after each ROM-routine tape-save block (milliseconds) */
#define TAPE_ROM_SAVE_PAUSE_MS 1000

int tape_trap_load_block( libspectrum_tape_block *block,
                          size_t *bytes_consumed );
static libspectrum_error tape_trap_advance_data_edges( size_t data_edges );

static int
does_tape_load_with_code( void )
{
  libspectrum_tape_block *block;
  libspectrum_tape_iterator iterator;
  int needs_code = 0;

  for( block = libspectrum_tape_iterator_init( &iterator, tape );
       block;
       block = libspectrum_tape_iterator_next( &iterator ) ) {

    libspectrum_tape_type block_type;
    size_t block_length;
    libspectrum_byte *data;

    /* Skip over blocks until we find one which might be a header */
    block_type = libspectrum_tape_block_type( block );
    if( block_type != LIBSPECTRUM_TAPE_BLOCK_ROM &&
        block_type != LIBSPECTRUM_TAPE_BLOCK_DATA_BLOCK )
      continue;

    /* For this to be a CODE block, the block must have the right
       length, it must have the header flag set and it must indicate
       a CODE block */
    block_length = libspectrum_tape_block_data_length( block );
    data = libspectrum_tape_block_data( block );
    needs_code =
      (block_length == TAPE_ROM_HEADER_LEN) &&
      (data[0] == TAPE_ROM_HEADER_FLAG) &&
      (data[1] == 0x03);

    /* Stop looking now - either we found an appropriate block or we found
       something strange, in which case we'll just assume it loads normally */
    break;
  }

  return needs_code;
}

/* Type the right command to start the current tape autoloading */
int
tape_autoload( libspectrum_machine hardware )
{
  int needs_code = does_tape_load_with_code();
  machine_reset( 0 );
  phantom_typist_activate( hardware, needs_code );
  return 0;
}

/* Load the next tape block into memory; returns 0 if a block was
   loaded (even if it had an tape loading error or equivalent) or
   non-zero if there was an error at the emulator level, or tape traps
   are not active */
static int
tape_trap_precheck( void )
{
  if( !settings_current.tape_traps || tape_playing ||
      rzx_playback || rzx_recording )
    return 2;
  if( !trap_check_rom( CHECK_TAPE_ROM ) ) return 3;
  if( !libspectrum_tape_present( tape ) ) return 1;
  return 0;
}

static int
consume_pending_trap_interval( libspectrum_tape_block **block )
{
  libspectrum_tape_edge edge;
  int error;

  if( !trap_resume_pending ) return 0;
  /* PZX completion leaves the cursor at DATA's tail, not a ROM pause. */
  if( !tape_trap_pzx_data( *block ) &&
      libspectrum_tape_state( tape ) != LIBSPECTRUM_TAPE_STATE_PAUSE )
    return 0;

  error = libspectrum_tape_get_next_edge( &edge, tape );
  if( error ) return error;
  tape_update_microphone( &edge );
  trap_resume_pending = 0;
  ui_tape_browser_update( UI_TAPE_BROWSER_SELECT_BLOCK, NULL );
  if( tape_edge_requests_stop( &edge ) ) {
    tape_handle_stop_request( &edge );
    tape_stop_pending = 0; /* Already stopped; no edge event will service it. */
    return 1;
  }
  *block = libspectrum_tape_current_block( tape );
  return 0;
}

static int
skip_metadata_blocks( libspectrum_tape_block **block )
{
  while( libspectrum_tape_block_metadata( *block ) ) {
    *block = libspectrum_tape_select_next_block( tape );
    if( !*block ) return 1;
  }
  return 0;
}

/* Deliberately exact: custom encodings must retain waveform playback. */
int
tape_trap_pzx_data( libspectrum_tape_block *block )
{
  if( !block || libspectrum_tape_block_type( block ) !=
      LIBSPECTRUM_TAPE_BLOCK_DATA_BLOCK ) return 0;
  if( libspectrum_tape_block_data_length( block ) < 2 ||
      libspectrum_tape_block_bits_in_last_byte( block ) != 8 ||
      libspectrum_tape_block_bit0_pulse_count( block ) != 2 ||
      libspectrum_tape_block_bit1_pulse_count( block ) != 2 ) return 0;
  return libspectrum_tape_block_bit0_pulses( block, 0 ) == 855 &&
         libspectrum_tape_block_bit0_pulses( block, 1 ) == 855 &&
         libspectrum_tape_block_bit1_pulses( block, 0 ) == 1710 &&
         libspectrum_tape_block_bit1_pulses( block, 1 ) == 1710;
}

static int
pzx_has_rom_pilot( libspectrum_tape_block *block )
{
  size_t repeats;

  if( !block || libspectrum_tape_block_type( block ) !=
      LIBSPECTRUM_TAPE_BLOCK_PULSE_SEQUENCE ||
      libspectrum_tape_block_count( block ) != 3 ) return 0;
  repeats = libspectrum_tape_block_pulse_repeats( block, 0 );
  return ( repeats == 8063 || repeats == 3223 ) &&
         libspectrum_tape_block_pulse_lengths( block, 0 ) == 2168 &&
         libspectrum_tape_block_pulse_lengths( block, 1 ) == 667 &&
         libspectrum_tape_block_pulse_lengths( block, 2 ) == 735 &&
         libspectrum_tape_block_pulse_repeats( block, 1 ) == 1 &&
         libspectrum_tape_block_pulse_repeats( block, 2 ) == 1;
}

/* Unlike the general metadata predicate, exclude loop/control instructions. */
static int
pzx_trap_metadata( libspectrum_tape_block *block )
{
  switch( libspectrum_tape_block_type( block ) ) {
  case LIBSPECTRUM_TAPE_BLOCK_GROUP_START:
  case LIBSPECTRUM_TAPE_BLOCK_GROUP_END:
  case LIBSPECTRUM_TAPE_BLOCK_COMMENT:
  case LIBSPECTRUM_TAPE_BLOCK_MESSAGE:
  case LIBSPECTRUM_TAPE_BLOCK_ARCHIVE_INFO:
  case LIBSPECTRUM_TAPE_BLOCK_HARDWARE:
  case LIBSPECTRUM_TAPE_BLOCK_CUSTOM:
  case LIBSPECTRUM_TAPE_BLOCK_CONCAT:
    return 1;
  default:
    return 0;
  }
}

static int
pzx_trap_can_skip( libspectrum_tape_block *block )
{
  return pzx_trap_metadata( block ) ||
         ( libspectrum_tape_block_type( block ) == LIBSPECTRUM_TAPE_BLOCK_PAUSE &&
           libspectrum_tape_block_pause_tstates( block ) );
}

/* Read-only lookahead: no playback or trap state changes on rejection. */
static libspectrum_tape_block *
find_pzx_rom_data( libspectrum_tape *source, size_t *position )
{
  libspectrum_tape_block *block;
  libspectrum_tape_iterator iterator;
  int current;
  size_t index = 0;

  if( libspectrum_tape_position( &current, source ) ) return NULL;
  block = libspectrum_tape_iterator_init( &iterator, source );
  while( block && index < (size_t)current ) {
    block = libspectrum_tape_iterator_next( &iterator );
    index++;
  }
  while( block && pzx_trap_can_skip( block ) ) {
    block = libspectrum_tape_iterator_next( &iterator );
    index++;
  }
  if( !pzx_has_rom_pilot( block ) ) return NULL;
  block = libspectrum_tape_iterator_next( &iterator );
  index++;
  while( block && pzx_trap_metadata( block ) ) {
    block = libspectrum_tape_iterator_next( &iterator );
    index++;
  }
  if( !tape_trap_pzx_data( block ) ) return NULL;
  *position = index;
  return block;
}

static void
skip_pzx_pilot( libspectrum_tape_block **block )
{
  size_t position;
  libspectrum_tape_block *data = find_pzx_rom_data( tape, &position );

  if( !data ||
      libspectrum_tape_block_data_length( data ) != (size_t)DE + 2 ) return;
  if( libspectrum_tape_nth_block( tape, position ) ) return;
  *block = libspectrum_tape_current_block( tape );
  trap_resume_pending = 0;
  trap_pzx_ready = 1;
  ui_tape_browser_update( UI_TAPE_BROWSER_SELECT_BLOCK, NULL );
}

static int
block_requires_playback( libspectrum_tape_block *block )
{
  libspectrum_tape_block *next_block;
  size_t length;

  if( tape_trap_pzx_data( block ) )
    return !trap_pzx_ready ||
           libspectrum_tape_block_data_length( block ) != (size_t)DE + 2;

  if( libspectrum_tape_block_type( block ) != LIBSPECTRUM_TAPE_BLOCK_ROM ||
      libspectrum_tape_state( tape ) != LIBSPECTRUM_TAPE_STATE_PILOT )
    return 1;

  next_block = libspectrum_tape_peek_next_block( tape );
  length = libspectrum_tape_block_data_length( block );
  return length > DE + 2 ||
         ( length < DE + 2 && libspectrum_tape_block_type( next_block ) !=
                              LIBSPECTRUM_TAPE_BLOCK_ROM );
}

static void
set_load_return_pc( void )
{
  /* All returns made via the RET at #05E2, except on Timex 2068 at #0136 */
  if( machine_current->machine == LIBSPECTRUM_MACHINE_TC2068 ||
      machine_current->machine == LIBSPECTRUM_MACHINE_TS2068 )
    PC = 0x0136;
  else
    PC = 0x05e2;
}

static int
finish_trapped_load( size_t bytes_consumed )
{
  libspectrum_tape_block *next_block =
    libspectrum_tape_peek_next_block( tape );
  int error;

  /* Preserve the waveform position when loaded code may take over at the
     trailing pause after a load ending at the top of memory. */
  if( ( F & FLAG_C ) && IX == 0xffff && next_block &&
      libspectrum_tape_block_type( next_block ) ==
        LIBSPECTRUM_TAPE_BLOCK_ROM ) {
    error = tape_trap_advance_data_edges( bytes_consumed * 16 );
    if( error ) return error;
    trap_resume_pending = 1;
    return 0;
  }

  if( libspectrum_tape_block_type( next_block ) !=
      LIBSPECTRUM_TAPE_BLOCK_ROM )
    return tape_trap_finish_rom_block();

  next_block = libspectrum_tape_select_next_block( tape );
  if( !next_block ) return 1;
  ui_tape_browser_update( UI_TAPE_BROWSER_SELECT_BLOCK, NULL );
  return 0;
}

static int
finish_pzx_trapped_load( libspectrum_tape_block *block, size_t bytes_consumed )
{
  /* Stop exactly after the consumed bits. The ordinary edge engine owns
     the tail, separate PAUS/STOP blocks, and any unconsumed data. */
  int error = tape_trap_advance_data_edges( bytes_consumed * 16 );

  if( error ) return error;
  trap_pzx_ready = 0;
  if( bytes_consumed == libspectrum_tape_block_data_length( block ) ) {
    trap_resume_pending = 1;
    return 0;
  }
  return tape_play( 1 );
}

int
tape_load_trap( void )
{
  libspectrum_tape_block *block;
  size_t bytes_consumed;
  int error;

  error = tape_trap_precheck();
  if( error ) return error;

  block = libspectrum_tape_current_block( tape );
  error = consume_pending_trap_interval( &block );
  if( error ) return error;
  error = skip_metadata_blocks( &block );
  if( error ) return error;

  skip_pzx_pilot( &block );

  /* Custom loaders and unsupported partial loads observe normal playback. */
  if( block_requires_playback( block ) ) {
    tape_play( 1 );
    return -1;
  }

  phantom_typist_deactivate();
  set_load_return_pc();

  error = tape_trap_load_block( block, &bytes_consumed );
  if( error ) return error;
  trap_resume_pending = 0;
  if( tape_trap_pzx_data( block ) )
    return finish_pzx_trapped_load( block, bytes_consumed );
  return finish_trapped_load( bytes_consumed );
}

libspectrum_error
tape_trap_finish_rom_block( void )
{
  libspectrum_tape_cursor *cursor = libspectrum_tape_cursor_capture( tape );
  libspectrum_tape_cursor *preview = NULL;
  libspectrum_tape_state_type state;
  libspectrum_tape_edge edge;
  libspectrum_error error = LIBSPECTRUM_ERROR_NONE;

  if( !cursor ) return LIBSPECTRUM_ERROR_MEMORY;
  do {
    error = libspectrum_tape_cursor_state( &state, cursor );
    if( error || state == LIBSPECTRUM_TAPE_STATE_PAUSE ) break;
    error = libspectrum_tape_cursor_get_next_edge( &edge, cursor );
  } while( !error );

  /* The trapped ROM has consumed the final data pulse. Preview the ordinary
     pause edge to obtain its absolute handoff level without consuming it. */
  if( !error ) preview = libspectrum_tape_cursor_clone( cursor );
  if( !error && !preview ) error = LIBSPECTRUM_ERROR_MEMORY;
  if( !error ) error = libspectrum_tape_cursor_get_next_edge( &edge, preview );
  if( !error ) error = libspectrum_tape_cursor_apply( tape, cursor );
  libspectrum_tape_cursor_free( preview );
  libspectrum_tape_cursor_free( cursor );
  if( !error ) tape_microphone = edge.level;
  return error;
}

static libspectrum_error
tape_trap_advance_data_edges( size_t data_edges )
{
  libspectrum_tape_cursor *cursor;
  libspectrum_error error = LIBSPECTRUM_ERROR_NONE;

  cursor = libspectrum_tape_cursor_capture( tape );
  if( !cursor ) return LIBSPECTRUM_ERROR_MEMORY;

  /* The edge engine tracks absolute signal level, including PZX's initial
     level. Do not mutate the live position or EAR until traversal succeeds. */
  while( data_edges ) {
    libspectrum_tape_edge edge;

    error = libspectrum_tape_cursor_get_next_edge( &edge, cursor );
    if( error ) goto done;
    if( edge.flags & ( LIBSPECTRUM_TAPE_FLAGS_LENGTH_SHORT |
                       LIBSPECTRUM_TAPE_FLAGS_LENGTH_LONG ) )
      data_edges--;
  }

  error = libspectrum_tape_cursor_apply( tape, cursor );
  if( !error ) {
    libspectrum_tape_signal_level level;
    error = libspectrum_tape_signal_level_get( &level, tape );
    if( !error ) tape_microphone = level;
  }

done:
  libspectrum_tape_cursor_free( cursor );
  return error;
}

typedef struct trap_load_state {
  libspectrum_byte *data;
  libspectrum_byte parity;
  int length;
  int read;
  int processed;
  int verify;
  size_t *bytes_consumed;
} trap_load_state;

static void
trap_load_finish( trap_load_state *state )
{
  /* At this point, AF, AF', B and L are already modified */
  C = 1;
  H = state->parity;
  DE -= state->processed;
  IX += state->processed;
}

static void
trap_load_fail( trap_load_state *state )
{
  F &= ~FLAG_C;
  trap_load_finish( state );
}

static int
trap_verify_bytes( trap_load_state *state )
{
  for( state->processed = 0; state->processed < state->read;
       state->processed++ ) {
    state->parity ^= state->data[ state->processed ];
    if( state->data[ state->processed ] !=
        readbyte_internal( IX + state->processed ) ) {
      L = state->data[ state->processed ];
      *state->bytes_consumed = state->processed + 2;
      return 1;
    }
  }
  return 0;
}

static void
trap_copy_bytes( trap_load_state *state )
{
  for( state->processed = 0; state->processed < state->read;
       state->processed++ ) {
    state->parity ^= state->data[ state->processed ];
    writebyte_internal( IX + state->processed,
                        state->data[ state->processed ] );
  }
}

static void
trap_check_parity( trap_load_state *state )
{
  /* If |DE| bytes have been read and there's more data, check parity. */
  if( DE == state->processed && state->read + 1 < state->length ) {
    state->parity ^= state->data[ state->read ];
    *state->bytes_consumed = state->read + 2;
    A = state->parity;
    CP( 1 ); /* Parity is successful if A == 0. */
    B = 0xB0;
    trap_load_finish( state );
    return;
  }

  /* Failure to read first bit of the next byte (ref. 48K ROM, 0x5EC) */
  B = 255;
  L = 1;
  INC( B );
  trap_load_fail( state );
}

int
tape_trap_load_block( libspectrum_tape_block *block, size_t *bytes_consumed )
{
  /* On exit:
   *  A = calculated parity byte if parity checked, else 0 (CHECKME)
   *  F : if parity checked, all flags are modified
   *      else carry only is modified (FIXME)
   *  B = 0xB0 (success) or 0x00 (failure)
   *  C = 0x01 (confirmed), 0x21, 0xFE or 0xDE (CHECKME)
   * DE : decremented by number of bytes loaded or verified
   *  H = calculated parity byte or undefined
   *  L = last byte read, or 1 if none
   * IX : incremented by number of bytes loaded or verified
   * A' = unchanged on error + no flag byte, else 0x01
   * F' = 0x01      on error + no flag byte, else 0x45
   *  R = no point in altering it :-)
   * Other registers unchanged.
   */
  trap_load_state state;
  int requested_flag;

  state.data = libspectrum_tape_block_data( block );
  state.length = libspectrum_tape_block_data_length( block );

  /* Number of bytes to load or verify. */
  state.read = state.length - 1;
  if( state.read > DE ) state.read = DE;
  state.processed = 0;
  state.bytes_consumed = bytes_consumed;
  *bytes_consumed = 0;

  /* If there's no data in the block, L and F' are the only registers set. */
  if( !state.length ) {
    L = F_ = 1;
    F &= ~FLAG_C;
    return 0;
  }

  /* Loading or verifying is determined by the carry flag of F'. */
  state.verify = !( F_ & FLAG_C );
  requested_flag = A_;
  A = 0;

  /* Initialise the parity check and L to the block ID byte. */
  L = state.parity = *state.data++;
  *bytes_consumed = 1;

  /* Emulate the zero-length block ROM bug. */
  if( !DE ) {
    /* One byte was read, but it is not treated as a data byte. */
    B = 0xB0; /* Value at the end of the LD-8-BITS/0x05CA loop. */
    A = state.parity; /* ROM address 0x05DF. */
    CP( 1 ); /* Parity is successful if A == 0. */
    trap_load_finish( &state );
    return 0;
  }

  AF_ = 0x0145;

  /* The ROM compares the block ID with the requested flag using XOR,
     leaving the difference in A on a mismatch. */
  A = requested_flag;
  XOR( state.parity );
  if( A ) {
    /* The flag byte is not included in the IX/DE data count. */
    trap_load_fail( &state );
    return 0;
  }

  /* Set L to the last data byte that will be processed. For a bare flag
     byte, there is no data and L already contains the flag. */
  if( state.read ) L = state.data[ state.read - 1 ];
  if( state.verify && trap_verify_bytes( &state ) ) {
    trap_load_fail( &state );
    return 0;
  }
  if( !state.verify ) trap_copy_bytes( &state );

  /* Both paths leave processed at the number of bytes handled. */
  *bytes_consumed = state.processed + 1;
  trap_check_parity( &state );
  return 0;
}

/* Append to the current tape file in memory; returns 0 if a block was
   saved or non-zero if there was an error at the emulator level, or tape
   traps are not active */
int
tape_save_trap( void )
{
  libspectrum_tape_block *block;
  libspectrum_byte parity, *data;
  libspectrum_error error;
  size_t length;

  int i;

  /* Do nothing if tape traps aren't active */
  if( !settings_current.tape_traps || tape_recording ||
      rzx_playback || rzx_recording )
    return 2;

  /* Check we're in the right ROM */
  if( !trap_check_rom( CHECK_TAPE_ROM ) ) return 3;

  block = libspectrum_tape_block_alloc( LIBSPECTRUM_TAPE_BLOCK_ROM );
  
  /* The +2 here is for the flag and parity bytes */
  length = DE + 2;
  libspectrum_tape_block_set_data_length( block, length );

  data = libspectrum_new( libspectrum_byte, length );
  libspectrum_tape_block_set_data( block, data );

  /* First, store the flag byte (and initialise the parity counter) */
  data[0] = parity = A;

  /* then the main body of the data, counting parity along the way */
  for( i=0; i<DE; i++) {
    libspectrum_byte b = readbyte_internal( IX+i );
    parity ^= b;
    data[i+1] = b;
  }

  /* And finally the parity byte */
  data[ DE+1 ] = parity;

  /* Give a 1 second pause after this block */
  libspectrum_tape_block_set_pause( block, TAPE_ROM_SAVE_PAUSE_MS );

  error = libspectrum_tape_append_block( tape, block );
  if( error ) {
    libspectrum_tape_block_free( block );
    return error;
  }

  tape_modified = 1;
  ui_tape_browser_update( UI_TAPE_BROWSER_NEW_BLOCK, block );

  /* And then return via the RET at #053E, except on Timex 2068 at #00E4 */
  if ( machine_current->machine == LIBSPECTRUM_MACHINE_TC2068 ||
       machine_current->machine == LIBSPECTRUM_MACHINE_TS2068 ) {
    PC = 0x00e4;
  } else {
    PC = 0x053e;
  }

  return 0;

}

