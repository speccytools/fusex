/* display.c: Routines for printing the Spectrum screen
   Copyright (c) 1999-2026 Philip Kendall, Thomas Harte, Witold Filipczyk
                           and Fredrick Meunier

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

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "debugger/gdbserver.h"
#include "fuse.h"
#include "infrastructure/startup_manager.h"
#include "machine.h"
#include "movie.h"
#include "peripherals/scld.h"
#include "rectangle.h"
#include "screenshot.h"
#include "settings.h"
#include "spectrum.h"
#include "ui/ui.h"
#include "ui/uidisplay.h"

/* Set once we have initialised the UI */
int display_ui_initialised = 0;

/* The current border colour */
libspectrum_byte display_lores_border;
libspectrum_byte display_hires_border;
libspectrum_byte display_last_border;

/* Stores the pixel, attribute and SCLD screen mode information used to
   draw each 8x1 group of pixels (including border) last frame */
libspectrum_dword
display_last_screen[ DISPLAY_SCREEN_WIDTH_COLS * DISPLAY_SCREEN_HEIGHT ];

/* Offsets as to where the data and the attributes for each pixel
   line start */
libspectrum_word display_line_start[ DISPLAY_HEIGHT ];
libspectrum_word display_attr_start[ DISPLAY_HEIGHT ];

/* The number of frames mod 32 that have elapsed.
    0<=d_f_c<16 => Flashing characters are normal
   16<=d_f_c<32 => Flashing characters are reversed
*/
static int display_frame_count;
static int display_flash_reversed;

/* Which eight-pixel chunks on each line (including border) need to
   be redisplayed. Bit 0 corresponds to pixels 0-7, bit 39 to
   pixels 311-319. */
static libspectrum_qword display_is_dirty[ DISPLAY_SCREEN_HEIGHT ];

/* Which eight-pixel chunks on each line may need to be redisplayed. Bit 0
   corresponds to pixels 0-7, bit 31 to pixels 248-255. */
static libspectrum_dword display_maybe_dirty[ DISPLAY_HEIGHT ];

/* This value signifies that the entire line must be redisplayed */
static libspectrum_qword display_all_dirty;

/* Used to signify that we're redrawing the entire screen */
static int display_redraw_all;

/* The last point at which we updated the screen display */
static int critical_region_x = 0, critical_region_y = 0;

/* Cache the most-recently computed beam position keyed on tstates.
   get_beam_position() is called once per dirty screen write, and attribute
   writes trigger eight consecutive calls all at the same tstates value.
   Caching the fully adjusted/clamped screen coordinates avoids the border
   subtraction and clamping branches on each cache hit (~7/8 of calls). */
static libspectrum_dword display_cached_beam_tstates = (libspectrum_dword)-1;
static int display_cached_screen_x, display_cached_screen_y;

/* The border colour changes which have occurred in this frame.
   `t` is the T-state offset within the scanline at which the change becomes
   visible (post end-of-OUT alignment), measured from the T-state where this
   line's display column 0 begins. Range 0..DISPLAY_TSTATES_PER_LINE_VISIBLE. */
struct border_change_t {
  int t, y;
  int colour;
};

display_dirty_fn display_dirty;
display_write_if_dirty_fn display_write_if_dirty;

static struct border_change_t border_change_end_sentinel =
  { DISPLAY_TSTATES_PER_LINE_VISIBLE, DISPLAY_SCREEN_HEIGHT - 1, 0 };

/* The current border colour */
int current_border[ DISPLAY_SCREEN_HEIGHT ][ DISPLAY_SCREEN_WIDTH_COLS ];

static void display_dirty8( libspectrum_word address );
static void display_dirty64( libspectrum_word address );

static int border_changes_last = 0;
static struct border_change_t *border_changes = NULL;

static struct border_change_t *
alloc_change(void)
{
  static int border_changes_size = 0;

  if( border_changes_size == border_changes_last ) {
    border_changes_size += 10;
    border_changes = libspectrum_renew( struct border_change_t,
                                        border_changes, border_changes_size );
  }
  return border_changes + border_changes_last++; 
}

static void
add_border_sentinel( void )
{
  struct border_change_t *sentinel = alloc_change();

  sentinel->t = sentinel->y = 0;
  sentinel->colour = scld_last_dec.name.hires ?
                            display_hires_border : display_lores_border;
}

int
display_init( int *argc, char ***argv )
{
  int i, j, k, y;

  if(ui_init(argc, argv))
    return 1;

  /* Set up the 'all pixels must be refreshed' marker: one bit per column */
  display_all_dirty = ( (libspectrum_qword)1 << DISPLAY_SCREEN_WIDTH_COLS ) - 1;

  for(i=0;i<3;i++)
    for(j=0;j<8;j++)
      for(k=0;k<8;k++)
	display_line_start[ (64*i) + (8*j) + k ] =
	  32 * ( (64*i) + j + (k*8) );

  for(y=0;y<DISPLAY_HEIGHT;y++) {
    display_attr_start[y]=DISPLAY_PIXEL_BYTES + (DISPLAY_WIDTH_COLS*(y/8));
  }

  display_frame_count=0; display_flash_reversed=0;

  display_refresh_all();

  border_changes_last = 0;
  if( border_changes ) {
    libspectrum_free( border_changes );
  }
  border_changes = NULL;
  add_border_sentinel();
  display_last_border = scld_last_dec.name.hires ?
                            display_hires_border : display_lores_border;

  return 0;
}

static int
display_init_wrapper( void *context )
{
  display_startup_context *typed_context =
    (display_startup_context*) context;

  return display_init( typed_context->argc, typed_context->argv );
}

void
display_register_startup( display_startup_context *context )
{
  /* The Wii has an explicit call to display_init for now */
#ifndef GEKKO
  startup_manager_register_no_dependencies( STARTUP_MANAGER_MODULE_DISPLAY,
                                            display_init_wrapper, context,
                                            NULL );
#endif                          /* #ifndef GEKKO */
}

/* Mark as 'dirty' the pixels which have been changed by a write to
   'offset' within the RAM page containing the screen */
void
display_dirty_timex( libspectrum_word offset )
{
  switch ( scld_last_dec.mask.scrnmode ) {

    case STANDARD: /* standard Speccy screen */
    case HIRESATTR: /* strange mode */
      if( offset >= DISPLAY_FILE_SIZE ) break;
      if( offset <  DISPLAY_PIXEL_BYTES ) {
        display_dirty8( offset );
      } else {
        display_dirty64( offset );
      }
      break;

    case ALTDFILE: /* second screen */
    case HIRESATTRALTD: /* strange mode using second screen */      
      if( offset < ALTDFILE_OFFSET ||
          offset >= ALTDFILE_OFFSET + DISPLAY_FILE_SIZE ) break;
      if( offset < ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES ) {
        display_dirty8( offset - ALTDFILE_OFFSET );
      } else {
        display_dirty64( offset - ALTDFILE_OFFSET );
      }
      break;

    case EXTCOLOUR: /* extended colours */
    case HIRES: /* hires mode */
      if( offset >= ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES ) break;
      if( offset >= DISPLAY_PIXEL_BYTES && offset < ALTDFILE_OFFSET ) break;
      if( offset >= ALTDFILE_OFFSET ) offset -= ALTDFILE_OFFSET;
      display_dirty8( offset );
      break;

    default:
    /* case EXTCOLALTD: extended colours, but attributes and data
       taken from second screen */
    /* case HIRESDOUBLECOL: hires mode, but data taken only from
       second screen */
      if( offset >= ALTDFILE_OFFSET &&
          offset < ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES )
	display_dirty8( offset - ALTDFILE_OFFSET );
      break;
  }
}

void
display_dirty_pentagon_16_col( libspectrum_word offset )
{
  /* The only relevant sections of the page will be the two DISPLAY_PIXEL_BYTES
     sections separated by ALTDFILE_OFFSET, which have the same display offset */
  if( offset >= ALTDFILE_OFFSET ) offset -= ALTDFILE_OFFSET;
  /* No attributes are relevent in this mode */
  if( offset <  DISPLAY_PIXEL_BYTES ) {
    display_dirty8( offset );
  }
}

void
display_dirty_sinclair( libspectrum_word offset )
{
  if( offset >= DISPLAY_FILE_SIZE ) return;
  if( offset <  DISPLAY_PIXEL_BYTES ) {
    display_dirty8( offset );
  } else {
    display_dirty64( offset );
  }
}

/* Get the attribute byte or equivalent for the eight pixels starting at
   ( (8*x) , y ) */
static inline libspectrum_byte
display_get_attr_byte( int x, int y )
{
  libspectrum_byte attr;

  if ( scld_last_dec.name.hires ) {
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

static void
update_dirty_rects( void )
{
  int start, y;

  for( y=0; y<DISPLAY_SCREEN_HEIGHT; y++ ) {
    int x = 0;
    while( display_is_dirty[y] ) {

#ifdef __GNUC__
      /* Skip to the first dirty bit using a single BSF/TZCNT instruction */
      int skip = __builtin_ctzll( (unsigned long long)display_is_dirty[y] );
      display_is_dirty[y] >>= skip;
      x += skip;

      start = x;

      /* Count the run of consecutive dirty bits using ~value */
      int run = __builtin_ctzll( (unsigned long long)~display_is_dirty[y] );
      display_is_dirty[y] >>= run;
      x += run;

      rectangle_add( y, start, run );
#else
      /* Find the first dirty chunk on this row */
      while( !( display_is_dirty[y] & 0x01 ) ) {
        display_is_dirty[y] >>= 1;
        x++;
      }

      start = x;

      /* Walk to the end of the dirty region */
      do {
        display_is_dirty[y] >>= 1;
        x++;
      } while( display_is_dirty[y] & 0x01 );

      rectangle_add( y, start, x - start );
#endif
    }

    /* compress the active rectangles list */
    rectangle_end_line( y );
  }

  /* Force all rectangles into the inactive list */
  rectangle_end_line( DISPLAY_SCREEN_HEIGHT );
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

  last_chunk_detail = (display_flash_reversed << 24) | (mode_data << 16) |
                      (data2 << 8) | data;
  /* And draw it if it is different to what was there last time */
  index = beam_x + beam_y * DISPLAY_SCREEN_WIDTH_COLS;
  if( display_last_screen[ index ] != last_chunk_detail ) {
    libspectrum_byte ink, paper;
    if( scld_last_dec.name.hires ) {
      /* In hires mode the attr byte is not in data2, so we must look it up. */
      display_parse_attr( display_get_attr_byte( x, y ), &ink, &paper );
      libspectrum_word hires_data = (data << 8) + data2;
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
    display_is_dirty[ beam_y ] |= ( (libspectrum_qword)1 << beam_x );
  }
}

static inline void
pentagon_16c_get_colour( libspectrum_byte data, libspectrum_byte *colour1,
                         libspectrum_byte *colour2 )
{
  *colour1 = (data & 0x07) + ( (data & 0x40) >> 3 );
  *colour2 = ( (data & 0x38) >> 3 ) + ( (data & 0x80) >> 4 );
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
  last_chunk_detail = (data4 << 24) | (data3 << 16) | (data2 << 8) | data1;

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
    uidisplay_putpixel( draw_x  , beam_y, colour2 );

    /* Update last display record */
    display_last_screen[ index ] = last_chunk_detail;

    /* And now mark it dirty */
    display_is_dirty[ beam_y ] |= ( (libspectrum_qword)1 << beam_x );
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

  beam_x = x + DISPLAY_BORDER_WIDTH_COLS;
  beam_y = y + DISPLAY_BORDER_HEIGHT;
  offset = display_get_addr( x, y );

  /* Read byte, atrr/byte, and screen mode */
  screen = RAM[ memory_current_screen ];
  data = screen[ offset ];
  data2 = display_get_attr_byte( x, y );

  last_chunk_detail = (display_flash_reversed << 24) | (data2 << 8) | data;
  /* And draw it if it is different to what was there last time */
  index = beam_x + beam_y * DISPLAY_SCREEN_WIDTH_COLS;
  if( display_last_screen[ index ] != last_chunk_detail ) {
    libspectrum_byte ink, paper;
    display_parse_attr( data2, &ink, &paper );
    uidisplay_plot8( beam_x, beam_y, data, ink, paper );

    /* Update last display record */
    display_last_screen[ index ] = last_chunk_detail;

    /* And now mark it dirty */
    display_is_dirty[ beam_y ] |= ( (libspectrum_qword)1 << beam_x );
  }
}

/* Plot any dirty data from ( x, y ) to ( end, y ) of the critical
   region to the drawing region */
static void
copy_critical_region_line( int y, int x, int end )
{
  libspectrum_dword bit_mask, dirty;

  /* Nothing to do for an empty range; also guards against undefined
     behaviour in the shift expressions below when end <= x (which can
     occur legitimately when the beam is at column 0 at the start of a
     display line). */
  if( end <= x ) return;

  if( x < DISPLAY_WIDTH_COLS ) {

    /* Build a mask for the bits we're interested in */
    bit_mask = display_all_dirty;

    bit_mask >>= x;
    bit_mask <<= x + ( 32 - end );
    bit_mask >>= ( 32 - end );

    /* Get the bits we're interested in */
    dirty = ( display_maybe_dirty[y] & bit_mask ) >> x;

    /* And remove those bits from the dirty mask */
    display_maybe_dirty[y] &= ~bit_mask;

  } else {

    dirty = 0;

  }

  while( dirty ) {

#ifdef __GNUC__
    /* Skip to the first dirty bit using a single BSF/TZCNT instruction */
    int skip = __builtin_ctz( (unsigned int)dirty );
    dirty >>= skip;
    x += skip;
#else
    /* Find the first dirty chunk on this row */
    while( !( dirty & 0x01 ) ) {

      dirty >>= 1;
      x++;

    }
#endif

    /* Walk to the end of the dirty region, writing the bytes to the
       drawing area along the way */
    do {

      display_write_if_dirty( x, y );

      dirty >>= 1;
      x++;

    } while( dirty & 0x01 );

  }
  
}

/* Copy any dirty data from the critical region to the drawing region */
static void
copy_critical_region( int beam_x, int beam_y )
{
  if( critical_region_y == beam_y ) {

    copy_critical_region_line( critical_region_y, critical_region_x, beam_x );

  } else {

    copy_critical_region_line( critical_region_y++, critical_region_x,
			       DISPLAY_WIDTH_COLS );
  
    for( ; critical_region_y < beam_y; critical_region_y++ )
      copy_critical_region_line( critical_region_y, 0,
				 DISPLAY_WIDTH_COLS );

    copy_critical_region_line( critical_region_y, 0, beam_x );
  }

  critical_region_x = beam_x;
}

static inline void
get_beam_position( int *x, int *y )
{
  if( tstates < machine_current->line_times[ 0 ] ) {
    *x = *y = -1;
    return;
  }

  *y = ( tstates - machine_current->line_times[ 0 ] ) /
    machine_current->timings.tstates_per_line;

  if( *y >= 0 && *y <= DISPLAY_SCREEN_HEIGHT )
    *x = ( tstates - machine_current->line_times[ *y ] ) / 4;
  else *x = 0;
}

inline static void
update_critical_internal( int x, int y )
{
  int beam_x, beam_y;

  if( tstates != display_cached_beam_tstates ) {
    get_beam_position( &beam_x, &beam_y );
    display_cached_beam_tstates = tstates;

    beam_x -= DISPLAY_BORDER_WIDTH_COLS;
    beam_y -= DISPLAY_BORDER_HEIGHT;

    if( beam_y < 0 ) {
      beam_x = beam_y = 0;
    } else if( beam_y >= DISPLAY_HEIGHT ) {
      beam_x = DISPLAY_WIDTH_COLS;
      beam_y = DISPLAY_HEIGHT - 1;
    }

    if( beam_x < 0 ) {
      beam_x = 0;
    } else if( beam_x > DISPLAY_WIDTH_COLS ) {
      beam_x = DISPLAY_WIDTH_COLS;
    }

    display_cached_screen_x = beam_x;
    display_cached_screen_y = beam_y;
  }

  if(   y <  display_cached_screen_y                              ||
      ( y == display_cached_screen_y && x < display_cached_screen_x ) )
    copy_critical_region( display_cached_screen_x, display_cached_screen_y );
}

void
display_update_critical( int x, int y )
{
  update_critical_internal( x, y );
}

/* Mark the 8-pixel chunk at (x,y) as maybe dirty and update the critical
   region as appropriate */
static inline void
display_dirty_chunk( int x, int y )
{
  /* If the write is between the start of the critical region and the
     current beam position, then we must copy the critical region now */
  if(   y >  critical_region_y                             ||
      ( y == critical_region_y && x >= critical_region_x )    ) {

    update_critical_internal( x, y );
  }

  display_maybe_dirty[y] |= ( (libspectrum_dword)1 << x );
}

static void
display_dirty8( libspectrum_word offset )
{
  int x, y;

  /* The ZX Spectrum pixel area uses a non-linear address encoding:
       bits  4-0:  column (x, 0-31)
       bits  7-5:  character row within third (j, 0-7)
       bits 10-8:  pixel row within character (k, 0-7)
       bits 12-11: third of screen (i, 0-2)
     Screen line y = 64*i + 8*j + k
     GCC reduces the multiplications to shifts. */
  x = offset & ( DISPLAY_WIDTH_COLS - 1 );
  y = 64 * ( ( offset >> 11 ) & 3 )
    +  8 * ( ( offset >>  5 ) & 7 )
    +      ( ( offset >>  8 ) & 7 );

  display_dirty_chunk( x, y );
}

static void
display_dirty64( libspectrum_word offset )
{
  int i, x, y;
  int idx = offset - DISPLAY_PIXEL_BYTES;

  /* The attribute area is laid out linearly: column x, row y, so:
     x = idx % DISPLAY_WIDTH_COLS
     y = (idx / DISPLAY_WIDTH_COLS) * 8
     Both divisions are exact powers of two and optimise to shifts/masks. */
  x = idx & ( DISPLAY_WIDTH_COLS - 1 );
  y = ( idx / DISPLAY_WIDTH_COLS ) * 8;

  for( i = 0; i < 8; i++ ) display_dirty_chunk( x, y + i );
}

void
display_parse_attr( libspectrum_byte attr,
		    libspectrum_byte *ink, libspectrum_byte *paper )
{
  if( (attr & 0x80) && display_flash_reversed ) {
    *ink  = (attr & ( 0x0f << 3 ) ) >> 3;
    *paper= (attr & 0x07) + ( (attr & 0x40) >> 3 );
  } else {
    *ink= (attr & 0x07) + ( (attr & 0x40) >> 3 );
    *paper= (attr & ( 0x0f << 3 ) ) >> 3;
  }
}

/* Get the attributes for the eight pixels starting at
   ( (8*x) , y ) */
static void
display_get_attr( int x, int y,
                  libspectrum_byte *ink, libspectrum_byte *paper )
{
  display_parse_attr( display_get_attr_byte( x, y ), ink, paper );
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
  if( scld_last_dec.name.hires &&
      display_hires_border != display_last_border ) {
    push_border_change( display_hires_border );
    display_last_border = display_hires_border;
  } else if( !scld_last_dec.name.hires &&
             display_lores_border != display_last_border ) {
    push_border_change( display_lores_border );
    display_last_border = display_lores_border;
  }
}

void
display_set_lores_border( int colour )
{
  if( display_lores_border != colour ) {
    display_lores_border = colour;
  }
  check_border_change();
}

void
display_set_hires_border( int colour )
{
  if( display_hires_border != colour ) {
    display_hires_border = colour;
  }
  check_border_change();
}

/* Per-line border pixel buffer. Populated from the change list, then flushed
   column-by-column. Sized for the widest display (Timex = 16 px/col). */
static int border_pixel_buf[ DISPLAY_SCREEN_WIDTH ];

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
      display_is_dirty[ y ] |= ( (libspectrum_qword)1 << c );
    }
  }
}

/* Take account of all the border colour changes which happened in this
   frame. Between each consecutive pair of changes, the swept region is
   painted with the first change's colour. The pixel buffer accumulates an
   entire scanline before being flushed so that mid-column transitions can be
   composited correctly. */
static void
update_border( void )
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

/* Send the updated screen to the UI-specific code */
static void
update_ui_screen( void )
{
  static int frame_count = 0;
  int scale = machine_current->timex ? 2 : 1;
  size_t i;
  struct rectangle *ptr;

  if( settings_current.frame_rate <= ++frame_count ) {
    frame_count = 0;
    if( movie_recording ) {
      movie_start_frame();
    }

    if( display_redraw_all ) {
      if( movie_recording ) {
        movie_add_area( 0, 0, DISPLAY_ASPECT_WIDTH >> 3,
                        DISPLAY_SCREEN_HEIGHT );
      }
      uidisplay_area( 0, 0,
                      scale * DISPLAY_ASPECT_WIDTH,
                      scale * DISPLAY_SCREEN_HEIGHT );
      display_redraw_all = 0;
    } else {
      for( i = 0, ptr = rectangle_inactive;
           i < rectangle_inactive_count;
           i++, ptr++ ) {
            if( movie_recording ) {
              movie_add_area( ptr->x, ptr->y, ptr->w, ptr->h );
            }
              uidisplay_area( 8 * scale * ptr->x, scale * ptr->y,
                        8 * scale * ptr->w, scale * ptr->h );
      }
    }

    rectangle_inactive_count = 0;

    uidisplay_frame_end();
  }
}

int
display_frame( void )
{
  /* Invalidate the beam position cache so get_beam_position() is called
     fresh in the new frame (machine timing may have changed on reset). */
  display_cached_beam_tstates = (libspectrum_dword)-1;

  /* Copy all the critical region to the display */
  copy_critical_region( DISPLAY_WIDTH_COLS, DISPLAY_HEIGHT - 1 );
  critical_region_x = critical_region_y = 0;

  update_border();
  update_dirty_rects();
  update_ui_screen();

  display_frame_count++;
  if(display_frame_count==DISPLAY_FLASH_HALF_PERIOD) {
    display_flash_reversed=1;
    display_dirty_flashing();
  } else if(display_frame_count==DISPLAY_FLASH_PERIOD) {
    display_flash_reversed=0;
    display_dirty_flashing();
    display_frame_count=0;
  }
  
  return 0;
}

display_dirty_flashing_fn display_dirty_flashing;

void
display_dirty_flashing_timex(void)
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

      for( offset= ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES;
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
display_dirty_flashing_pentagon_16_col(void)
{
  /* No flash attribute in 16 colour mode */
}

void
display_dirty_flashing_sinclair(void)
{
  libspectrum_word offset;
  libspectrum_byte *screen, attr;

  screen = RAM[ memory_current_screen ];
  
  /* Standard Speccy screen */
  for( offset = DISPLAY_PIXEL_BYTES; offset < DISPLAY_FILE_SIZE; offset++ ) {
    attr = screen[ offset ];
    if( attr & 0x80 ) display_dirty64( offset );
  }
}

void display_refresh_main_screen(void)
{
  size_t i;

  for( i = 0; i < DISPLAY_HEIGHT; i++ )
    display_maybe_dirty[i] = display_all_dirty;
}

void display_refresh_all(void)
{
  size_t i;

  display_redraw_all = 1;

  display_refresh_main_screen();

  for( i = 0; i < DISPLAY_SCREEN_HEIGHT; i++ )
    display_is_dirty[i] = display_all_dirty;

  memset( display_last_screen, 0xff,
          DISPLAY_SCREEN_WIDTH_COLS * DISPLAY_SCREEN_HEIGHT 
          * sizeof(libspectrum_dword) );

#ifndef DISPLAYTEST
  gdbserver_refresh_status();
#endif
}

/* Fetch pixel (x, y). On a Timex this will be a point on a 640x480 canvas,
   on a Sinclair/Amstrad/Russian clone this will be a point on a 320x240
   canvas */
int
display_getpixel( int x, int y )
{
  libspectrum_byte ink, paper;
  libspectrum_byte data, data2;
  int mask = 1 << (7 - (x % 8));
  int index;

  if( machine_current->timex ) {
    int column = x >> 4;
    scld mode_data;

    y >>= 1;
    index = column + y * DISPLAY_SCREEN_WIDTH_COLS;

    data = display_last_screen[ index ] & 0xff;
    data2 = (display_last_screen[ index ] & 0xff00)>>8;
    mode_data.byte = (display_last_screen[ index ] & 0xff0000)>>16;

    if( mode_data.name.hires ) {
      if( x % 16 > 7 ) data = data2;
      display_parse_attr( hires_convert_dec( mode_data.byte ), &ink, &paper );
    } else {
      /* divide x by two to get the same value for adjacent pixels */
      mask = 1 << (7 - ((x>>1) % 8));
      display_parse_attr( data2, &ink, &paper );
    }
  } else {
    int column = x >> 3;

    index = column + y * DISPLAY_SCREEN_WIDTH_COLS;

    data = display_last_screen[ index ] & 0xff;
    data2 = (display_last_screen[ index ] & 0xff00)>>8;

    display_parse_attr( data2, &ink, &paper );
  }

  if( data & mask ) return ink;

  return paper;
}

#ifdef DISPLAYTEST

/* Helper functions for the unit tests */
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

void
display_clear_maybe_dirty( void )
{
  memset( display_maybe_dirty, 0, sizeof( display_maybe_dirty ) );
}

void
display_clear_is_dirty( void )
{
  memset( display_is_dirty, 0, sizeof( display_is_dirty ) );
}

void
display_set_maybe_dirty( int y, libspectrum_qword dirty )
{
  display_maybe_dirty[y] = dirty;
}

libspectrum_qword
display_get_is_dirty( int y )
{
  return display_is_dirty[y];
}

libspectrum_dword
display_get_maybe_dirty( int y )
{
  return display_maybe_dirty[y];
}

#endif          /* #ifdef DISPLAYTEST */
