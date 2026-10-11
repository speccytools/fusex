/* display_border.c: Spectrum border display handling
   Copyright (c) 1999-2026 Philip Kendall, Thomas Harte, Witold Filipczyk
                           and Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.
*/

#include "config.h"

#include <string.h>

#include "display.h"
#include "display_internal.h"
#include "machine.h"
#include "spectrum.h"
#include "peripherals/scld.h"
#include "peripherals/ulaplus.h"
#include "ui/uidisplay.h"

/* The current border colour */
libspectrum_byte display_lores_border;
libspectrum_byte display_hires_border;

/* The colour the border was last changed to: a colour index, which is 0-7 or,
   when the border is drawn from the ULA+ palette, a ULA+ colour */
static int display_last_border;

/* The current border colour */
int current_border[ DISPLAY_SCREEN_HEIGHT ][ DISPLAY_SCREEN_WIDTH_COLS ];

/* The border colour changes which have occurred in this frame.
   `t` is the T-state offset within the scanline at which the change becomes
   visible (post end-of-OUT alignment), measured from the T-state where this
   line's display column 0 begins. Range 0..DISPLAY_TSTATES_PER_LINE_VISIBLE. */
struct border_change_t {
  int t, y;
  int colour;
};

static struct border_change_t border_change_end_sentinel =
{ DISPLAY_TSTATES_PER_LINE_VISIBLE, DISPLAY_SCREEN_HEIGHT - 1, 0 };

static int border_changes_last;
static struct border_change_t *border_changes;

static struct border_change_t *
alloc_change( void )
{
  static int border_changes_size;

  if( border_changes_size == border_changes_last ) {
    border_changes_size += 10;
    border_changes = libspectrum_renew( struct border_change_t,
                                        border_changes, border_changes_size );
  }
  return border_changes + border_changes_last++;
}

/* The colour the border is drawn in now: with ULA+ on, entry 8 + the border
   colour of the first colour lookup table */
static int
current_border_colour( void )
{
  int colour = scld_last_dec.name.hires ?
               display_hires_border : display_lores_border;

  if( display_ulaplus_active() )
    return DISPLAY_ULAPLUS_BASE + ulaplus_get_colour( 8 + ( colour & 0x07 ) );

  return colour;
}

static void
add_border_sentinel( void )
{
  struct border_change_t *sentinel = alloc_change();

  sentinel->t = sentinel->y = 0;
  sentinel->colour = current_border_colour();
}

int
display_border_init( void )
{
  border_changes_last = 0;
  if( border_changes ) libspectrum_free( border_changes );
  border_changes = NULL;

  add_border_sentinel();

  display_last_border = current_border_colour();

  return 0;
}

static void
push_border_change( int colour )
{
  /* OUT (#FE) is an 11-T-state instruction (Z80 OUT (n),A: 4 M1 + 3 operand
     + 4 I/O cycle); the ULA reads the new border value from the data bus
     during the I/O cycle. On entry here, `tstates` already reflects
     ula_contend_port_early and sits inside that cycle; +2 places the
     recorded transition where the new colour becomes visible on the beam. */
  libspectrum_dword t_event = tstates + 2;
  int beam_y, t_in_line;
  struct border_change_t *change;

  if( t_event < machine_current->line_times[ 0 ] ) {
    beam_y = 0;
    t_in_line = 0;
  } else {
    beam_y = ( t_event - machine_current->line_times[ 0 ] ) /
             machine_current->timings.tstates_per_line;
    if( beam_y >= DISPLAY_SCREEN_HEIGHT ) return;
    t_in_line = t_event - machine_current->line_times[ beam_y ];
    if( t_in_line > DISPLAY_TSTATES_PER_LINE_VISIBLE )
      t_in_line = DISPLAY_TSTATES_PER_LINE_VISIBLE;
  }

  change = alloc_change();
  change->t = t_in_line;
  change->y = beam_y;
  change->colour = colour;
}

/* Change border colour if the colour in use changes */
static void
check_border_change( void )
{
  int colour = current_border_colour();

  if( colour != display_last_border ) {
    push_border_change( colour );
    display_last_border = colour;
  }
}

void
display_border_recheck( void )
{
  check_border_change();
}

void
display_set_lores_border( int colour )
{
  if( display_lores_border != colour ) display_lores_border = colour;
  check_border_change();
}

void
display_set_hires_border( int colour )
{
  if( display_hires_border != colour ) display_hires_border = colour;
  check_border_change();
}

/* Per-line border pixel buffer. Populated from the change list, then flushed
   column-by-column. Sized for the widest display (Timex = 16 px/col). */
static int border_pixel_buf[ DISPLAY_SCREEN_WIDTH ];

/* Plot a border cell in which either colour is a ULA+ colour. A cell where the
   mode switched has a standard colour on one side and a ULA+ colour on the
   other. */
static void
flush_ulaplus_border_cell( int c, int y, int colour_left, int colour_right,
                           int transition_pix, int pix_per_bit )
{
  libspectrum_byte data;
  libspectrum_word ink, paper;
  libspectrum_dword chunk_detail, colours;
  int index = c + y * DISPLAY_SCREEN_WIDTH_COLS;

  paper = colour_left;

  if( transition_pix < 0 ) {
    data = 0x00;
    ink = paper;
  } else {
    data = (libspectrum_byte)
      ( ( 1 << ( 8 - transition_pix / pix_per_bit ) ) - 1 );
    ink = colour_right;
  }

  colours = ( ink << 16 ) | paper;
  chunk_detail = DISPLAY_LAST_SCREEN_ULAPLUS | data;

  if( display_last_screen[ index ] != chunk_detail ||
      display_last_colours[ index ] != colours ) {
    uidisplay_plot8( c, y, data, ink, paper );
    display_last_screen[ index ] = chunk_detail;
    display_last_colours[ index ] = colours;
    display_mark_screen_dirty( c, y );
  }
}

/* Plot the accumulated border_pixel_buf for row y, column-by-column. The
   buffer holds one colour per pixel; for each column we detect whether all
   pixels are identical (solid border, the common case) or there is exactly
   one mid-column transition (the OUT spacing guarantees at most one per
   4-T-state column). The transition is emitted via plot8's bitmap form. */
static void
flush_border_line( int y )
{
  const int pix_per_col = machine_current->timex ? 16 : 8;
  const int pix_per_bit = pix_per_col / 8;
  /* On paper rows, skip the middle columns: the paper rendering path owns
     display_last_screen for those columns and would be overwritten otherwise. */
  const int paper_row = ( y >= DISPLAY_BORDER_HEIGHT &&
                          y <  DISPLAY_BORDER_HEIGHT + DISPLAY_HEIGHT );
  const int paper_col_start = DISPLAY_BORDER_WIDTH_COLS;
  const int paper_col_end = DISPLAY_BORDER_WIDTH_COLS + DISPLAY_WIDTH_COLS;
  int c;

  for( c = 0; c < DISPLAY_SCREEN_WIDTH_COLS; c++ ) {
    int pix_start, colour_left, colour_right, transition_pix, p, index;
    libspectrum_dword chunk_detail;
    libspectrum_byte data, ink, paper;

    if( paper_row && c >= paper_col_start && c < paper_col_end ) continue;

    pix_start = c * pix_per_col;
    colour_left = border_pixel_buf[ pix_start ];
    colour_right = colour_left;
    transition_pix = -1;

    for( p = 1; p < pix_per_col; p++ ) {
      if( border_pixel_buf[ pix_start + p ] != colour_right ) {
        /* OUT (#FE) instructions are at least 11 T-states apart, so a single
           4-T-state column cannot hold two transitions. */
        if( transition_pix >= 0 ) break;
        transition_pix = p;
        colour_right = border_pixel_buf[ pix_start + p ];
      }
    }

    if( colour_left >= DISPLAY_ULAPLUS_BASE ||
        colour_right >= DISPLAY_ULAPLUS_BASE ) {
      flush_ulaplus_border_cell( c, y, colour_left, colour_right,
                                 transition_pix, pix_per_bit );
      continue;
    }

    if( transition_pix < 0 ) {
      chunk_detail = (libspectrum_dword) colour_left << 11;
      data = 0x00;
      ink = 0;
      paper = colour_left;
    } else {
      /* In plot8's data byte, bit (7-i) covers pixel i (Timex doubles each bit
         to cover 2 hires pixels). Clear bits left of the transition (paper =
         colour_left), set bits at or right of it (ink = colour_right). */
      int transition_bit = transition_pix / pix_per_bit;
      data = (libspectrum_byte) ( ( 1 << ( 8 - transition_bit ) ) - 1 );
      ink = colour_right;
      paper = colour_left;
      /* Pack the chunk_detail in the same (attr << 8) | bitmap layout the
         Sinclair paper renderer uses, so display_getpixel, screenshot, and
         movie capture decode the partial-column cell correctly. attr packs
         ink=colour_right (bits 0-2) and paper=colour_left (bits 3-5); bits
         6-7 stay clear so display_parse_attr treats bright and flash as 0. */
      chunk_detail = ( (libspectrum_dword) ( ( colour_left << 3 ) | colour_right ) << 8 )
                   |   (libspectrum_dword) data;
    }

    index = c + y * DISPLAY_SCREEN_WIDTH_COLS;
    if( display_last_screen[ index ] != chunk_detail ) {
      uidisplay_plot8( c, y, data, ink, paper );
      display_last_screen[ index ] = chunk_detail;
      display_mark_screen_dirty( c, y );
    }
  }
}

/* Take account of all the border colour changes which happened in this
   frame. Between each consecutive pair of changes, the swept region is
   painted with the first change's colour. The pixel buffer accumulates an
   entire scanline before being flushed so that mid-column transitions can be
   composited correctly. */
void
display_border_frame( void )
{
  const int pix_per_col = machine_current->timex ? 16 : 8;
  const int pix_per_tstate = pix_per_col / 4;
  const int line_pixels = DISPLAY_SCREEN_WIDTH_COLS * pix_per_col;
  struct border_change_t *end_sentinel;
  int cursor_y = 0;
  int cursor_pix = 0;
  int i;

  /* Put the final sentinel onto the list */
  end_sentinel = alloc_change();
  memcpy( end_sentinel, &border_change_end_sentinel,
          sizeof( struct border_change_t ) );

  for( i = 0; i < border_changes_last - 1; i++ ) {
    struct border_change_t *first = border_changes + i;
    struct border_change_t *second = border_changes + i + 1;
    int target_y = second->y;
    int target_pix = second->t * pix_per_tstate;
    int colour = first->colour;
    int p;

    if( target_pix > line_pixels ) target_pix = line_pixels;

    while( cursor_y < target_y ) {
      for( p = cursor_pix; p < line_pixels; p++ )
        border_pixel_buf[ p ] = colour;
      flush_border_line( cursor_y );
      cursor_y++;
      cursor_pix = 0;
    }
    if( target_pix > cursor_pix ) {
      for( p = cursor_pix; p < target_pix; p++ )
        border_pixel_buf[ p ] = colour;
      cursor_pix = target_pix;
    }
  }

  /* Flush the line the cursor finished on (always reached via the end sentinel) */
  flush_border_line( cursor_y );

  border_changes_last = 0;
  add_border_sentinel();
}
