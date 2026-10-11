/* display_render.c: Spectrum screen rendering
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

#include "display.h"
#include "display_internal.h"
#include "machine.h"
#include "peripherals/scld.h"
#include "peripherals/ulaplus.h"
#include "spectrum.h"
#include "ui/uidisplay.h"

/* The number of frames mod 32 that have elapsed. */
static int display_frame_count;
static int display_flash_reversed;

display_write_if_dirty_fn display_write_if_dirty;

/* Get the attribute byte or equivalent for the eight pixels starting at
   ( (8*x) , y ) */
static inline libspectrum_byte
display_get_attr_byte( int x, int y )
{
  libspectrum_byte attr;

  if( scld_last_dec.name.hires ) {
    attr = hires_get_attr();
  } else {

    libspectrum_word offset;

    if( scld_last_dec.name.b1 ) {
      offset = display_line_start[y] + x + ALTDFILE_OFFSET;
    } else if( scld_last_dec.name.altdfile ) {
      offset = display_attr_start[y] + x + ALTDFILE_OFFSET;
    } else {
      offset = display_attr_start[y] + x;
    }

    attr = RAM[ memory_current_screen ][ offset ];
  }

  return attr;
}

void
display_write_if_dirty_timex( int x, int y )
{
  int beam_x, beam_y;
  int index;
  libspectrum_word offset;
  libspectrum_byte *screen;
  libspectrum_byte data, data2;
  libspectrum_dword mode_data;
  libspectrum_dword last_chunk_detail;

  beam_x = x + DISPLAY_BORDER_WIDTH_COLS;
  beam_y = y + DISPLAY_BORDER_HEIGHT;
  offset = display_get_addr( x, y );

  /* Read byte, atrr/byte, and screen mode */
  screen = RAM[ memory_current_screen ];
  data = screen[ offset ];
  mode_data = scld_last_dec.byte;

  if( scld_last_dec.name.hires ) {
    switch( scld_last_dec.mask.scrnmode ) {

    case HIRESATTRALTD:
      offset = display_attr_start[ y ] + x + ALTDFILE_OFFSET;
      data2 = screen[ offset ];
      break;

    case HIRES:
      data2 = screen[ offset + ALTDFILE_OFFSET ];
      break;

    case HIRESDOUBLECOL:
      data2 = data;
      break;

    default: /* case HIRESATTR: */
      offset = display_attr_start[ y ] + x;
      data2 = screen[ offset ];
      break;

    }
  } else {
    data2 = display_get_attr_byte( x, y );
  }

  last_chunk_detail = ( display_flash_reversed << 24 ) | ( mode_data << 16 ) |
                      ( data2 << 8 ) | data;
  /* And draw it if it is different to what was there last time */
  index = beam_x + beam_y * DISPLAY_SCREEN_WIDTH_COLS;
  if( display_last_screen[ index ] != last_chunk_detail ) {
    libspectrum_byte ink, paper;
    if( scld_last_dec.name.hires ) {
      /* In hires mode the attr byte is not in data2, so we must look it up. */
      display_parse_attr( display_get_attr_byte( x, y ), &ink, &paper );
      libspectrum_word hires_data = ( data << 8 ) + data2;
      uidisplay_plot16( beam_x, beam_y, hires_data, ink, paper );
    } else {
      /* In lores mode data2 already holds the attr byte (set above), so
         parse it directly instead of reading it a second time. */
      display_parse_attr( data2, &ink, &paper );
      uidisplay_plot8( beam_x, beam_y, data, ink, paper );
    }

    /* Update last display record */
    display_last_screen[ index ] = last_chunk_detail;

    /* And now mark it dirty */
    display_mark_screen_dirty( beam_x, beam_y );
  }
}

static inline void
pentagon_16c_get_colour( libspectrum_byte data, libspectrum_byte *colour1,
                         libspectrum_byte *colour2 )
{
  *colour1 = ( data & 0x07 ) + ( ( data & 0x40 ) >> 3 );
  *colour2 = ( ( data & 0x38 ) >> 3 ) + ( ( data & 0x80 ) >> 4 );
}

/* In this mode we need to gather the pixel information for the 8 pixels to
   be displayed, if current screen is 5 we need to read from pages 5 and 4,
   and if current screen is 7 we need to read from pages 7 and 6. */
void
display_write_if_dirty_pentagon_16_col( int x, int y )
{
  int beam_x, beam_y;
  int index;
  libspectrum_word offset;
  libspectrum_byte *screen;
  libspectrum_byte data1, data2, data3, data4;
  libspectrum_dword last_chunk_detail;
  libspectrum_byte colour1, colour2;

  /* We need to read the pixels from the appropriate two pages and write them
     out to the frame buffer */
  int memory_screen_page_1 = 5;
  int memory_screen_page_2 = 4;

  if( memory_current_screen == 7 ) {
    memory_screen_page_1 = 7;
    memory_screen_page_2 = 6;
  }

  beam_x = x + DISPLAY_BORDER_WIDTH_COLS;
  beam_y = y + DISPLAY_BORDER_HEIGHT;
  offset = display_get_addr( x, y );

  /* Read byte, atrr/byte, and screen mode */
  screen = RAM[ memory_screen_page_1 ];
  data2 = screen[ offset ];
  data4 = screen[ offset + ALTDFILE_OFFSET ];
  screen = RAM[ memory_screen_page_2 ];
  data1 = screen[ offset ];
  data3 = screen[ offset + ALTDFILE_OFFSET ];

  /* This is a bit of a cheat - we'd normally encode the screen mode in here
     as well to support screen mode mixing. I doubt there is much call for
     mixing 16 colour mode with other modes so will assume that as long as
     we are in 16 colour mode the screen we draw is in that mode as it seems
     a shame to chuck more memory at supporting just this obscure mode */
  last_chunk_detail = ( data4 << 24 ) | ( data3 << 16 ) | ( data2 <<
  8 ) | data1;

  /* And draw it if it is different to what was there last time */
  index = beam_x + beam_y * DISPLAY_SCREEN_WIDTH_COLS;

  if( display_last_screen[ index ] != last_chunk_detail ) {
    /* Print pixel 1 & 2 from screen_page_2 base, pixel 3 & 4 from
       screen_page_1 base, pixel 5 & 6 from screen_page_2 ALTDFILE_OFFSET,
       pixel 7 & 8 from screen_page_1 ALTDFILE_OFFSET */

    int draw_x = beam_x << 3;
    pentagon_16c_get_colour( data1, &colour1, &colour2 );
    uidisplay_putpixel( draw_x++, beam_y, colour1 );
    uidisplay_putpixel( draw_x++, beam_y, colour2 );
    pentagon_16c_get_colour( data2, &colour1, &colour2 );
    uidisplay_putpixel( draw_x++, beam_y, colour1 );
    uidisplay_putpixel( draw_x++, beam_y, colour2 );
    pentagon_16c_get_colour( data3, &colour1, &colour2 );
    uidisplay_putpixel( draw_x++, beam_y, colour1 );
    uidisplay_putpixel( draw_x++, beam_y, colour2 );
    pentagon_16c_get_colour( data4, &colour1, &colour2 );
    uidisplay_putpixel( draw_x++, beam_y, colour1 );
    uidisplay_putpixel( draw_x, beam_y, colour2 );

    /* Update last display record */
    display_last_screen[ index ] = last_chunk_detail;

    /* And now mark it dirty */
    display_mark_screen_dirty( beam_x, beam_y );
  }
}

int
display_ulaplus_active( void )
{
  /* Other display modes, such as the Pentagon 16 colour one, ignore ULA+ */
  return display_write_if_dirty == display_write_if_dirty_sinclair &&
         ulaplus_is_enabled();
}

/* Draw a cell in ULA+ mode: the attribute selects a colour lookup table of 16
   palette entries with its flash and bright bits, and ink and paper index into
   it. FLASH does not flash. The palette is read as the cell is drawn, so a
   change to it affects only the cells drawn afterwards. */
static void
display_write_if_dirty_ulaplus( int x, int y )
{
  int beam_x, beam_y;
  int index;
  libspectrum_byte data, attr, clut_base;
  libspectrum_word ink, paper;
  libspectrum_dword last_chunk_detail, colours;

  beam_x = x + DISPLAY_BORDER_WIDTH_COLS;
  beam_y = y + DISPLAY_BORDER_HEIGHT;

  data = RAM[ memory_current_screen ][ display_get_addr( x, y ) ];
  attr = display_get_attr_byte( x, y );

  clut_base = ( attr >> 6 ) * 16;
  ink = DISPLAY_ULAPLUS_BASE + ulaplus_get_colour( clut_base + ( attr & 0x07 ) );
  paper = DISPLAY_ULAPLUS_BASE +
          ulaplus_get_colour( clut_base + 8 + ( ( attr >> 3 ) & 0x07 ) );
  colours = ( ink << 16 ) | paper;

  last_chunk_detail = DISPLAY_LAST_SCREEN_ULAPLUS | ( attr << 8 ) | data;
  index = beam_x + beam_y * DISPLAY_SCREEN_WIDTH_COLS;

  if( display_last_screen[ index ] != last_chunk_detail ||
      display_last_colours[ index ] != colours ) {
    uidisplay_plot8( beam_x, beam_y, data, ink, paper );

    display_last_screen[ index ] = last_chunk_detail;
    display_last_colours[ index ] = colours;

    display_mark_screen_dirty( beam_x, beam_y );
  }
}

void
display_write_if_dirty_sinclair( int x, int y )
{
  int beam_x, beam_y;
  int index;
  libspectrum_word offset;
  libspectrum_byte *screen;
  libspectrum_byte data, data2;
  libspectrum_dword last_chunk_detail;

  if( ulaplus_is_enabled() ) {
    display_write_if_dirty_ulaplus( x, y );
    return;
  }

  beam_x = x + DISPLAY_BORDER_WIDTH_COLS;
  beam_y = y + DISPLAY_BORDER_HEIGHT;
  offset = display_get_addr( x, y );

  /* Read byte, atrr/byte, and screen mode */
  screen = RAM[ memory_current_screen ];
  data = screen[ offset ];
  data2 = display_get_attr_byte( x, y );

  last_chunk_detail = ( display_flash_reversed << 24 ) | ( data2 << 8 ) | data;
  /* And draw it if it is different to what was there last time */
  index = beam_x + beam_y * DISPLAY_SCREEN_WIDTH_COLS;
  if( display_last_screen[ index ] != last_chunk_detail ) {
    libspectrum_byte ink, paper;
    display_parse_attr( data2, &ink, &paper );
    uidisplay_plot8( beam_x, beam_y, data, ink, paper );

    /* Update last display record */
    display_last_screen[ index ] = last_chunk_detail;

    /* And now mark it dirty */
    display_mark_screen_dirty( beam_x, beam_y );
  }
}

void
display_parse_attr( libspectrum_byte attr,
                    libspectrum_byte *ink, libspectrum_byte *paper )
{
  if( ( attr & 0x80 ) && display_flash_reversed ) {
    *ink  = ( attr & ( 0x0f << 3 ) ) >> 3;
    *paper = ( attr & 0x07 ) + ( ( attr & 0x40 ) >> 3 );
  } else {
    *ink = ( attr & 0x07 ) + ( ( attr & 0x40 ) >> 3 );
    *paper = ( attr & ( 0x0f << 3 ) ) >> 3;
  }
}

display_dirty_flashing_fn display_dirty_flashing;

void
display_dirty_flashing_timex( void )
{
  libspectrum_word offset;
  libspectrum_byte *screen, attr;

  screen = RAM[ memory_current_screen ];

  if( !scld_last_dec.name.hires ) {
    if( scld_last_dec.name.b1 ) {

      for( offset = ALTDFILE_OFFSET;
           offset < ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES;
           offset++ ) {
        attr = screen[ offset ];
        if( attr & 0x80 ) display_dirty8( offset - ALTDFILE_OFFSET );
      }

    } else if( scld_last_dec.name.altdfile ) {

      for( offset = ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES;
           offset < ALTDFILE_OFFSET + DISPLAY_FILE_SIZE;
           offset++ ) {
        attr = screen[ offset ];
        if( attr & 0x80 ) display_dirty64( offset - ALTDFILE_OFFSET );
      }

    } else { /* Standard Speccy screen */

      display_dirty_flashing_sinclair();

    }
  }
}

void
display_dirty_flashing_pentagon_16_col( void )
{
  /* No flash attribute in 16 colour mode */
}

void
display_dirty_flashing_sinclair( void )
{
  libspectrum_word offset;
  libspectrum_byte *screen, attr;

  /* ULA+ uses the flash bit to select a colour lookup table */
  if( ulaplus_is_enabled() ) return;

  screen = RAM[ memory_current_screen ];

  /* Standard Speccy screen */
  for( offset = DISPLAY_PIXEL_BYTES; offset < DISPLAY_FILE_SIZE; offset++ ) {
    attr = screen[ offset ];
    if( attr & 0x80 ) display_dirty64( offset );
  }
}

void
display_render_init( void )
{
  display_frame_count = 0;
  display_flash_reversed = 0;
}

void
display_render_frame( void )
{
  display_frame_count++;
  if( display_frame_count == DISPLAY_FLASH_HALF_PERIOD ) {
    display_flash_reversed = 1;
    display_dirty_flashing();
  } else if( display_frame_count == DISPLAY_FLASH_PERIOD ) {
    display_flash_reversed = 0;
    display_dirty_flashing();
    display_frame_count = 0;
  }
}

#ifdef DISPLAYTEST

void
display_reset_frame_count( void )
{
  /* We set the frame count to DISPLAY_FLASH_PERIOD - 1 so the next call
     to display_frame() pushes us back to zero and resets
     display_flash_reversed */
  display_frame_count = DISPLAY_FLASH_PERIOD - 1;
}

void
display_set_flash_reversed( int reversed )
{
  display_flash_reversed = reversed;
}

#endif /* #ifdef DISPLAYTEST */
