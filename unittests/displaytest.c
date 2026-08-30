/* displaytest.c: Test program for Fuse's display code
   Copyright (c) 2017 Philip Kendall

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

#include <config.h>

#include <libspectrum.h>

#include <string.h>

#include "compat.h"
#include "infrastructure/startup_manager.h"
#include "machine.h"
#include "display.h"
#include "memory_pages.h"
#include "peripherals/scld.h"
#include "rectangle.h"
#include "settings.h"

libspectrum_dword tstates;

scld scld_last_dec;

int memory_current_screen;

libspectrum_byte RAM[ SPECTRUM_RAM_PAGES ][0x4000] = { { 0 } };

fuse_machine_info *machine_current;

const int LINE_TIME = 224;
const int TOP_BORDER = 24;

void display_reset_frame_count( void );
void display_set_flash_reversed( int reversed );
void display_clear_maybe_dirty( void );
void display_clear_is_dirty( void );
void display_set_maybe_dirty( int y, libspectrum_qword dirty );
libspectrum_qword display_get_is_dirty( int y );
libspectrum_dword display_get_maybe_dirty( int y );

/* Various "mocks" for the UI code */

typedef void (*plot8_fn_t)( int x, int y, libspectrum_byte data,
                            libspectrum_byte ink, libspectrum_byte paper );

static plot8_fn_t plot8_fn;

static void
plot8_null( int x, int y, libspectrum_byte data, libspectrum_byte ink,
            libspectrum_byte paper )
{
  /* Do nothing */
}

static int plot8_count;

struct plot8_record_t {
  int x;
  int y;
  libspectrum_byte data;
  libspectrum_byte ink;
  libspectrum_byte paper;
};

static struct plot8_record_t plot8_last_write;

static void
plot8_count_fn( int x, int y, libspectrum_byte data, libspectrum_byte ink,
                libspectrum_byte paper )
{
  plot8_count++;
  plot8_last_write.x = x;
  plot8_last_write.y = y;
  plot8_last_write.data = data;
  plot8_last_write.ink = ink;
  plot8_last_write.paper = paper;
}

static int
plot8_assert( int count, int x, int y, libspectrum_byte data,
              libspectrum_byte ink, libspectrum_byte paper )
{
  if( plot8_count != count ) {
    fprintf( stderr, "plot8_count: expected %d, got %d\n",
             count, plot8_count );
    return 1;
  }
  if( plot8_last_write.x != x ) {
    fprintf( stderr, "plot8 x: expected %d, got %d\n", x,
             plot8_last_write.x );
    return 1;
  }
  if( plot8_last_write.y != y ) {
    fprintf( stderr, "plot8 y: expected %d, got %d\n", y,
             plot8_last_write.y );
    return 1;
  }
  if( plot8_last_write.data != data ) {
    fprintf( stderr, "plot8 data: expected 0x%02x, got 0x%02x\n",
             data, plot8_last_write.data );
    return 1;
  }
  if( plot8_last_write.ink != ink ) {
    fprintf( stderr, "plot8 ink: expected 0x%02x, got 0x%02x\n",
             ink, plot8_last_write.ink );
    return 1;
  }
  if( plot8_last_write.paper != paper ) {
    fprintf( stderr, "plot8 paper: expected 0x%02x, got 0x%02x\n",
             paper, plot8_last_write.paper );
    return 1;
  }

  return 0;
}

/* Vector off to the "plot8" implementation for the current test */
void
uidisplay_plot8( int x, int y, libspectrum_byte data, libspectrum_byte ink,
                 libspectrum_byte paper )
{
  plot8_fn( x, y, data, ink, paper );
}

/* Tracking infrastructure for uidisplay_plot16 */

typedef void (*plot16_fn_t)( int x, int y, libspectrum_word data,
                             libspectrum_byte ink, libspectrum_byte paper );

static plot16_fn_t plot16_fn;

static void
plot16_null( int x, int y, libspectrum_word data, libspectrum_byte ink,
             libspectrum_byte paper )
{
  /* Do nothing */
}

/* putpixel tracking (used by the Pentagon 16-colour display path) */

typedef void (*putpixel_fn_t)( int x, int y, int colour );

static putpixel_fn_t putpixel_fn;

static void
putpixel_null( int x, int y, int colour )
{
  /* Do nothing */
}

static int plot16_count;

struct plot16_record_t {
  int x;
  int y;
  libspectrum_word data;
  libspectrum_byte ink;
  libspectrum_byte paper;
};

static struct plot16_record_t plot16_last_write;

static void
plot16_count_fn( int x, int y, libspectrum_word data, libspectrum_byte ink,
                 libspectrum_byte paper )
{
  plot16_count++;
  plot16_last_write.x = x;
  plot16_last_write.y = y;
  plot16_last_write.data = data;
  plot16_last_write.ink = ink;
  plot16_last_write.paper = paper;
}

static int
plot16_assert( int count, int x, int y, libspectrum_word data,
               libspectrum_byte ink, libspectrum_byte paper )
{
  if( plot16_count != count ) {
    fprintf( stderr, "plot16_count: expected %d, got %d\n",
             count, plot16_count );
    return 1;
  }
  if( plot16_last_write.x != x ) {
    fprintf( stderr, "plot16 x: expected %d, got %d\n", x,
             plot16_last_write.x );
    return 1;
  }
  if( plot16_last_write.y != y ) {
    fprintf( stderr, "plot16 y: expected %d, got %d\n", y,
             plot16_last_write.y );
    return 1;
  }
  if( plot16_last_write.data != data ) {
    fprintf( stderr, "plot16 data: expected 0x%04x, got 0x%04x\n",
             data, plot16_last_write.data );
    return 1;
  }
  if( plot16_last_write.ink != ink ) {
    fprintf( stderr, "plot16 ink: expected 0x%02x, got 0x%02x\n",
             ink, plot16_last_write.ink );
    return 1;
  }
  if( plot16_last_write.paper != paper ) {
    fprintf( stderr, "plot16 paper: expected 0x%02x, got 0x%02x\n",
             paper, plot16_last_write.paper );
    return 1;
  }
  return 0;
}

static int putpixel_count;

static void
putpixel_count_fn( int x, int y, int colour )
{
  putpixel_count++;
}

static int write_if_dirty_count;
static int write_if_dirty_last_x;
static int write_if_dirty_last_y;

static void
write_if_dirty_count_fn( int x, int y )
{
  write_if_dirty_count++;
  write_if_dirty_last_x = x;
  write_if_dirty_last_y = y;
}

/* Main program code */

static void
create_fake_machine( void )
{
  size_t y;

  machine_current = libspectrum_malloc( sizeof( *machine_current ) );

  machine_current->timex = 0;
  machine_current->timings.tstates_per_line = LINE_TIME;

  for( y = 0; y < ARRAY_SIZE( machine_current->line_times ); y++ )
    machine_current->line_times[y] = y * LINE_TIME;

  display_dirty_flashing = display_dirty_flashing_sinclair;
}

static void
test_before( void )
{
  memset( RAM[0], 0, ARRAY_SIZE( RAM[0] ) );
  memset( display_last_screen, 0, sizeof( display_last_screen ) );
  display_clear_maybe_dirty();

  putpixel_fn = putpixel_null;
  putpixel_count = 0;

  plot8_fn = plot8_null;
  display_reset_frame_count();
  display_write_if_dirty = display_write_if_dirty_sinclair;

  display_frame();
  display_clear_is_dirty();

  plot8_fn = plot8_count_fn;
  plot16_fn = plot16_null;
  plot8_count = 0;

  write_if_dirty_count = 0;
  write_if_dirty_last_x = -1;
  write_if_dirty_last_y = -1;
}

static void
timex_test_before( libspectrum_byte scld_byte )
{
  memset( RAM[0], 0, ARRAY_SIZE( RAM[0] ) );
  memset( display_last_screen, 0, sizeof( display_last_screen ) );
  display_clear_maybe_dirty();

  scld_last_dec.byte = scld_byte;
  plot8_fn = plot8_null;
  plot16_fn = plot16_null;
  display_reset_frame_count();
  display_write_if_dirty = display_write_if_dirty_timex;

  display_frame();
  display_clear_is_dirty();

  plot8_fn = plot8_count_fn;
  plot16_fn = plot16_count_fn;
  plot8_count = 0;
  plot16_count = 0;

  write_if_dirty_count = 0;
  write_if_dirty_last_x = -1;
  write_if_dirty_last_y = -1;
}

static int
no_write_if_data_unchanged( void )
{
  /* Arrange */
  RAM[0][0] = 0;
  RAM[0][DISPLAY_PIXEL_BYTES] = 0;

  /* Act */
  display_write_if_dirty_sinclair( 0, 0 );

  /* Assert */
  if( plot8_count ) return 1;

  return 0;
}

static int
write_called_for_new_data( void )
{
  /* Arrange */
  RAM[0][0] = 0x01;
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x02;

  /* Act */
  display_write_if_dirty_sinclair( 0, 0 );

  /* Assert */
  if( plot8_assert( 1, 4, 24, 0x01, 2, 0 ) ) return 1;
  if( display_last_screen[ 964 ] != 0x201 ) {
    fprintf( stderr,
             "display_last_screen[964]: expected 0x201, got 0x%x (attr=0x%02x, scld=0x%02x)\n",
             display_last_screen[ 964 ], RAM[0][DISPLAY_PIXEL_BYTES], scld_last_dec.byte );
    return 1;
  }
  if( display_get_is_dirty( 24 ) != ( (libspectrum_qword)1 << 4 ) ) {
    fprintf( stderr, "display_get_is_dirty(24): expected 0x%lx, got 0x%llx\n",
             (unsigned long)( (libspectrum_qword)1 << 4 ),
             (unsigned long long)display_get_is_dirty( 24 ) );
    return 1;
  }

  return 0;
}

static int
write_reads_from_appropriate_x( void )
{
  /* Arrange */
  RAM[0][31] = 0x12;
  RAM[0][DISPLAY_PIXEL_BYTES + 31] = 0x34;

  /* Act */
  display_write_if_dirty_sinclair( 31, 0 );

  /* Assert */
  if( plot8_assert( 1, 35, 24, 0x12, 4, 6 ) ) return 1;
  if( display_last_screen[ 995 ] != 0x3412 ) return 1;
  if( display_get_is_dirty( 24 ) != ( (libspectrum_qword)1 << 35 ) ) return 1;

  return 0;
}

static int
write_reads_from_appropriate_y( void )
{
  /* Arrange */
  RAM[0][32] = 0x56;
  RAM[0][DISPLAY_PIXEL_BYTES + 32] = 0x78;

  /* Act */
  display_write_if_dirty_sinclair( 0, 8 );

  /* Assert */
  if( plot8_assert( 1, 4, 32, 0x56, 8, 15 ) ) return 1;
  if( display_last_screen[ 1284 ] != 0x7856 ) return 1;
  if( display_get_is_dirty( 32 ) != ( (libspectrum_qword)1 << 4 ) ) return 1;

  return 0;
}

static int
write_reads_from_middle_third( void )
{
  /* y=64 is the first pixel row of the middle screen third.
     display_line_start[64] = 32*64 = 2048; attr at display_attr_start[64] = 0x1900.
     beam_x = 0 + 4 = 4, beam_y = 64 + 24 = 88, last_screen index = 4 + 88*40 = 3524 */
  RAM[0][2048] = 0xa5;
  RAM[0][0x1900] = 0x38;    /* ink=0, paper=7 */

  display_write_if_dirty_sinclair( 0, 64 );

  if( plot8_assert( 1, 4, 88, 0xa5, 0, 7 ) ) return 1;
  if( display_last_screen[ 3524 ] != 0x38a5 ) {
    fprintf( stderr, "display_last_screen[3524]: expected 0x38a5, got 0x%x\n",
             display_last_screen[ 3524 ] );
    return 1;
  }
  if( display_get_is_dirty( 88 ) != ( (libspectrum_qword)1 << 4 ) ) return 1;

  return 0;
}

static int
write_reads_from_bottom_third( void )
{
  /* y=128 is the first pixel row of the bottom screen third.
     display_line_start[128] = 32*128 = 4096; attr at display_attr_start[128] = 0x1a00.
     beam_x = 0 + 4 = 4, beam_y = 128 + 24 = 152, last_screen index = 4 + 152*40 = 6084 */
  RAM[0][4096] = 0x99;
  RAM[0][0x1a00] = 0x09;    /* ink=1, paper=1 */

  display_write_if_dirty_sinclair( 0, 128 );

  if( plot8_assert( 1, 4, 152, 0x99, 1, 1 ) ) return 1;
  if( display_last_screen[ 6084 ] != 0x0999 ) {
    fprintf( stderr, "display_last_screen[6084]: expected 0x0999, got 0x%x\n",
             display_last_screen[ 6084 ] );
    return 1;
  }
  if( display_get_is_dirty( 152 ) != ( (libspectrum_qword)1 << 4 ) ) return 1;

  return 0;
}

static int
no_redraw_if_cached_after_write( void )
{
  /* First call writes new data and caches it in display_last_screen */
  RAM[0][0] = 0x01;
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x02;

  display_write_if_dirty_sinclair( 0, 0 );
  if( !plot8_count ) {
    fprintf( stderr, "expected plot8 on first call, got none\n" );
    return 1;
  }

  /* Second call with identical RAM data: cache hit, no redraw expected */
  plot8_count = 0;
  display_write_if_dirty_sinclair( 0, 0 );
  if( plot8_count ) {
    fprintf( stderr,
             "expected no plot8 on second call with same data, got %d\n",
             plot8_count );
    return 1;
  }

  return 0;
}

static int
flash_inverts_colours( void )
{
  /* Arrange */
  RAM[0][0] = 0x01;
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x82;

  display_set_flash_reversed( 1 );

  /* Act */
  display_write_if_dirty_sinclair( 0, 0 );

  /* Assert */
  if( plot8_assert( 1, 4, 24, 0x01, 0, 2 ) ) return 1;
  if( display_last_screen[ 964 ] != 0x01008201 ) return 1;
  if( display_get_is_dirty( 24 ) != ( (libspectrum_qword)1 << 4 ) ) return 1;

  return 0;
}

static int
no_write_if_nothing_dirty( void )
{
  /* Arrange */
  tstates = (TOP_BORDER + 1) * LINE_TIME;
  display_write_if_dirty = write_if_dirty_count_fn;

  /* Act */
  display_dirty_sinclair( 0x0000 );

  /* Assert */
  if( write_if_dirty_count ) return 1;

  return 0;
}

static int
write_if_dirty( void )
{
  /* Arrange */
  tstates = (TOP_BORDER + 1) * LINE_TIME;
  display_set_maybe_dirty( 0, 0x01 );
  display_write_if_dirty = write_if_dirty_count_fn;

  /* Act */
  display_dirty_sinclair( 0x0000 );

  /* Assert */
  if( write_if_dirty_count != 1 ) return 1;
  if( write_if_dirty_last_y != 0 ) return 1;
  if( write_if_dirty_last_x != 0 ) return 1;

  return 0;
}

static int
no_write_if_dirty_area_ahead_of_beam( void )
{
  /* Arrange */
  tstates = (TOP_BORDER + 1) * LINE_TIME;
  display_set_maybe_dirty( 2, 0x01 );
  display_write_if_dirty = write_if_dirty_count_fn;

  /* Act */
  display_dirty_sinclair( 0x0000 );

  /* Assert */
  if( write_if_dirty_count ) return 1;

  return 0;
}

static int
no_write_if_modified_area_ahead_of_critical_region( void )
{
  /* Arrange */
  tstates = TOP_BORDER * LINE_TIME;
  display_set_maybe_dirty( 0, 0x01 );
  display_write_if_dirty = write_if_dirty_count_fn;

  /* Act */
  display_dirty_sinclair( 0x0020 );

  /* Assert */
  if( write_if_dirty_count ) return 1;

  return 0;
}

static int
attribute_write_marks_correct_cell( void )
{
  int y;

  /* Arrange: the final attribute byte is column 31, character row 23. */

  /* Act */
  display_dirty_sinclair( DISPLAY_PIXEL_BYTES + DISPLAY_ATTR_BYTES - 1 );

  /* Assert: mark column 31 in each of the cell's eight pixel rows only. */
  for( y = 0; y < DISPLAY_HEIGHT; y++ ) {
    libspectrum_dword expected =
      y >= 184 ? ( (libspectrum_dword)1 << 31 ) : 0;

    if( display_get_maybe_dirty( y ) != expected ) return 1;
  }

  return 0;
}

static int
attribute_write_marks_correct_cells( void )
{
  /* Verify the display_dirty64() attribute address decoder against
     boundary and representative offsets.

     The attribute area begins at DISPLAY_PIXEL_BYTES and is
     laid out linearly: idx = offset - DISPLAY_PIXEL_BYTES, x = idx % 32,
     char_row = idx / 32.  Each attribute covers eight pixel rows
     (char_row*8 .. char_row*8+7), so eight consecutive dirty bits
     must be set for column x. */

  static const struct {
    libspectrum_word offset;
    int expected_x;
    int expected_char_row;   /* y = expected_char_row * 8 */
  } cases[] = {
    { DISPLAY_PIXEL_BYTES,                               0,  0 },  /* first attr byte: col 0, char row 0 */
    { DISPLAY_PIXEL_BYTES + DISPLAY_WIDTH_COLS - 1,     31,  0 },  /* last col of first char row */
    { DISPLAY_PIXEL_BYTES + DISPLAY_WIDTH_COLS,          0,  1 },  /* first col of second char row */
    { DISPLAY_PIXEL_BYTES + 2 * DISPLAY_WIDTH_COLS,      0,  2 },  /* first col of third char row */
    { DISPLAY_PIXEL_BYTES + 23 * DISPLAY_WIDTH_COLS,     0, 23 },  /* first col of last char row */
    { DISPLAY_PIXEL_BYTES + DISPLAY_ATTR_BYTES - 1,     31, 23 },  /* last attr byte */
  };
  int c;

  for( c = 0; c < (int)( sizeof cases / sizeof cases[0] ); c++ ) {
    int y;
    libspectrum_word offset = cases[c].offset;
    int expected_x = cases[c].expected_x;
    int first_y = cases[c].expected_char_row * 8;
    libspectrum_dword expected_bit = (libspectrum_dword)1 << expected_x;

    test_before();

    display_dirty_sinclair( offset );

    for( y = 0; y < DISPLAY_HEIGHT; y++ ) {
      libspectrum_dword expected =
        ( y >= first_y && y < first_y + 8 ) ? expected_bit : 0;
      if( display_get_maybe_dirty( y ) != expected ) return 1;
    }
  }

  return 0;
}

static int
pixel_write_marks_correct_cells( void )
{
  /* Verify the display_dirty8() address decoder against a representative
     sample of the ZX Spectrum pixel-area layout.

     The screen is encoded as:
       bits  4-0:  column x (0-31)
       bits  7-5:  character row within third j (0-7)
       bits 10-8:  pixel row within character k (0-7)
       bits 12-11: third of screen i (0-2)
     giving screen line y = 64*i + 8*j + k.

     We write to a specific offset, then confirm that exactly bit x of
     display_maybe_dirty[y] is set. */

  static const struct {
    libspectrum_word offset;
    int expected_x, expected_y;
  } cases[] = {
    { 0x0000,  0,   0 },  /* col 0, first scan line of top third */
    { 0x001f, 31,   0 },  /* col 31, first scan line of top third */
    { 0x0020,  0,   8 },  /* col 0, first scan line of second char row */
    { 0x0100,  0,   1 },  /* col 0, second scan line of top char row */
    { 0x0800,  0,  64 },  /* col 0, first scan line of middle third */
    { DISPLAY_PIXEL_BYTES - 1, 31, 191 },  /* col 31, last scan line of bottom third */
  };
  int c;

  for( c = 0; c < (int)( sizeof cases / sizeof cases[0] ); c++ ) {
    int y;
    libspectrum_word offset = cases[c].offset;
    int expected_x = cases[c].expected_x;
    int expected_y = cases[c].expected_y;
    libspectrum_dword expected_bit = (libspectrum_dword)1 << expected_x;

    test_before();

    display_dirty_sinclair( offset );

    for( y = 0; y < DISPLAY_HEIGHT; y++ ) {
      libspectrum_dword expected = ( y == expected_y ) ? expected_bit : 0;
      if( display_get_maybe_dirty( y ) != expected ) return 1;
    }
  }

  return 0;
}

/* display_dirty_flashing_sinclair() tests */

static int
flash_dirty_no_flash_attrs( void )
{
  int y;

  /* Arrange: all attribute bytes have flash bit clear (RAM zeroed by test_before) */

  /* Act */
  display_dirty_flashing_sinclair();

  /* Assert: no maybe_dirty bits should be set */
  for( y = 0; y < DISPLAY_HEIGHT; y++ )
    if( display_get_maybe_dirty( y ) ) return 1;

  return 0;
}

static int
flash_dirty_with_flash_attr_row0_col0( void )
{
  int i;

  /* Arrange: set flash bit in attribute for row 0, col 0 */
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x80;

  /* Act */
  display_dirty_flashing_sinclair();

  /* Assert: all 8 pixel rows for that attribute cell must be dirty at col 0 */
  for( i = 0; i < 8; i++ )
    if( !( display_get_maybe_dirty( i ) & 0x01 ) ) return 1;

  /* Row 8 onwards must be clean */
  for( i = 8; i < DISPLAY_HEIGHT; i++ )
    if( display_get_maybe_dirty( i ) ) return 1;

  return 0;
}

static int
flash_dirty_non_flash_attr( void )
{
  int y;

  /* Arrange: non-flash attr at row 0, col 0 (bit 7 clear) */
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x47;

  /* Act */
  display_dirty_flashing_sinclair();

  /* Assert: no maybe_dirty bits should be set */
  for( y = 0; y < DISPLAY_HEIGHT; y++ )
    if( display_get_maybe_dirty( y ) ) return 1;

  return 0;
}

/* display_dirty_flashing_timex() tests */

static void
timex_flash_test_before( libspectrum_byte scld_byte )
{
  test_before();
  scld_last_dec.byte = scld_byte;
}

static int
timex_flash_hires_skips_flashing( void )
{
  int y;

  /* Arrange: hires mode; set flash bits in both screen areas */
  timex_flash_test_before( HIRES );
  RAM[0][ALTDFILE_OFFSET] = 0x80;
  RAM[0][ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES] = 0x80;

  /* Act */
  display_dirty_flashing_timex();

  /* Assert: hires path is a no-op — no maybe_dirty bits should be set */
  for( y = 0; y < DISPLAY_HEIGHT; y++ )
    if( display_get_maybe_dirty( y ) ) return 1;

  return 0;
}

static int
timex_flash_b1_no_attrs_clean( void )
{
  int y;

  /* Arrange: b1 mode, no flash bytes in alternate pixel area (RAM zeroed) */
  timex_flash_test_before( 0x02 ); /* b1=1, hires=0, altdfile=0 */

  /* Act */
  display_dirty_flashing_timex();

  /* Assert: no maybe_dirty bits set */
  for( y = 0; y < DISPLAY_HEIGHT; y++ )
    if( display_get_maybe_dirty( y ) ) return 1;

  return 0;
}

static int
timex_flash_b1_marks_dirty( void )
{
  /* Arrange: b1 mode; set flash bit at first byte of second screen pixel area */
  timex_flash_test_before( 0x02 ); /* b1=1, hires=0, altdfile=0 */
  RAM[0][ALTDFILE_OFFSET] = 0x80;  /* offset 0x2000, maps to pixel row 0, col 0 */

  /* Act */
  display_dirty_flashing_timex();

  /* Assert: pixel row 0, col 0 must be marked dirty */
  if( !( display_get_maybe_dirty( 0 ) & 0x01 ) ) {
    fprintf( stderr,
             "timex_flash_b1_marks_dirty: expected maybe_dirty[0] bit 0 set\n" );
    return 1;
  }

  return 0;
}

static int
timex_flash_altdfile_marks_dirty( void )
{
  int i;

  /* Arrange: altdfile mode; flash attr at row 0, col 0 of second screen */
  timex_flash_test_before( ALTDFILE ); /* altdfile=1, b1=0, hires=0 */
  RAM[0][ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES] = 0x80;  /* second screen attr area, row 0 col 0 */

  /* Act */
  display_dirty_flashing_timex();

  /* Assert: all 8 pixel rows for that attribute cell dirty at col 0 */
  for( i = 0; i < 8; i++ ) {
    if( !( display_get_maybe_dirty( i ) & 0x01 ) ) {
      fprintf( stderr,
               "timex_flash_altdfile_marks_dirty: expected maybe_dirty[%d] bit 0 set\n",
               i );
      return 1;
    }
  }

  /* Rows 8 onwards must be clean */
  for( i = 8; i < DISPLAY_HEIGHT; i++ )
    if( display_get_maybe_dirty( i ) ) return 1;

  return 0;
}

static int
timex_flash_standard_delegates_to_sinclair( void )
{
  int i;

  /* Arrange: standard Timex mode (scld=0); flash attr at row 0, col 0 */
  timex_flash_test_before( STANDARD );
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x80;

  /* Act: standard path delegates to display_dirty_flashing_sinclair() */
  display_dirty_flashing_timex();

  /* Assert: same result as sinclair flash test — 8 rows dirty at col 0 */
  for( i = 0; i < 8; i++ ) {
    if( !( display_get_maybe_dirty( i ) & 0x01 ) ) {
      fprintf( stderr,
               "timex_flash_standard_delegates: expected maybe_dirty[%d] bit 0 set\n",
               i );
      return 1;
    }
  }

  for( i = 8; i < DISPLAY_HEIGHT; i++ )
    if( display_get_maybe_dirty( i ) ) return 1;

  return 0;
}

/* display_write_if_dirty_timex() tests */

static int
timex_lores_no_redraw_if_unchanged( void )
{
  /* Arrange: STANDARD mode, all-zero RAM (matches zeroed display_last_screen) */
  timex_test_before( STANDARD );

  /* Act */
  display_write_if_dirty_timex( 0, 0 );

  /* Assert: cache hit — no redraw */
  if( plot8_count ) return 1;

  return 0;
}

static int
timex_lores_write_called_for_new_data( void )
{
  /* Arrange: STANDARD mode, non-zero pixel and attribute data */
  timex_test_before( STANDARD );
  RAM[0][0] = 0x01;
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x02; /* ink=2, paper=0 */

  /* Act */
  display_write_if_dirty_timex( 0, 0 );

  /* Assert: plot8 called; cache updated with (mode_data=0x00, attr=0x02, data=0x01) */
  if( plot8_assert( 1, 4, 24, 0x01, 2, 0 ) ) return 1;
  if( display_last_screen[ 964 ] != 0x00000201 ) {
    fprintf( stderr,
             "display_last_screen[964]: expected 0x201, got 0x%x\n",
             display_last_screen[ 964 ] );
    return 1;
  }

  return 0;
}

static int
timex_mode_change_causes_redraw( void )
{
  /* Arrange: draw once in STANDARD mode to prime the cache */
  timex_test_before( STANDARD );
  RAM[0][0] = 0x01;
  RAM[0][DISPLAY_PIXEL_BYTES] = 0x02;
  display_write_if_dirty_timex( 0, 0 );
  plot8_count = 0;

  /* Act: change mode_data via SCLD byte (pixel/attr bytes unchanged) */
  scld_last_dec.byte = 0x40; /* intdisable set; scrnmode still STANDARD */
  display_write_if_dirty_timex( 0, 0 );

  /* Assert: mode_data differs → cache miss → redraw required */
  if( plot8_count != 1 ) {
    fprintf( stderr, "timex_mode_change: expected plot8_count=1, got %d\n",
             plot8_count );
    return 1;
  }

  return 0;
}

static int
timex_hires_plot16_called_with_correct_data( void )
{
  /* Arrange: HIRES mode; pixel from first screen, pixel from second screen */
  timex_test_before( HIRES );
  RAM[0][0] = 0xAA;
  RAM[0][ALTDFILE_OFFSET] = 0x55;

  /* Act */
  display_write_if_dirty_timex( 0, 0 );

  /* Assert: uidisplay_plot16 called with hires_data = (0xAA<<8)|0x55 */
  if( plot16_assert( 1, 4, 24, 0xAA55, 0, 0 ) ) return 1;

  return 0;
}

typedef int (*test_fn_t)( void );

struct test_t {
  const char *name;
  test_fn_t fn;
};

/* Setup for Pentagon 16-colour display tests */

static void
pentagon_test_before( void )
{
  memset( RAM[4], 0, sizeof( RAM[4] ) );
  memset( RAM[5], 0, sizeof( RAM[5] ) );
  memset( RAM[6], 0, sizeof( RAM[6] ) );
  memset( RAM[7], 0, sizeof( RAM[7] ) );
  memset( display_last_screen, 0, sizeof( display_last_screen ) );
  display_clear_is_dirty();

  putpixel_fn = putpixel_count_fn;
  putpixel_count = 0;

  memory_current_screen = 5;
}

/* display_write_if_dirty_pentagon_16_col() tests */

static int
pentagon_no_write_if_data_unchanged( void )
{
  pentagon_test_before();

  /* All RAM pages and display_last_screen are zero — cache is current,
     so no pixels should be emitted. */
  display_write_if_dirty_pentagon_16_col( 0, 0 );

  if( putpixel_count ) {
    fprintf( stderr, "putpixel_count: expected 0, got %d\n", putpixel_count );
    return 1;
  }

  return 0;
}

static int
pentagon_write_called_for_new_data( void )
{
  pentagon_test_before();

  /* Set data2 (page 5 base offset) to a nonzero value. */
  RAM[5][0] = 0x01;

  display_write_if_dirty_pentagon_16_col( 0, 0 );

  /* 8 pixels must be emitted. */
  if( putpixel_count != 8 ) {
    fprintf( stderr, "putpixel_count: expected 8, got %d\n", putpixel_count );
    return 1;
  }

  /* last_chunk_detail = (data4<<24)|(data3<<16)|(data2<<8)|data1 = 0x100 */
  if( display_last_screen[ 964 ] != 0x100 ) {
    fprintf( stderr,
             "display_last_screen[964]: expected 0x100, got 0x%x\n",
             display_last_screen[ 964 ] );
    return 1;
  }

  if( display_get_is_dirty( 24 ) != ( (libspectrum_qword)1 << 4 ) ) {
    fprintf( stderr,
             "display_is_dirty[24]: expected bit 4 set\n" );
    return 1;
  }

  return 0;
}

static int
pentagon_page7_reads_correct_pages( void )
{
  pentagon_test_before();
  memory_current_screen = 7;

  /* Set data2 from page 7 base offset — verifies page 7/6 selection. */
  RAM[7][0] = 0x11;

  display_write_if_dirty_pentagon_16_col( 0, 0 );

  if( putpixel_count != 8 ) {
    fprintf( stderr, "putpixel_count: expected 8, got %d\n", putpixel_count );
    return 1;
  }

  /* last_chunk_detail = 0|(0<<16)|(0x11<<8)|0 = 0x1100 */
  if( display_last_screen[ 964 ] != 0x1100 ) {
    fprintf( stderr,
             "display_last_screen[964]: expected 0x1100, got 0x%x\n",
             display_last_screen[ 964 ] );
    return 1;
  }

  return 0;
}

/* display_parse_attr() tests */

/* Helper: parse attr and return non-zero if ink or paper don't match. */
static int
check_parse_attr( libspectrum_byte attr, libspectrum_byte expected_ink,
                  libspectrum_byte expected_paper, const char *label )
{
  libspectrum_byte ink, paper;

  display_parse_attr( attr, &ink, &paper );

  if( ink != expected_ink ) {
    fprintf( stderr, "%s: ink: expected %d, got %d\n",
             label, (int)expected_ink, (int)ink );
    return 1;
  }
  if( paper != expected_paper ) {
    fprintf( stderr, "%s: paper: expected %d, got %d\n",
             label, (int)expected_paper, (int)paper );
    return 1;
  }
  return 0;
}

/* attr = 0x05: ink=5 (0b101), paper=0, bright=0, flash=0 */
static int
parse_attr_ink_only( void )
{
  display_set_flash_reversed( 0 );
  return check_parse_attr( 0x05, 5, 0, "parse_attr_ink_only" );
}

/* attr = 0x28: ink=0, paper=5 (bits 3-5 = 0b101), bright=0, flash=0 */
static int
parse_attr_paper_only( void )
{
  display_set_flash_reversed( 0 );
  return check_parse_attr( 0x28, 0, 5, "parse_attr_paper_only" );
}

/* attr = 0x45: ink=5, paper=0, bright=1 → both get +8 */
static int
parse_attr_bright( void )
{
  display_set_flash_reversed( 0 );
  /* 0x45 = 0100 0101: ink=5, paper=0, bright=1, flash=0 */
  return check_parse_attr( 0x45, 13, 8, "parse_attr_bright" );
}

/* attr = 0xa8: ink=0, paper=5, flash=1; flash_reversed=0 → no swap */
static int
parse_attr_flash_not_reversed( void )
{
  display_set_flash_reversed( 0 );
  /* 0xa8 = 1010 1000: ink=0, paper=5 (0b101 in bits 3-5), flash=1 */
  return check_parse_attr( 0xa8, 0, 5, "parse_attr_flash_not_reversed" );
}

/* Same attr but flash_reversed=1 → ink and paper are swapped */
static int
parse_attr_flash_reversed( void )
{
  int r;

  display_set_flash_reversed( 1 );
  /* ink and paper swap: expected ink=5, paper=0 */
  r = check_parse_attr( 0xa8, 5, 0, "parse_attr_flash_reversed" );
  display_set_flash_reversed( 0 );
  return r;
}

/* attr = 0xed: ink=5, paper=5, bright=1, flash=1; flash_reversed=1 → swap
   0xed = 1110 1101: bits 0-2=5 (ink), bits 3-5=5 (paper), bit6=1, bit7=1
   normal ink = 5+8=13, paper = 5+8=13; swapped: still 13, 13 */
static int
parse_attr_flash_reversed_symmetric( void )
{
  int r;

  display_set_flash_reversed( 1 );
  r = check_parse_attr( 0xed, 13, 13, "parse_attr_flash_reversed_symmetric" );
  display_set_flash_reversed( 0 );
  return r;
}

static const struct test_t tests[] = {
  /* display_write_if_dirty_sinclair() tests */
  { "no_write_if_data_unchanged", no_write_if_data_unchanged },
  { "write_called_for_new_data", write_called_for_new_data },
  { "write_reads_from_appropriate_x", write_reads_from_appropriate_x },
  { "write_reads_from_appropriate_y", write_reads_from_appropriate_y },
  { "write_reads_from_middle_third", write_reads_from_middle_third },
  { "write_reads_from_bottom_third", write_reads_from_bottom_third },
  { "no_redraw_if_cached_after_write", no_redraw_if_cached_after_write },
  { "flash_inverts_colours", flash_inverts_colours },

  /* display_dirty_sinclair() tests */
  { "no_write_if_nothing_dirty", no_write_if_nothing_dirty },
  { "write_if_dirty", write_if_dirty },
  { "no_write_if_dirty_area_ahead_of_beam",
    no_write_if_dirty_area_ahead_of_beam },
  { "no_write_if_modified_area_ahead_of_critical_region",
    no_write_if_modified_area_ahead_of_critical_region },
  { "attribute_write_marks_correct_cell", attribute_write_marks_correct_cell },
  { "attribute_write_marks_correct_cells",
    attribute_write_marks_correct_cells },
  { "pixel_write_marks_correct_cells", pixel_write_marks_correct_cells },

  /* display_dirty_flashing_sinclair() tests */
  { "flash_dirty_no_flash_attrs", flash_dirty_no_flash_attrs },
  { "flash_dirty_with_flash_attr_row0_col0",
    flash_dirty_with_flash_attr_row0_col0 },
  { "flash_dirty_non_flash_attr", flash_dirty_non_flash_attr },

  /* display_dirty_flashing_timex() tests */
  { "timex_flash_hires_skips_flashing",
    timex_flash_hires_skips_flashing },
  { "timex_flash_b1_no_attrs_clean",
    timex_flash_b1_no_attrs_clean },
  { "timex_flash_b1_marks_dirty",
    timex_flash_b1_marks_dirty },
  { "timex_flash_altdfile_marks_dirty",
    timex_flash_altdfile_marks_dirty },
  { "timex_flash_standard_delegates_to_sinclair",
    timex_flash_standard_delegates_to_sinclair },

  /* display_write_if_dirty_timex() tests */
  { "timex_lores_no_redraw_if_unchanged",
    timex_lores_no_redraw_if_unchanged },
  { "timex_lores_write_called_for_new_data",
    timex_lores_write_called_for_new_data },
  { "timex_mode_change_causes_redraw",
    timex_mode_change_causes_redraw },
  { "timex_hires_plot16_called_with_correct_data",
    timex_hires_plot16_called_with_correct_data },

  /* display_write_if_dirty_pentagon_16_col() tests */
  { "pentagon_no_write_if_data_unchanged",
    pentagon_no_write_if_data_unchanged },
  { "pentagon_write_called_for_new_data",
    pentagon_write_called_for_new_data },
  { "pentagon_page7_reads_correct_pages",
    pentagon_page7_reads_correct_pages },

  /* display_parse_attr() tests */
  { "parse_attr_ink_only", parse_attr_ink_only },
  { "parse_attr_paper_only", parse_attr_paper_only },
  { "parse_attr_bright", parse_attr_bright },
  { "parse_attr_flash_not_reversed", parse_attr_flash_not_reversed },
  { "parse_attr_flash_reversed", parse_attr_flash_reversed },
  { "parse_attr_flash_reversed_symmetric", parse_attr_flash_reversed_symmetric },

  /* End marker */
  { NULL, NULL }
};

#ifdef main
/* SDL headers redefine main on Windows, but this test needs a normal entry point. */
#undef main
#endif

int
main( int argc, char *argv[] )
{
  const struct test_t *test;

  if( display_init( &argc, &argv ) ) {
    fprintf( stderr, "Error from display_init()\n");
    return 1;
  }

  create_fake_machine();

  for( test = tests; test->fn; test++ ) {
    test_before();
    int result = test->fn();
    if( result ) {
      fprintf( stderr, "Test failed: %s\n", test->name );
      return 1;
    }
  }

  return 0;
}

/* Dummy code for the rest of the UI */

int
ui_init( int *argc, char ***argv )
{
  return 0;
}

void uidisplay_area( int x, int y, int w, int h ) {}
void uidisplay_frame_end( void ) {}
void uidisplay_putpixel( int x, int y, int colour ) { putpixel_fn( x, y, colour ); }

void
uidisplay_plot16( int x, int y, libspectrum_word data, libspectrum_byte ink,
                  libspectrum_byte paper )
{
  plot16_fn( x, y, data, ink, paper );
}

/* Dummy movie code */

int movie_recording;

void movie_start_frame( void ) {}
void movie_add_area( int x, int y, int w, int h ) {}

/* Dummy rectangle code */

struct rectangle *rectangle_inactive;
size_t rectangle_inactive_count, rectangle_inactive_allocated;

void rectangle_add( int y, int x, int w ) {}
void rectangle_end_line( int y ) {}

/* Dummy SCLD code */

libspectrum_byte hires_get_attr( void )
{
  return 0;
}

libspectrum_byte hires_convert_dec( libspectrum_byte attr )
{
  return 0;
}

/* Miscellaneous dummy code */

settings_info settings_current;

void startup_manager_register_no_dependencies(
  startup_manager_module module, startup_manager_init_fn init_fn,
  void *init_context, startup_manager_end_fn end_fn )
{
}
