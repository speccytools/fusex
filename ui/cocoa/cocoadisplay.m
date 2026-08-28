/* cocoadisplay.m: Routines for dealing with the Cocoa display
   Copyright (c) 2006-2007 Fredrick Meunier

   $Id$

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

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <libspectrum.h>

#import "DisplayOpenGLView.h"

#include "cocoadisplay.h"
#include "dirty.h"
#include "display.h"
#include "fuse.h"
#include "machine.h"
#include "scld.h"
#include "screenshot.h"
#include "settings.h"
#include "ui/ui.h"
#include "ui/scaler/scaler.h"
#include "ui/uidisplay.h"
#include "utils.h"

/* The current size of the display (in units of DISPLAY_SCREEN_*) */
static float display_current_size = 1.0f;

static int image_width;
static int image_height;

/* Screen texture */
Cocoa_Texture* screen = NULL;

/* Screen texture in native size */
Cocoa_Texture unscaled_screen;

/* Screen texture after scaling (only if a transforming scaler is in place) */
Cocoa_Texture scaled_screen;

/* Screen texture second buffer */
Cocoa_Texture buffered_screen;

/* and a lock to protect it from concurrent access */
NSLock *buffered_screen_lock = nil;

/* Colours are in 1A 5R 5G 5B format */
static uint16_t colour_values[] = {
  0x0000,
  0x0017,
  0x5c00,
  0x5c17,
  0x02e0,
  0x02f7,
  0x5ee0,
  0x5ef7,
  0x0000,
  0x001f,
  0x7c00,
  0x7c1f,
  0x03e0,
  0x03ff,
  0x7fe0,
  0x7fff
};

static uint16_t bw_values[16];

static int display_updated = 0;
static int window_resize_enabled = 0;

/* This is a rule of thumb for the maximum number of rects that can be updated
   each frame. */
#define MAX_UPDATE_RECT 300

static void
init_scalers( void )
{
  scaler_register_clear();

  scaler_register( SCALER_NORMAL );
  if( machine_current->timex ) {
    scaler_register( SCALER_TIMEXTV );
  } else {
    scaler_register( SCALER_TV2X );
    scaler_register( SCALER_TV3X );
    scaler_register( SCALER_TV4X );
    scaler_register( SCALER_PALTV2X );
    scaler_register( SCALER_PALTV3X );
    scaler_register( SCALER_PALTV4X );
    scaler_register( SCALER_2XSAI );
    scaler_register( SCALER_SUPER2XSAI );
    scaler_register( SCALER_SUPEREAGLE );
    scaler_register( SCALER_ADVMAME2X );
    scaler_register( SCALER_ADVMAME3X );
    scaler_register( SCALER_DOTMATRIX );
    scaler_register( SCALER_HQ2X );
    scaler_register( SCALER_HQ3X );
    scaler_register( SCALER_HQ4X );
    scaler_register( SCALER_NTSC2X );
    scaler_register( SCALER_NTSC3X );
    scaler_register( SCALER_NTSC4X );
  }
  
  if( scaler_is_supported( current_scaler ) ) {
    scaler_activate_scaler( current_scaler );
  } else {
    scaler_activate_scaler( SCALER_NORMAL );
  }

  scaler_select_bitformat( 555 );
}

static int
allocate_screen( Cocoa_Texture* new_screen, int height, int width,
                 float scaling_factor )
{
  new_screen->image_width = width * scaling_factor;
  new_screen->image_height = height * scaling_factor;

  /* Need some extra bytes around when using 2xSaI */
  new_screen->full_width = new_screen->image_width+3;
  new_screen->image_xoffset = 1;
  new_screen->full_height = new_screen->image_height+3;
  new_screen->image_yoffset = 1;

  new_screen->pixels = calloc( new_screen->full_width*new_screen->full_height,
                           sizeof(uint16_t) );
  if( !new_screen->pixels ) {
    fprintf( stderr, "%s: couldn't allocate screen.pixels\n", fuse_progname );
    return 1;
  }

  new_screen->dirty = pig_dirty_open( MAX_UPDATE_RECT );
  if( !new_screen->dirty ) {
    free( new_screen->pixels );
    fprintf( stderr, "%s: couldn't allocate screen.dirty\n", fuse_progname );
    return 1;
  }

  new_screen->pitch = new_screen->full_width * sizeof(uint16_t);

  return 0;
}

static void
free_screen( Cocoa_Texture* screen )
{
  if( screen->pixels ) {
    free( screen->pixels );
    screen->pixels = NULL;
  }
  if( screen->dirty ) {
    pig_dirty_close( screen->dirty );
    screen->dirty = NULL;
  }
}

static int
cocoadisplay_load_gfx_mode( void )
{
  int error;

  display_current_size = scaler_get_scaling_factor( current_scaler );

  error = allocate_screen( &unscaled_screen, image_height, image_width, 1.0f );
  if( error ) return error;

  screen = &unscaled_screen;

  if( current_scaler != SCALER_NORMAL ) {
    error = allocate_screen( &scaled_screen, image_height, image_width,
                             display_current_size );
    if( error ) return error;

    screen = &scaled_screen;
  }

  error = allocate_screen( &buffered_screen, screen->image_height,
                           screen->image_width, 1.0f );
  if( error ) return error;

  /* Destroy any existing OpenGL textures (and their dirty lists) */
  [[DisplayOpenGLView instance] destroyTexture];

  /* Create OpenGL textures for the image in DisplayOpenGLView */
  [[DisplayOpenGLView instance] createTexture:&buffered_screen];

  return 0;
}

static void
cocoadisplay_allocate_colours( int numColours, uint16_t *colour_values,
                               uint16_t *bw_values )
{
  int i;
  uint8_t red, green, blue, grey;

  for( i = 0; i < numColours; i++ ) {
    red = (colour_values[i] >> 10) & 0x1f;
    green = (colour_values[i] >> 5) & 0x1f;
    blue = colour_values[i] & 0x1f;

    /* Addition of 0.5 is to avoid rounding errors */
    grey = ( 0.299 * red + 0.587 * green + 0.114 * blue ) + 0.5;

    bw_values[i] = (grey << 10) | (grey << 5) | grey;
  }
}

/* Size the current scaler asks the window's content area to be. A window that
   would not fit the screen ends up smaller than this. */
static NSSize
cocoadisplay_scaler_content_size( void )
{
  float factor = scaler_get_scaling_factor( current_scaler );

  return NSMakeSize( image_width * factor, image_height * factor );
}

/* Content size most recently requested of the window; main thread only.

   Holds what was requested rather than what AppKit granted: an oversized
   frame comes back shrunk, and asking for that same oversized size again
   still has to count as no change.

   Seeded at the first rendered frame so that the first scaler change of a
   session compares against the scaler in effect rather than against zero,
   which would resize a window the user sized by hand and AppKit restored. */
static NSSize last_resized_content_size = { 0.0, 0.0 };

/* Resize the emulator window after an explicit scaler change to match the
   unscaled framebuffer times the current scaler factor, anchored at the
   window's top-left corner.

   Callers may be on a worker thread (fuse_init runs on the emulator thread
   spawned by -[Emulator connectWithPorts:]) or the main thread
   (Preferences close). NSWindow geometry must be touched on the main
   thread, so dispatch unconditionally to it; the size is captured by the
   block to avoid reading shared state on the wrong thread. */
static void
cocoadisplay_resize_window( void )
{
  /* Startup applies the saved scaler before the first rendered frame; let
     AppKit keep the restored window frame until emulation is visibly running. */
  if( !display_ui_initialised || !window_resize_enabled ) return;

  NSSize size = cocoadisplay_scaler_content_size();

  dispatch_async( dispatch_get_main_queue(), ^{
    NSWindow *win = [[DisplayOpenGLView instance] window];
    /* AppKit's fullscreen Space owns the window frame; application code
       cannot resize a fullscreen window. The pre-dispatch check was
       previously a worker-thread-safe read of a mirrored flag; the
       authoritative state lives in the window's styleMask, which must be
       read on main. */
    if( ( [win styleMask] & NSWindowStyleMaskFullScreen ) != 0 ) return;

    /* Recorded past the fullscreen bail above, so a scaler change made in
       fullscreen does not claim a resize that never happened. */
    if( NSEqualSizes( size, last_resized_content_size ) ) return;
    last_resized_content_size = size;

    NSRect old = [win frame];
    NSRect frame = [win frameRectForContentRect:
                            NSMakeRect( 0, 0, size.width, size.height )];

    /* AppKit's origin is the bottom-left one, so holding the top edge means
       moving the origin down as the window grows. */
    frame.origin.x = old.origin.x;
    frame.origin.y = NSMaxY( old ) - frame.size.height;

    /* constrainFrameRect: only keeps the title bar reachable and would leave a
       widened window off the right edge. Clamp last-write-wins so a window too
       big to fit lands on the top-left. -screen is nil when fully off-screen. */
    NSScreen *win_screen = [win screen] ?: [NSScreen mainScreen];
    if( win_screen ) {
      NSRect visible = [win_screen visibleFrame];

      frame.origin.x = MIN( frame.origin.x, NSMaxX( visible ) - frame.size.width );
      frame.origin.x = MAX( frame.origin.x, NSMinX( visible ) );
      frame.origin.y = MAX( frame.origin.y, NSMinY( visible ) );
      frame.origin.y = MIN( frame.origin.y, NSMaxY( visible ) - frame.size.height );
    }

    /* AppKit trims a frame taller than the visible area, and the 4:3
       contentAspectRatio then pulls the width down to match. */
    [win setFrame:frame display:YES animate:YES];
  });
}

int
uidisplay_init( int width, int height )
{
  cocoadisplay_allocate_colours( sizeof(colour_values) / sizeof(uint16_t),
                                 colour_values, bw_values );

  image_width = width;
  image_height = height;

  init_scalers();

  if ( scaler_activate_scaler( current_scaler ) )
    scaler_activate_scaler( SCALER_NORMAL );

  [buffered_screen_lock lock];
  cocoadisplay_load_gfx_mode();
  [buffered_screen_lock unlock];

  /* We can now output error messages to our output device */
  display_ui_initialised = 1;

  return 0;
}

int
uidisplay_hotswap_gfx_mode( void )
{
  fuse_emulation_pause();

  /* obtain lock for buffered screen */
  [buffered_screen_lock lock];

  /* Preserve last rendered pixel data so we can repopulate the new surfaces
     after reallocation, avoiding a black frame during a scaler-only swap.
     (A swap that follows a machine change still flashes black for one frame
     because uidisplay_init has just zeroed unscaled_screen.pixels and no
     emulator frame has run yet.)
     pixels is NULL on the very first hotswap (before uidisplay_init has run
     load_gfx_mode) and after uidisplay_end, so the NULL check covers
     "no valid frame yet". */
  uint16_t *saved_pixels = NULL;
  size_t saved_pixels_size = 0;
  if( unscaled_screen.pixels ) {
    size_t bytes = (size_t)unscaled_screen.full_width *
                   unscaled_screen.full_height *
                   sizeof(uint16_t);
    saved_pixels = malloc( bytes );
    if( saved_pixels ) {
      memcpy( saved_pixels, unscaled_screen.pixels, bytes );
      saved_pixels_size = bytes;
    }
  }

  /* Free the old surfaces */
  free_screen( &unscaled_screen );
  free_screen( &scaled_screen );
  free_screen( &buffered_screen );

  /* Setup the new GFX mode */
  cocoadisplay_load_gfx_mode();

  /* Repopulate the new surfaces under the lock so the display link sees a
     valid frame the moment the lock is released. Inlines the equivalent of
     uidisplay_area + uidisplay_frame_end without re-acquiring the lock
     (NSLock is non-recursive). Skip the restore if the new surface size
     differs from the saved buffer to avoid a buffer overrun. */
  if( saved_pixels && unscaled_screen.pixels &&
      saved_pixels_size == (size_t)unscaled_screen.full_width *
                           unscaled_screen.full_height *
                           sizeof(uint16_t) ) {
    int i;
    PIG_rect r = { 0, 0, image_width, image_height };

    memcpy( unscaled_screen.pixels, saved_pixels, saved_pixels_size );

    if( current_scaler == SCALER_NORMAL ) {
      pig_dirty_add( unscaled_screen.dirty, &r );
    } else {
      r.w = display_current_size * image_width;
      r.h = display_current_size * image_height;
      pig_dirty_add( scaled_screen.dirty, &r );
      scaler_proc16( unscaled_screen.pixels +
                       unscaled_screen.image_yoffset * unscaled_screen.pitch +
                       sizeof(uint16_t) * unscaled_screen.image_xoffset,
                     unscaled_screen.pitch,
                     scaled_screen.pixels +
                       scaled_screen.image_yoffset * scaled_screen.pitch +
                       sizeof(uint16_t) * scaled_screen.image_xoffset,
                     scaled_screen.pitch, image_width, image_height );
    }

    for( i = 0; i < screen->dirty->count; ++i )
      copy_area( &buffered_screen, screen, screen->dirty->rects + i );
    pig_dirty_merge( buffered_screen.dirty, screen->dirty );

    unscaled_screen.dirty->count = 0;
    if( current_scaler != SCALER_NORMAL ) scaled_screen.dirty->count = 0;
  }

  [buffered_screen_lock unlock];

  free( saved_pixels );

  if( uidisplay_take_next_hotswap_reason() ==
      UIDISPLAY_HOTSWAP_REASON_SCALER_EXPLICIT ) {
    cocoadisplay_resize_window();
  }

  fuse_emulation_unpause();

  return 0;
}

/* Set one pixel in the display */
void
uidisplay_putpixel( int x, int y, int colour )
{
  uint16_t *dest_base, *dest;
  uint16_t *palette_values = settings_current.bw_tv ? bw_values : colour_values;

  uint16_t palette_colour = palette_values[ colour ];

  if( machine_current->timex ) {
    x <<= 1; y <<= 1;
    dest_base = dest = (uint16_t*)( (uint8_t*)unscaled_screen.pixels +
                                    (x+unscaled_screen.image_xoffset) * sizeof(uint16_t) +
                                    (y+unscaled_screen.image_yoffset) * unscaled_screen.pitch );

    *(dest++) = palette_colour;
    *(dest  ) = palette_colour;
    dest = (uint16_t*)( (uint8_t*)dest_base + unscaled_screen.pitch );
    *(dest++) = palette_colour;
    *(dest  ) = palette_colour;
  } else {
    dest = (uint16_t*)( (uint8_t*)unscaled_screen.pixels +
                        (x+unscaled_screen.image_xoffset) * sizeof(uint16_t) +
                        (y+unscaled_screen.image_yoffset) * unscaled_screen.pitch );

    *dest = palette_colour;
  }
}

/* Print the 8 pixels in `data' using ink colour `ink' and paper
   colour `paper' to the screen at ( (8*x) , y ) */
void
uidisplay_plot8( int x, int y, libspectrum_byte data,
	         libspectrum_byte ink, libspectrum_byte paper )
{
  uint16_t *dest;
  uint16_t *palette_values = settings_current.bw_tv ? bw_values : colour_values;

  uint16_t palette_ink = palette_values[ ink ];
  uint16_t palette_paper = palette_values[ paper ];

  if( machine_current->timex ) {
    int i;
    uint16_t *dest_base;

    x <<= 4; y <<= 1;

    dest_base = (uint16_t*)( (uint8_t*)unscaled_screen.pixels +
                             (x+unscaled_screen.image_xoffset) * sizeof(uint16_t) +
                             (y+unscaled_screen.image_yoffset) * unscaled_screen.pitch );

    for( i=0; i<2; i++ ) {
      dest = dest_base;

      *(dest++) = ( data & 0x80 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x80 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x40 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x40 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x20 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x20 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x10 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x10 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x08 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x08 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x04 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x04 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x02 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x02 ) ? palette_ink : palette_paper;
      *(dest++) = ( data & 0x01 ) ? palette_ink : palette_paper;
      *dest     = ( data & 0x01 ) ? palette_ink : palette_paper;

      dest_base = (uint16_t*)( (uint8_t*)dest_base + unscaled_screen.pitch );
    }
  } else {
    x <<= 3;
    dest = (uint16_t*)( (uint8_t*)unscaled_screen.pixels +
                        (x+unscaled_screen.image_xoffset) * sizeof(uint16_t) +
                        (y+unscaled_screen.image_yoffset) * unscaled_screen.pitch );

    *(dest++) = ( data & 0x80 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x40 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x20 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x10 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x08 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x04 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x02 ) ? palette_ink : palette_paper;
    *dest     = ( data & 0x01 ) ? palette_ink : palette_paper;
  }
}

/* Print the 16 pixels in `data' using ink colour `ink' and paper
   colour `paper' to the screen at ( (16*x) , y ) */
void
uidisplay_plot16( int x, int y, libspectrum_word data,
		  libspectrum_byte ink, libspectrum_byte paper )
{
  uint16_t *dest_base, *dest;
  int i; 
  uint16_t *palette_values = settings_current.bw_tv ? bw_values : colour_values;
  uint16_t palette_ink = palette_values[ ink ];
  uint16_t palette_paper = palette_values[ paper ];
  x <<= 4; y <<= 1;

  dest_base = (uint16_t*)( (uint8_t*)unscaled_screen.pixels + (x+unscaled_screen.image_xoffset) *
                           sizeof(uint16_t) + (y+unscaled_screen.image_yoffset) *
                           unscaled_screen.pitch );

  for( i=0; i<2; i++ ) {
    dest = dest_base;

    *(dest++) = ( data & 0x8000 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x4000 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x2000 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x1000 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0800 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0400 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0200 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0100 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0080 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0040 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0020 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0010 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0008 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0004 ) ? palette_ink : palette_paper;
    *(dest++) = ( data & 0x0002 ) ? palette_ink : palette_paper;
    *dest     = ( data & 0x0001 ) ? palette_ink : palette_paper;

    dest_base = (uint16_t*)( (uint8_t*)dest_base + unscaled_screen.pitch );
  }
}

void
copy_area( Cocoa_Texture *dest_screen, Cocoa_Texture *src_screen, PIG_rect *r )
{
  int y;

  for( y = r->y; y <= r->y + r->h; y++ ) {
    int src_offset = (y + src_screen->image_yoffset) * src_screen->pitch +
                     sizeof(uint16_t) * ( r->x + src_screen->image_xoffset);
    int dest_offset = (y + dest_screen->image_yoffset) * dest_screen->pitch +
                      sizeof(uint16_t) * ( r->x + dest_screen->image_xoffset);
    memcpy( dest_screen->pixels + dest_offset, src_screen->pixels + src_offset,
            r->w * sizeof(uint16_t) );
  }
}

void
uidisplay_frame_end( void )
{
  int i;

  if( !window_resize_enabled ) {
    /* Adopt the scaler already in effect as the baseline for the session.
       Dispatched because last_resized_content_size belongs to the main
       thread and this runs on the emulator one. */
    NSSize size = cocoadisplay_scaler_content_size();

    dispatch_async( dispatch_get_main_queue(), ^{
      last_resized_content_size = size;
    });

    window_resize_enabled = 1;
  }

  if( display_updated ) {
    /* obtain lock for buffered screen */
    [buffered_screen_lock lock];

    /* copy screen data to buffered screen */
    for(i = 0; i < screen->dirty->count; ++i)
      copy_area( &buffered_screen, screen, screen->dirty->rects + i );

    pig_dirty_merge( buffered_screen.dirty, screen->dirty );

    /* release lock for buffered screen */
    [buffered_screen_lock unlock];

    display_updated = 0;
    unscaled_screen.dirty->count = 0;
    if( current_scaler != SCALER_NORMAL ) scaled_screen.dirty->count = 0;
  }
}

void
uidisplay_area( int x, int y, int width, int height )
{
  PIG_rect r = { x, y, width, height };

  display_updated = 1;

  if( current_scaler == SCALER_NORMAL ) {
    pig_dirty_add( unscaled_screen.dirty, &r );
    return;
  }

  /* Extend the dirty region by 1 pixel for scalers that "smear" the screen,
     e.g. 2xSAI */
  if( scaler_flags & SCALER_FLAGS_EXPAND )
    scaler_expander( &x, &y, &width, &height, image_width, image_height );

  r.x = display_current_size * x;
  r.y = display_current_size * y;
  r.w = display_current_size * width;
  r.h = display_current_size * height;
  pig_dirty_add( scaled_screen.dirty, &r );

  /* Create scaled image */
  scaler_proc16( unscaled_screen.pixels + ( y + unscaled_screen.image_yoffset ) *
                   unscaled_screen.pitch + sizeof(uint16_t) *
                   ( x + unscaled_screen.image_xoffset ),
                 unscaled_screen.pitch,
                 scaled_screen.pixels + ( r.y + scaled_screen.image_yoffset ) *
                   scaled_screen.pitch + sizeof(uint16_t) *
                   ( r.x + scaled_screen.image_xoffset ),
                 scaled_screen.pitch, width, height );
}

int
uidisplay_end( void )
{
  [buffered_screen_lock lock];

  if( screen && screen->pixels ) {
    [[DisplayOpenGLView instance] destroyTexture];
  }

  [buffered_screen_lock unlock];

  free_screen( &unscaled_screen );
  free_screen( &scaled_screen );
  free_screen( &buffered_screen );

  return 0;
}
