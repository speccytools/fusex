/* display_dirty.c: Dirty-region and critical-beam display tracking
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
#include "fuse.h"
#include "machine.h"
#include "peripherals/scld.h"
#include "rectangle.h"

/* Which eight-pixel chunks on each line (including border) need to
   be redisplayed. Bit 0 corresponds to pixels 0-7, bit 39 to
   pixels 311-319. */
static libspectrum_qword display_is_dirty[ DISPLAY_SCREEN_HEIGHT ];

/* Which eight-pixel chunks on each line may need to be redisplayed. Bit 0
   corresponds to pixels 0-7, bit 31 to pixels 248-255. */
static libspectrum_dword display_maybe_dirty[ DISPLAY_HEIGHT ];

/* This value signifies that the entire line must be redisplayed */
static libspectrum_qword display_all_dirty;

/* Has the palette changed since the start of this frame? */
static int display_palette_changed_this_frame;

/* The last point at which we updated the screen display */
static int critical_region_x = 0, critical_region_y = 0;

/* Cache the most-recently computed beam position keyed on tstates.
   get_beam_position() is called once per dirty screen write, and attribute
   writes trigger eight consecutive calls all at the same tstates value.
   Caching the fully adjusted/clamped screen coordinates avoids the border
   subtraction and clamping branches on each cache hit (~7/8 of calls). */
static libspectrum_dword display_cached_beam_tstates = ( libspectrum_dword ) -
                                                       1;
static int display_cached_screen_x, display_cached_screen_y;

/* Mark as 'dirty' the pixels which have been changed by a write to
   'offset' within the RAM page containing the screen */
void
display_dirty_timex( libspectrum_word offset )
{
  switch( scld_last_dec.mask.scrnmode ) {

  case STANDARD:   /* standard Speccy screen */
  case HIRESATTR:   /* strange mode */
    if( offset >= DISPLAY_FILE_SIZE ) break;
    if( offset <  DISPLAY_PIXEL_BYTES ) {
      display_dirty8( offset );
    } else {
      display_dirty64( offset );
    }
    break;

  case ALTDFILE:   /* second screen */
  case HIRESATTRALTD:   /* strange mode using second screen */
    if( offset < ALTDFILE_OFFSET ||
        offset >= ALTDFILE_OFFSET + DISPLAY_FILE_SIZE ) break;
    if( offset < ALTDFILE_OFFSET + DISPLAY_PIXEL_BYTES ) {
      display_dirty8( offset - ALTDFILE_OFFSET );
    } else {
      display_dirty64( offset - ALTDFILE_OFFSET );
    }
    break;

  case EXTCOLOUR:   /* extended colours */
  case HIRES:   /* hires mode */
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

void
display_mark_screen_dirty( int x, int y )
{
  display_is_dirty[y] |= ( (libspectrum_qword)1 << x );
}

static void
update_dirty_rects( void )
{
  int start, y;

  for( y = 0; y < DISPLAY_SCREEN_HEIGHT; y++ ) {
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

void
display_get_beam_position( int *x, int *y )
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

/* Work out where the beam is, as a position on the main screen */
static void
update_cached_beam( void )
{
  int beam_x, beam_y;

  if( tstates != display_cached_beam_tstates ) {
    display_get_beam_position( &beam_x, &beam_y );
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
}

void
display_update_critical( int x, int y )
{
  update_cached_beam();

  if(   y <  display_cached_screen_y                              ||
        ( y == display_cached_screen_y && x < display_cached_screen_x ) )
    copy_critical_region( display_cached_screen_x, display_cached_screen_y );
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

    display_update_critical( x, y );
  }

  display_maybe_dirty[y] |= ( (libspectrum_dword)1 << x );
}

void
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

void
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
display_palette_changed( void )
{
  int x, y;

  /* Drawing is deferred until the beam passes a cell, so draw what it has
     passed now, with the old palette. The critical region only moves forward,
     so if the beam is behind it, as it is after a snapshot has set the T-state
     back, there is nothing to draw. */
  update_cached_beam();
  x = display_cached_screen_x;
  y = display_cached_screen_y;

  if( y > critical_region_y || ( y == critical_region_y && x > critical_region_x ) )
    copy_critical_region( x, y );

  /* The beam has not yet drawn the cell it is in, or those after it */
  if( x < DISPLAY_WIDTH_COLS )
    display_maybe_dirty[y] |= (libspectrum_dword)display_all_dirty << x;
  for( y++; y < DISPLAY_HEIGHT; y++ )
    display_maybe_dirty[y] = display_all_dirty;

  /* Cells drawn earlier in this frame used the old palette, so the next frame
     redraws them */
  display_palette_changed_this_frame = 1;
}

void
display_refresh_main_screen( void )
{
  size_t i;

  for( i = 0; i < DISPLAY_HEIGHT; i++ )
    display_maybe_dirty[i] = display_all_dirty;
}

display_dirty_fn display_dirty;

void
display_dirty_init( void )
{
  display_all_dirty = ( (libspectrum_qword)1 <<
                        DISPLAY_SCREEN_WIDTH_COLS ) - 1;
}

void
display_dirty_frame_begin( void )
{
  /* Machine timing may have changed on reset. */
  display_cached_beam_tstates = ( libspectrum_dword ) - 1;
  copy_critical_region( DISPLAY_WIDTH_COLS, DISPLAY_HEIGHT - 1 );
  critical_region_x = critical_region_y = 0;

  /* Every cell may need redrawing with the palette now in force */
  if( display_palette_changed_this_frame ) {
    display_refresh_main_screen();
    display_palette_changed_this_frame = 0;
  }
}

void
display_dirty_frame_end( void )
{
  update_dirty_rects();
}

void
display_dirty_refresh_all( void )
{
  size_t i;

  for( i = 0; i < DISPLAY_SCREEN_HEIGHT; i++ )
    display_is_dirty[i] = display_all_dirty;
}

#ifdef DISPLAYTEST

/* Helper functions for the unit tests */
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

#endif /* #ifdef DISPLAYTEST */
