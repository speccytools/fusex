/* scalerexpandtest.c: regression tests for scaler dirty-region expansion
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "config.h"

#include <stdio.h>

#include "settings.h"
#include "ui/scaler/scaler.h"
#include "ui/ui.h"
#include "ui/uidisplay.h"
#include "utils.h"

settings_info settings_current;

int
ui_error( ui_error_level severity GCC_UNUSED, const char *format GCC_UNUSED, ... )
{
  return 0;
}

int
uidisplay_hotswap_gfx_mode( void )
{
  return 0;
}

void
uidisplay_set_next_hotswap_reason( uidisplay_hotswap_reason reason GCC_UNUSED )
{
}

char*
utils_safe_strdup( const char *src GCC_UNUSED )
{
  return NULL;
}

static int
scalerexpandtest_expect( int x, int y, int w, int h, int expected_x,
                         int expected_y, int expected_w, int expected_h )
{
  if( x == expected_x && y == expected_y && w == expected_w &&
      h == expected_h )
    return 0;

  fprintf( stderr, "got (%d, %d, %d, %d), expected (%d, %d, %d, %d)\n",
           x, y, w, h, expected_x, expected_y, expected_w, expected_h );
  return 1;
}

static int
scalerexpandtest_paltv( void )
{
  scaler_expand_fn *expander = scaler_get_expander( SCALER_PALTV2X );
  int x = 12, y = 8, w = 3, h = 1;

  if( !expander ) {
    fprintf( stderr, "PAL TV scaler has no dirty-region expander\n" );
    return 1;
  }

  if( scaler_get_flags( SCALER_PALTV2X ) & SCALER_FLAGS_FULL_REFRESH ) {
    fprintf( stderr, "PAL TV scaler unexpectedly requires full refresh\n" );
    return 1;
  }

  expander( &x, &y, &w, &h, 320, 240 );
  if( scalerexpandtest_expect( x, y, w, h, 0, 8, 320, 2 ) ) return 1;

  /* Last row: h clipped so it does not exceed the image height */
  x = 12; y = 239; w = 3; h = 1;
  expander( &x, &y, &w, &h, 320, 240 );
  return scalerexpandtest_expect( x, y, w, h, 0, 239, 320, 1 );
}

/* expand_1: used by AdvMAME 2x/3x and HQ scalers.
   Expands the dirty rectangle by one pixel in every direction, then clips. */
static int
scalerexpandtest_advmame( void )
{
  scaler_expand_fn *expander = scaler_get_expander( SCALER_ADVMAME2X );
  int x, y, w, h;

  if( !expander ) {
    fprintf( stderr, "AdvMAME 2x scaler has no dirty-region expander\n" );
    return 1;
  }

  if( scaler_get_flags( SCALER_ADVMAME2X ) & SCALER_FLAGS_FULL_REFRESH ) {
    fprintf( stderr, "AdvMAME 2x scaler unexpectedly requires full refresh\n" );
    return 1;
  }

  /* Interior point: expands by 1 in every direction */
  x = 10; y = 10; w = 5; h = 3;
  expander( &x, &y, &w, &h, 320, 240 );
  if( scalerexpandtest_expect( x, y, w, h, 9, 9, 7, 5 ) ) return 1;

  /* Top-left corner: negative coordinates are clipped to 0 */
  x = 0; y = 0; w = 4; h = 2;
  expander( &x, &y, &w, &h, 320, 240 );
  if( scalerexpandtest_expect( x, y, w, h, 0, 0, 5, 3 ) ) return 1;

  /* Bottom-right corner: width/height clipped to image bounds */
  x = 316; y = 237; w = 4; h = 3;
  expander( &x, &y, &w, &h, 320, 240 );
  return scalerexpandtest_expect( x, y, w, h, 315, 236, 5, 4 );
}

/* expand_sai: used by 2xSaI, Super 2xSaI, SuperEagle, and HQ scalers.
   Expands 2 pixels up and left, 1 pixel down and right, then clips. */
static int
scalerexpandtest_sai( void )
{
  scaler_expand_fn *expander = scaler_get_expander( SCALER_2XSAI );
  int x, y, w, h;

  if( !expander ) {
    fprintf( stderr, "2xSaI scaler has no dirty-region expander\n" );
    return 1;
  }

  if( scaler_get_flags( SCALER_2XSAI ) & SCALER_FLAGS_FULL_REFRESH ) {
    fprintf( stderr, "2xSaI scaler unexpectedly requires full refresh\n" );
    return 1;
  }

  /* Interior point: x-=2, y-=2, w+=3, h+=3 */
  x = 10; y = 10; w = 5; h = 3;
  expander( &x, &y, &w, &h, 320, 240 );
  if( scalerexpandtest_expect( x, y, w, h, 8, 8, 8, 6 ) ) return 1;

  /* Top-left corner: negative coordinates clipped to 0, w/h reduced by clip */
  x = 1; y = 1; w = 4; h = 4;
  expander( &x, &y, &w, &h, 320, 240 );
  if( scalerexpandtest_expect( x, y, w, h, 0, 0, 6, 6 ) ) return 1;

  /* Bottom-right corner: w/h clipped to image bounds */
  x = 316; y = 237; w = 4; h = 3;
  expander( &x, &y, &w, &h, 320, 240 );
  return scalerexpandtest_expect( x, y, w, h, 314, 235, 6, 5 );
}

/* expand_dotmatrix: rounds dirty-rect y down to the nearest even row. */
static int
scalerexpandtest_dotmatrix( void )
{
  scaler_expand_fn *expander = scaler_get_expander( SCALER_DOTMATRIX );
  int x, y, w, h;

  if( !expander ) {
    fprintf( stderr, "Dot Matrix scaler has no dirty-region expander\n" );
    return 1;
  }

  if( scaler_get_flags( SCALER_DOTMATRIX ) & SCALER_FLAGS_FULL_REFRESH ) {
    fprintf( stderr,
             "Dot Matrix scaler unexpectedly requires full refresh\n" );
    return 1;
  }

  /* Odd y: rounds down to even, h gains one extra row */
  x = 12; y = 9; w = 3; h = 1;
  expander( &x, &y, &w, &h, 320, 240 );
  if( scalerexpandtest_expect( x, y, w, h, 12, 8, 3, 2 ) ) return 1;

  /* Even y: no change to x, y, w, or h */
  x = 12; y = 8; w = 3; h = 1;
  expander( &x, &y, &w, &h, 320, 240 );
  if( scalerexpandtest_expect( x, y, w, h, 12, 8, 3, 1 ) ) return 1;

  /* y=0 (even): no rounding needed */
  x = 5; y = 0; w = 10; h = 2;
  expander( &x, &y, &w, &h, 320, 240 );
  return scalerexpandtest_expect( x, y, w, h, 5, 0, 10, 2 );
}

/* NTSC TV scalers require a full-screen refresh and have no expander. */
static int
scalerexpandtest_ntsctv( void )
{
  if( scaler_get_expander( SCALER_NTSC2X ) != NULL ) {
    fprintf( stderr, "NTSC TV 2x scaler unexpectedly has an expander\n" );
    return 1;
  }

  if( !( scaler_get_flags( SCALER_NTSC2X ) & SCALER_FLAGS_FULL_REFRESH ) ) {
    fprintf( stderr,
             "NTSC TV 2x scaler should have SCALER_FLAGS_FULL_REFRESH\n" );
    return 1;
  }

  if( scaler_get_expander( SCALER_NTSC3X ) != NULL ) {
    fprintf( stderr, "NTSC TV 3x scaler unexpectedly has an expander\n" );
    return 1;
  }

  if( !( scaler_get_flags( SCALER_NTSC3X ) & SCALER_FLAGS_FULL_REFRESH ) ) {
    fprintf( stderr,
             "NTSC TV 3x scaler should have SCALER_FLAGS_FULL_REFRESH\n" );
    return 1;
  }

  return 0;
}

#ifdef main
/* SDL headers redefine main on Windows, but this test needs a normal entry point. */
#undef main
#endif

int
main( void )
{
  int r = 0;

  r += scalerexpandtest_paltv();
  r += scalerexpandtest_advmame();
  r += scalerexpandtest_sai();
  r += scalerexpandtest_dotmatrix();
  r += scalerexpandtest_ntsctv();

  return r;
}
