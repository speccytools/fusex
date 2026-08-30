/* sdl2_display.c: SDL 2 display handling
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "config.h"

#include <limits.h>
#include <stdlib.h>
#include <SDL.h>

#include "display.h"
#include "fuse.h"
#include "machine.h"
#include "settings.h"
#include "ui/display_timing.h"
#include "ui/ui.h"
#include "ui/scaler/scaler.h"
#include "ui/uidisplay.h"
#include "utils.h"
#include "sdl2_display_internal.h"
#include "sdl2_scaler_state.h"
#include "sdl2_display.h"
#include "sdl2_ui.h"

static SDL_Window *sdl2_window;
static SDL_Renderer *sdl2_renderer;
static SDL_Texture *sdl2_texture;
static SDL_Surface *tmp_screen;
static SDL_Surface *scaled_screen;
static SDL_Surface *saved;
static libspectrum_byte sdl2display_is_full_screen;
static int fullscreen_x_off;
static int fullscreen_y_off;
static int texture_width;
static int texture_height;

static SDL_Surface *red_cassette[ 2 ], *green_cassette[ 2 ];
static SDL_Surface *red_mdr[ 2 ], *green_mdr[ 2 ];
static SDL_Surface *red_disk[ 2 ], *green_disk[ 2 ];

static ui_statusbar_state sdl2_disk_state, sdl2_mdr_state, sdl2_tape_state;
static int sdl2_status_updated;

static Uint32 colour_values[ 16 ];
static Uint32 bw_values[ 16 ];

static SDL_Rect updated_rects[ 300 ];
static int num_rects;
static libspectrum_byte sdl2display_force_full_refresh = 1;

/* Scaler input areas accumulated for the current emulated frame. */
struct sdl2display_dirty_area {
  int x, y, width, height;
};
static struct sdl2display_dirty_area dirty_areas[ DISPLAY_SCREEN_HEIGHT ];
static size_t dirty_area_count = 0;

static int image_width;
static int image_height;
static int tmp_screen_width;
static float sdl2display_current_size = 1.0f;
static sdl2_scaler_state scaler_state;

static SDL_Color colour_palette[] = {
  {   0,   0,   0,   0 },
  {   0,   0, 192,   0 },
  { 192,   0,   0,   0 },
  { 192,   0, 192,   0 },
  {   0, 192,   0,   0 },
  {   0, 192, 192,   0 },
  { 192, 192,   0,   0 },
  { 192, 192, 192,   0 },
  {   0,   0,   0,   0 },
  {   0,   0, 255,   0 },
  { 255,   0,   0,   0 },
  { 255,   0, 255,   0 },
  {   0, 255,   0,   0 },
  {   0, 255, 255,   0 },
  { 255, 255,   0,   0 },
  { 255, 255, 255,   0 }
};

static void
init_scalers( void )
{
  scaler_register_clear();

  scaler_register( SCALER_NORMAL );
  scaler_register( SCALER_2XSAI );
  scaler_register( SCALER_SUPER2XSAI );
  scaler_register( SCALER_SUPEREAGLE );
  scaler_register( SCALER_ADVMAME2X );
  scaler_register( SCALER_ADVMAME3X );
  scaler_register( SCALER_DOTMATRIX );
  scaler_register( SCALER_HQ2X );
  if( machine_current->timex ) {
    scaler_register( SCALER_HALF );
    scaler_register( SCALER_HALFSKIP );
    scaler_register( SCALER_TIMEXTV );
    scaler_register( SCALER_TIMEX1_5X );
    scaler_register( SCALER_TIMEX2X );
  } else {
    scaler_register( SCALER_DOUBLESIZE );
    scaler_register( SCALER_TRIPLESIZE );
    scaler_register( SCALER_QUADSIZE );
    scaler_register( SCALER_TV2X );
    scaler_register( SCALER_TV3X );
    scaler_register( SCALER_TV4X );
    scaler_register( SCALER_PALTV2X );
    scaler_register( SCALER_PALTV3X );
    scaler_register( SCALER_PALTV4X );
    scaler_register( SCALER_HQ3X );
    scaler_register( SCALER_HQ4X );
    scaler_register( SCALER_NTSC2X );
    scaler_register( SCALER_NTSC3X );
    scaler_register( SCALER_NTSC4X );
  }

  if( scaler_activate_scaler( current_scaler ) )
    scaler_activate_scaler( SCALER_NORMAL );
}

static void
sdl2display_allocate_colours( void )
{
  int i;

  for( i = 0; i < 16; i++ ) {
    Uint8 red = colour_palette[i].r;
    Uint8 green = colour_palette[i].g;
    Uint8 blue = colour_palette[i].b;
    Uint8 grey = ( 0.299 * red + 0.587 * green + 0.114 * blue ) + 0.5;

    colour_values[i] = SDL_MapRGB( tmp_screen->format, red, green, blue );
    bw_values[i] = SDL_MapRGB( tmp_screen->format, grey, grey, grey );
  }
}

static int
sdl2display_convert_icon( SDL_Surface *source, SDL_Surface **icon, int red )
{
  SDL_Surface *copy;
  int i;
  SDL_Color colors[ source->format->palette->ncolors ];

  copy = SDL_ConvertSurface( source, source->format, 0 );
  if( !copy ) return -1;

  for( i = 0; i < copy->format->palette->ncolors; i++ ) {
    colors[i].r = red ? copy->format->palette->colors[i].r : 0;
    colors[i].g = red ? 0 : copy->format->palette->colors[i].g;
    colors[i].b = 0;
    colors[i].a = SDL_ALPHA_OPAQUE;
  }

  if( SDL_SetPaletteColors( copy->format->palette, colors, 0, i ) ) {
    SDL_FreeSurface( copy );
    return -1;
  }

  icon[0] = SDL_ConvertSurface( copy, tmp_screen->format, 0 );
  SDL_FreeSurface( copy );
  if( !icon[0] ) return -1;

  icon[1] = SDL_CreateRGBSurface( 0,
                                  icon[0]->w << 1, icon[0]->h << 1,
                                  icon[0]->format->BitsPerPixel,
                                  icon[0]->format->Rmask,
                                  icon[0]->format->Gmask,
                                  icon[0]->format->Bmask,
                                  icon[0]->format->Amask );
  if( !icon[1] ) return -1;

  ( scaler_get_proc16( SCALER_DOUBLESIZE ) )(
    (libspectrum_byte *)icon[0]->pixels,
    icon[0]->pitch,
    (libspectrum_byte *)icon[1]->pixels,
    icon[1]->pitch,
    icon[0]->w,
    icon[0]->h
    );

  return 0;
}

static int
sdl2display_load_status_icon( const char *filename,
                              SDL_Surface **red,
                              SDL_Surface **green )
{
  char path[ PATH_MAX ];
  SDL_Surface *temp;

  if( utils_find_file_path( filename, path, UTILS_AUXILIARY_LIB ) ) {
    fprintf( stderr, "%s: Error getting path for icons\n", fuse_progname );
    return -1;
  }

  temp = SDL_LoadBMP( path );
  if( !temp ) {
    fprintf( stderr, "%s: Error loading icon \"%s\" text:%s\n", fuse_progname,
             path, SDL_GetError() );
    return -1;
  }

  if( !temp->format->palette ) {
    fprintf( stderr, "%s: Icon \"%s\" is not paletted\n", fuse_progname, path );
    SDL_FreeSurface( temp );
    return -1;
  }

  if( sdl2display_convert_icon( temp, red, 1 ) ||
      sdl2display_convert_icon( temp, green, 0 ) ) {
    SDL_FreeSurface( temp );
    return -1;
  }

  SDL_FreeSurface( temp );
  return 0;
}

static void
sdl2display_free_status_icons( void )
{
  int i;

  for( i = 0; i < 2; i++ ) {
    if( red_cassette[i] ) {
      SDL_FreeSurface( red_cassette[i] ); red_cassette[i] = NULL;
    }
    if( green_cassette[i] ) {
      SDL_FreeSurface( green_cassette[i] ); green_cassette[i] = NULL;
    }
    if( red_mdr[i] ) {
      SDL_FreeSurface( red_mdr[i] ); red_mdr[i] = NULL;
    }
    if( green_mdr[i] ) {
      SDL_FreeSurface( green_mdr[i] ); green_mdr[i] = NULL;
    }
    if( red_disk[i] ) {
      SDL_FreeSurface( red_disk[i] ); red_disk[i] = NULL;
    }
    if( green_disk[i] ) {
      SDL_FreeSurface( green_disk[i] ); green_disk[i] = NULL;
    }
  }
}

static void
merge_dirty_areas( struct sdl2display_dirty_area *destination,
                   const struct sdl2display_dirty_area *source )
{
  int right = destination->x + destination->width;
  int bottom = destination->y + destination->height;
  int source_right = source->x + source->width;
  int source_bottom = source->y + source->height;

  if( source->x < destination->x ) destination->x = source->x;
  if( source->y < destination->y ) destination->y = source->y;
  if( source_right > right ) right = source_right;
  if( source_bottom > bottom ) bottom = source_bottom;

  destination->width = right - destination->x;
  destination->height = bottom - destination->y;
}

static void
sdl2display_add_dirty_area( int x, int y, int width, int height )
{
  struct sdl2display_dirty_area area;
  size_t i;

  if( sdl2display_force_full_refresh ) return;

  area.x = x; area.y = y; area.width = width; area.height = height;

  /* Merge touching regions too. PAL TV expands every region to the full
     scanline, so this prevents the same expanded line being scaled twice. */
  for( i = 0; i < dirty_area_count; ) {
    struct sdl2display_dirty_area *old = &dirty_areas[i];

    if( area.x <= old->x + old->width && old->x <= area.x + area.width &&
        area.y <= old->y + old->height && old->y <= area.y + area.height ) {
      merge_dirty_areas( &area, old );
      dirty_areas[i] = dirty_areas[ --dirty_area_count ];
    } else {
      i++;
    }
  }

  if( dirty_area_count == DISPLAY_SCREEN_HEIGHT ) {
    for( i = 0; i < dirty_area_count; i++ )
      merge_dirty_areas( &area, &dirty_areas[i] );
    dirty_area_count = 0;
  }

  dirty_areas[ dirty_area_count++ ] = area;
}

static void
sdl2display_queue_status_rects( void )
{
  int scale = machine_current->timex ? 2 : 1;

  if( red_disk[ machine_current->timex ] ) {
    sdl2display_add_dirty_area( 243 * scale, 218 * scale,
                                red_disk[ machine_current->timex ]->w,
                                red_disk[ machine_current->timex ]->h );
  }
  if( red_mdr[ machine_current->timex ] ) {
    sdl2display_add_dirty_area( 264 * scale, 218 * scale,
                                red_mdr[ machine_current->timex ]->w,
                                red_mdr[ machine_current->timex ]->h );
  }
  if( red_cassette[ machine_current->timex ] ) {
    sdl2display_add_dirty_area( 285 * scale, 220 * scale,
                                red_cassette[ machine_current->timex ]->w,
                                red_cassette[ machine_current->timex ]->h );
  }
}

static void
sdl2display_status_icon( SDL_Surface **icon, int x, int y )
{
  SDL_Rect dst_rect;
  sdl2_display_rect rect;
  SDL_Surface *surface = icon[ machine_current->timex ];

  if( !surface ) return;

  if( machine_current->timex ) {
    x <<= 1;
    y <<= 1;
  }

  sdl2display_icon_rect( x, y, surface->w, surface->h,
                         sdl2display_current_size,
                         fullscreen_x_off, fullscreen_y_off, &rect );
  dst_rect.x = rect.x;
  dst_rect.y = rect.y;
  dst_rect.w = rect.w;
  dst_rect.h = rect.h;

  SDL_BlitScaled( surface, NULL, scaled_screen, &dst_rect );
}

static void
sdl2display_icon_overlay( void )
{
  if( !settings_current.statusbar ) {
    sdl2_status_updated = 0;
    return;
  }

  switch( sdl2_disk_state ) {
  case UI_STATUSBAR_STATE_ACTIVE:
    sdl2display_status_icon( green_disk, 243, 218 );
    break;
  case UI_STATUSBAR_STATE_INACTIVE:
    sdl2display_status_icon( red_disk, 243, 218 );
    break;
  case UI_STATUSBAR_STATE_NOT_AVAILABLE:
    break;
  }

  switch( sdl2_mdr_state ) {
  case UI_STATUSBAR_STATE_ACTIVE:
    sdl2display_status_icon( green_mdr, 264, 218 );
    break;
  case UI_STATUSBAR_STATE_INACTIVE:
    sdl2display_status_icon( red_mdr, 264, 218 );
    break;
  case UI_STATUSBAR_STATE_NOT_AVAILABLE:
    break;
  }

  switch( sdl2_tape_state ) {
  case UI_STATUSBAR_STATE_ACTIVE:
    sdl2display_status_icon( green_cassette, 285, 220 );
    break;
  case UI_STATUSBAR_STATE_INACTIVE:
  case UI_STATUSBAR_STATE_NOT_AVAILABLE:
    sdl2display_status_icon( red_cassette, 285, 220 );
    break;
  }

  sdl2_status_updated = 0;
}

static void
sdl2display_create_tmp_screen( void )
{
  Uint16 *tmp_screen_pixels;

  if( tmp_screen ) {
    libspectrum_free( tmp_screen->pixels );
    SDL_FreeSurface( tmp_screen );
    tmp_screen = NULL;
  }

  tmp_screen_width = image_width + 3;
  tmp_screen_pixels = libspectrum_new0( Uint16,
                                        tmp_screen_width * ( image_height + 3 ) );
  if( !tmp_screen_pixels ) {
    fprintf( stderr, "%s: couldn't allocate SDL2 tmp_screen\n", fuse_progname );
    fuse_abort();
  }

  tmp_screen = SDL_CreateRGBSurfaceFrom( tmp_screen_pixels,
                                         tmp_screen_width,
                                         image_height + 3,
                                         16,
                                         tmp_screen_width * 2,
                                         0xf800,
                                         0x07e0,
                                         0x001f,
                                         0x0000 );
  if( !tmp_screen ) {
    fprintf( stderr, "%s: couldn't create SDL2 tmp_screen: %s\n",
             fuse_progname, SDL_GetError() );
    fuse_abort();
  }

  scaler_select_bitformat( 565 );
  sdl2display_allocate_colours();
}

static void
sdl2display_create_scaled_screen( int width, int height )
{
  if( scaled_screen ) {
    SDL_FreeSurface( scaled_screen );
    scaled_screen = NULL;
  }

  scaled_screen = SDL_CreateRGBSurface( 0, width, height, 16,
                                        0xf800, 0x07e0, 0x001f, 0x0000 );
  if( !scaled_screen ) {
    fprintf( stderr, "%s: couldn't create SDL2 scaled screen: %s\n",
             fuse_progname, SDL_GetError() );
    fuse_abort();
  }
}

static void
sdl2display_sync_presentation_surfaces( void )
{
  int width = image_width * sdl2display_current_size;
  int height = image_height * sdl2display_current_size;

  if( !scaled_screen || scaled_screen->w != width ||
      scaled_screen->h != height ) {
    sdl2display_create_scaled_screen( width, height );
    sdl2display_force_full_refresh = 1;
  }

  if( !sdl2_texture || texture_width != width || texture_height != height ) {
    if( sdl2_texture ) SDL_DestroyTexture( sdl2_texture );

    /* Match sdl12-compat/SDL1-style fullscreen scaling: desktop fullscreen
       still stretches the fixed Fuse software-scaled texture to the output,
       but linear filtering avoids uneven nearest-neighbour pixel runs on
       displays where that final stretch is not an integer scale. */
    SDL_SetHint( SDL_HINT_RENDER_SCALE_QUALITY, "1" );

    sdl2_texture = SDL_CreateTexture( sdl2_renderer,
                                      SDL_PIXELFORMAT_RGB565,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      width, height );
    if( !sdl2_texture ) {
      fprintf( stderr, "%s: couldn't create SDL2 texture: %s\n",
               fuse_progname, SDL_GetError() );
      fuse_abort();
    }

    texture_width = width;
    texture_height = height;
    sdl2display_force_full_refresh = 1;
  }

  SDL_RenderSetLogicalSize( sdl2_renderer, width, height );

  fullscreen_x_off = 0;
  fullscreen_y_off = 0;
}

static void
sdl2display_get_scaler_env( sdl2_scaler_env *env,
                            unsigned char *supported, float *scales )
{
  SDL_DisplayMode mode;
  scaler_type scaler;

  env->image_width = image_width;
  env->image_height = image_height;
  env->display_width = 0;
  env->display_height = 0;
  env->supported = supported;
  env->scales = scales;
  env->scaler_count = SCALER_NUM;

  if( sdl2_renderer )
    SDL_GetRendererOutputSize( sdl2_renderer, &env->display_width,
                               &env->display_height );

  if( ( !env->display_width || !env->display_height ) &&
      !SDL_GetDesktopDisplayMode( 0, &mode ) ) {
    env->display_width = mode.w;
    env->display_height = mode.h;
  }

  for( scaler = 0; scaler < SCALER_NUM; scaler++ ) {
    supported[ scaler ] = scaler_is_supported( scaler );
    scales[ scaler ] = scaler_get_scaling_factor( scaler );
  }
}

static void
sdl2display_apply_scaler_decision( const sdl2_scaler_decision *decision )
{
  scaler_activate_scaler( decision->scaler );
  scaler_state = decision->next;
  sdl2display_current_size = scaler_get_scaling_factor( current_scaler );
}

static void
sdl2display_update_fullscreen_scaler( int startup,
                                      int explicit_scaler_change )
{
  sdl2_scaler_env env;
  sdl2_scaler_decision decision;
  unsigned char supported[ SCALER_NUM ];
  float scales[ SCALER_NUM ];

  init_scalers();
  sdl2display_get_scaler_env( &env, supported, scales );

  decision = sdl2_scaler_state_transition( &scaler_state, current_scaler,
                                           settings_current.full_screen,
                                           sdl2display_is_full_screen,
                                           startup, explicit_scaler_change,
                                           &env );

  sdl2display_apply_scaler_decision( &decision );
}

static void
sdl2display_create_window( int startup, int explicit_scaler_change )
{
  int width;
  int height;

  if( sdl2_texture ) {
    SDL_DestroyTexture( sdl2_texture );
    sdl2_texture = NULL;
    texture_width = texture_height = 0;
  }

  if( sdl2_renderer ) {
    SDL_DestroyRenderer( sdl2_renderer );
    sdl2_renderer = NULL;
  }

  sdl2display_current_size = scaler_get_scaling_factor( current_scaler );
  sdl2display_update_fullscreen_scaler( startup, explicit_scaler_change );
  width = image_width * sdl2display_current_size;
  height = image_height * sdl2display_current_size;

  if( !sdl2_window ) {
    Uint32 flags = SDL_WINDOW_HIDDEN;

    if( settings_current.full_screen ) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    sdl2_window = SDL_CreateWindow( "Fuse",
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    width,
                                    height,
                                    flags );
    if( !sdl2_window ) {
      fprintf( stderr, "%s: couldn't create SDL2 window: %s\n",
               fuse_progname, SDL_GetError() );
      fuse_abort();
    }
  } else {
    if( sdl2display_is_full_screen != settings_current.full_screen ) {
      Uint32 flags = settings_current.full_screen ?
                     SDL_WINDOW_FULLSCREEN_DESKTOP : 0;

      if( settings_current.full_screen ) SDL_SetWindowSize( sdl2_window, width,
                                                            height );

      if( SDL_SetWindowFullscreen( sdl2_window, flags ) ) {
        fprintf( stderr, "%s: couldn't set SDL2 fullscreen mode: %s\n",
                 fuse_progname, SDL_GetError() );
        fuse_abort();
      }

      if( !settings_current.full_screen ) SDL_SetWindowSize( sdl2_window,
                                                             width, height );
    } else {
      SDL_SetWindowSize( sdl2_window, width, height );
    }
  }

  sdl2display_is_full_screen = settings_current.full_screen;

  sdl2_renderer = SDL_CreateRenderer( sdl2_window, -1,
                                      SDL_RENDERER_ACCELERATED );
  if( !sdl2_renderer )
    sdl2_renderer = SDL_CreateRenderer( sdl2_window, -1, 0 );
  if( !sdl2_renderer ) {
    fprintf( stderr, "%s: couldn't create SDL2 renderer: %s\n",
             fuse_progname, SDL_GetError() );
    fuse_abort();
  }

  SDL_SetRenderDrawColor( sdl2_renderer, 0, 0, 0, 255 );

  sdl2display_update_fullscreen_scaler( 0, 0 );
  sdl2display_sync_presentation_surfaces();
  SDL_ShowWindow( sdl2_window );
}

static void
sdl2display_recreate( int startup, int explicit_scaler_change )
{
  sdl2display_force_full_refresh = 1;
  sdl2display_create_window( startup, explicit_scaler_change );
  sdl2display_create_tmp_screen();
  sdl2display_free_status_icons();
  sdl2display_load_status_icon( "cassette.bmp", red_cassette, green_cassette );
  sdl2display_load_status_icon( "microdrive.bmp", red_mdr, green_mdr );
  sdl2display_load_status_icon( "plus3disk.bmp", red_disk, green_disk );

  /* Reapply mouse grab state after recreating the SDL window. */
  if( ui_mouse_grabbed ) ui_mouse_grabbed = ui_mouse_grab( 0 );

  /* Match SDL1: fullscreen hides the host cursor even without a mouse grab. */
  sdl2ui_set_cursor_visibility( !( settings_current.full_screen ||
                                   ui_mouse_grabbed ) );

  display_refresh_all();
}

SDL_Window *
sdl2display_get_window( void )
{
  return sdl2_window;
}

void
sdl2display_set_title( const char *title )
{
  if( sdl2_window ) SDL_SetWindowTitle( sdl2_window, title );
}

int
uidisplay_init( int width, int height )
{
  image_width = width;
  image_height = height;

  display_timing_init( "sdl2" );

  init_scalers();
  sdl2_scaler_state_init( &scaler_state );

  if( scaler_activate_scaler( current_scaler ) )
    scaler_activate_scaler( SCALER_NORMAL );

  sdl2display_is_full_screen = settings_current.full_screen;
  sdl2display_recreate( 1, 0 );
  display_ui_initialised = 1;

  return 0;
}

int
uidisplay_hotswap_gfx_mode( void )
{
  int explicit_scaler_change;

  explicit_scaler_change =
    uidisplay_take_next_hotswap_reason() ==
    UIDISPLAY_HOTSWAP_REASON_SCALER_EXPLICIT;

  sdl2display_recreate( 0, explicit_scaler_change );
  return 0;
}

void
uidisplay_frame_save( void )
{
  if( saved ) {
    SDL_FreeSurface( saved );
    saved = NULL;
  }

  saved = SDL_ConvertSurface( tmp_screen, tmp_screen->format, 0 );
}

void
uidisplay_frame_restore( void )
{
  if( saved ) {
    SDL_BlitSurface( saved, NULL, tmp_screen, NULL );
    sdl2display_force_full_refresh = 1;
  }
}

void
uidisplay_putpixel( int x, int y, int colour )
{
  libspectrum_word *dest_base, *dest;
  Uint32 *palette_values = settings_current.bw_tv ? bw_values : colour_values;
  Uint32 palette_colour = palette_values[ colour ];

  if( machine_current->timex ) {
    x <<= 1;
    y <<= 1;
    dest_base = dest =
      (libspectrum_word *)( (libspectrum_byte *)tmp_screen->pixels +
                            ( x + 1 ) * tmp_screen->format->BytesPerPixel +
                            ( y + 1 ) * tmp_screen->pitch );

    *( dest++ ) = palette_colour;
    *( dest++ ) = palette_colour;
    dest = (libspectrum_word *)( (libspectrum_byte *)dest_base +
                                 tmp_screen->pitch );
    *( dest++ ) = palette_colour;
    *( dest++ ) = palette_colour;
  } else {
    dest =
      (libspectrum_word *)( (libspectrum_byte *)tmp_screen->pixels +
                            ( x + 1 ) * tmp_screen->format->BytesPerPixel +
                            ( y + 1 ) * tmp_screen->pitch );
    *dest = palette_colour;
  }
}

void
uidisplay_plot8( int x, int y, libspectrum_byte data,
                 libspectrum_byte ink, libspectrum_byte paper )
{
  libspectrum_word *dest;
  Uint32 *palette_values = settings_current.bw_tv ? bw_values : colour_values;
  Uint32 palette_ink = palette_values[ ink ];
  Uint32 palette_paper = palette_values[ paper ];

  if( machine_current->timex ) {
    int i;
    libspectrum_word *dest_base;

    x <<= 4;
    y <<= 1;

    dest_base =
      (libspectrum_word *)( (libspectrum_byte *)tmp_screen->pixels +
                            ( x + 1 ) * tmp_screen->format->BytesPerPixel +
                            ( y + 1 ) * tmp_screen->pitch );

    for( i = 0; i < 2; i++ ) {
      dest = dest_base;

      *( dest++ ) = ( data & 0x80 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x80 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x40 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x40 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x20 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x20 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x10 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x10 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x08 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x08 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x04 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x04 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x02 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x02 ) ? palette_ink : palette_paper;
      *( dest++ ) = ( data & 0x01 ) ? palette_ink : palette_paper;
      *dest = ( data & 0x01 ) ? palette_ink : palette_paper;

      dest_base = (libspectrum_word *)( (libspectrum_byte *)dest_base +
                                        tmp_screen->pitch );
    }
  } else {
    x <<= 3;

    dest =
      (libspectrum_word *)( (libspectrum_byte *)tmp_screen->pixels +
                            ( x + 1 ) * tmp_screen->format->BytesPerPixel +
                            ( y + 1 ) * tmp_screen->pitch );

    *( dest++ ) = ( data & 0x80 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x40 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x20 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x10 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x08 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x04 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x02 ) ? palette_ink : palette_paper;
    *dest = ( data & 0x01 ) ? palette_ink : palette_paper;
  }
}

void
uidisplay_plot16( int x, int y, libspectrum_word data,
                  libspectrum_byte ink, libspectrum_byte paper )
{
  libspectrum_word *dest_base, *dest;
  int i;
  Uint32 *palette_values = settings_current.bw_tv ? bw_values : colour_values;
  Uint32 palette_ink = palette_values[ ink ];
  Uint32 palette_paper = palette_values[ paper ];

  x <<= 4;
  y <<= 1;
  dest_base =
    (libspectrum_word *)( (libspectrum_byte *)tmp_screen->pixels +
                          ( x + 1 ) * tmp_screen->format->BytesPerPixel +
                          ( y + 1 ) * tmp_screen->pitch );

  for( i = 0; i < 2; i++ ) {
    dest = dest_base;

    *( dest++ ) = ( data & 0x8000 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x4000 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x2000 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x1000 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0800 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0400 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0200 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0100 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0080 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0040 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0020 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0010 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0008 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0004 ) ? palette_ink : palette_paper;
    *( dest++ ) = ( data & 0x0002 ) ? palette_ink : palette_paper;
    *dest = ( data & 0x0001 ) ? palette_ink : palette_paper;

    dest_base = (libspectrum_word *)( (libspectrum_byte *)dest_base +
                                      tmp_screen->pitch );
  }
}

void
uidisplay_area( int x, int y, int width, int height )
{
  if( sdl2display_force_full_refresh ) return;

  display_timing_area( width, height );

  if( scaler_flags & SCALER_FLAGS_EXPAND )
    scaler_expander( &x, &y, &width, &height, image_width, image_height );

  sdl2display_add_dirty_area( x, y, width, height );
}

void
uidisplay_frame_end( void )
{
  int i;
  int full_refresh;

  if( !sdl2_window || !scaled_screen || !sdl2_renderer ) return;

  if( sdl2display_is_full_screen != settings_current.full_screen ) {
    if( uidisplay_hotswap_gfx_mode() ) {
      fprintf( stderr, "%s: Error switching to fullscreen\n", fuse_progname );
      fuse_abort();
    }
  }

  full_refresh = sdl2display_force_full_refresh ||
                 ( scaler_flags & SCALER_FLAGS_FULL_REFRESH );

  if( sdl2_status_updated ) sdl2display_queue_status_rects();

  if( full_refresh ) dirty_area_count = 0;

  if( !( ui_widget_level >= 0 ) && dirty_area_count == 0 && !full_refresh )
    return;

  sdl2display_sync_presentation_surfaces();

  if( SDL_MUSTLOCK( scaled_screen ) ) SDL_LockSurface( scaled_screen );

  if( full_refresh ) SDL_FillRect( scaled_screen, NULL, 0 );

  num_rects = 0;

  display_timing_scaler_begin();
  if( full_refresh ) {
    scaler_proc16(
      (libspectrum_byte *)tmp_screen->pixels +
      1 * tmp_screen->format->BytesPerPixel +
      1 * tmp_screen->pitch,
      tmp_screen->pitch,
      (libspectrum_byte *)scaled_screen->pixels +
      fullscreen_x_off * scaled_screen->format->BytesPerPixel +
      fullscreen_y_off * scaled_screen->pitch,
      scaled_screen->pitch,
      image_width,
      image_height );
  } else {
    for( i = 0; i < (int)dirty_area_count; i++ ) {
      const struct sdl2display_dirty_area *area = &dirty_areas[i];
      sdl2_display_rect dst;

      scaler_proc16(
        (libspectrum_byte *)tmp_screen->pixels +
        ( area->x + 1 ) * tmp_screen->format->BytesPerPixel +
        ( area->y + 1 ) * tmp_screen->pitch,
        tmp_screen->pitch,
        (libspectrum_byte *)scaled_screen->pixels +
        ( fullscreen_x_off + (int)( area->x * sdl2display_current_size ) ) *
        scaled_screen->format->BytesPerPixel +
        ( fullscreen_y_off + (int)( area->y * sdl2display_current_size ) ) *
        scaled_screen->pitch,
        scaled_screen->pitch,
        area->width,
        area->height );

      sdl2display_update_rect( area->x, area->y, area->width, area->height,
                               sdl2display_current_size,
                               fullscreen_x_off, fullscreen_y_off, &dst );

      if( num_rects < (int)ARRAY_SIZE( updated_rects ) ) {
        updated_rects[num_rects].x = dst.x;
        updated_rects[num_rects].y = dst.y;
        updated_rects[num_rects].w = dst.w;
        updated_rects[num_rects].h = dst.h;
        num_rects++;
      }
    }
    dirty_area_count = 0;
  }
  display_timing_scaler_end();

  if( SDL_MUSTLOCK( scaled_screen ) ) SDL_UnlockSurface( scaled_screen );

  sdl2display_icon_overlay();

  if( full_refresh ) {
    SDL_UpdateTexture( sdl2_texture, NULL, scaled_screen->pixels,
                       scaled_screen->pitch );
  } else {
    for( i = 0; i < num_rects; i++ ) {
      SDL_Rect *r = &updated_rects[i];
      void *pixels = (libspectrum_byte *)scaled_screen->pixels +
                     r->x * scaled_screen->format->BytesPerPixel +
                     r->y * scaled_screen->pitch;

      SDL_UpdateTexture( sdl2_texture, r, pixels, scaled_screen->pitch );
    }
  }

  SDL_RenderClear( sdl2_renderer );
  SDL_RenderCopy( sdl2_renderer, sdl2_texture, NULL, NULL );
  display_timing_presentation_begin();
  SDL_RenderPresent( sdl2_renderer );
  display_timing_presentation_end();

  sdl2display_force_full_refresh = 0;

  display_timing_frame_end();
}

int
uidisplay_end( void )
{
  display_ui_initialised = 0;

  if( tmp_screen ) {
    free( tmp_screen->pixels );
    SDL_FreeSurface( tmp_screen );
    tmp_screen = NULL;
  }

  if( scaled_screen ) {
    SDL_FreeSurface( scaled_screen );
    scaled_screen = NULL;
  }

  sdl2display_free_status_icons();

  if( saved ) {
    SDL_FreeSurface( saved );
    saved = NULL;
  }

  if( sdl2_texture ) {
    SDL_DestroyTexture( sdl2_texture );
    sdl2_texture = NULL;
    texture_width = texture_height = 0;
  }

  if( sdl2_renderer ) {
    SDL_DestroyRenderer( sdl2_renderer );
    sdl2_renderer = NULL;
  }

  if( sdl2_window ) {
    SDL_DestroyWindow( sdl2_window );
    sdl2_window = NULL;
  }

  return 0;
}

int
ui_statusbar_update( ui_statusbar_item item, ui_statusbar_state state )
{
  switch( item ) {
  case UI_STATUSBAR_ITEM_DISK:
    sdl2_disk_state = state;
    sdl2_status_updated = 1;
    return 0;

  case UI_STATUSBAR_ITEM_PAUSED:
    return 0;

  case UI_STATUSBAR_ITEM_TAPE:
    sdl2_tape_state = state;
    sdl2_status_updated = 1;
    return 0;

  case UI_STATUSBAR_ITEM_MICRODRIVE:
    sdl2_mdr_state = state;
    sdl2_status_updated = 1;
    return 0;

  case UI_STATUSBAR_ITEM_MOUSE:
    return 0;
  }

  ui_error( UI_ERROR_ERROR, "Attempt to update unknown statusbar item %d",
            item );
  return 1;
}
