/* palcompositetest.c: regression tests for PAL composite scaler output
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "libspectrum.h"

#include "settings.h"
#include "ui/scaler/scaler_internals.h"
#include "ui/ui.h"

#define TEST_WIDTH 8
#define TEST_HEIGHT 2
#define TEST_MAX_INPUT_HEIGHT 3
#define TEST_ROW_PIXELS ( TEST_WIDTH + 3 )
#define TEST_MAX_SCALE 4
#define TEST_OUTPUT_WIDTH ( TEST_WIDTH * TEST_MAX_SCALE )
#define TEST_OUTPUT_HEIGHT ( TEST_HEIGHT * TEST_MAX_SCALE )

typedef struct test_rgb {
  libspectrum_byte red;
  libspectrum_byte green;
  libspectrum_byte blue;
} test_rgb;

settings_info settings_current;

int
ui_error( ui_error_level severity GCC_UNUSED, const char *format GCC_UNUSED, ... )
{
  return 0;
}

static const test_rgb source_colours[ TEST_HEIGHT ][ TEST_WIDTH ] = {
  { { 0, 0, 0 }, { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
    { 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 },
    { 255, 255, 255 } },
  { { 16, 32, 48 }, { 64, 80, 96 }, { 112, 128, 144 },
    { 160, 176, 192 }, { 208, 224, 240 }, { 240, 224, 208 },
    { 192, 176, 160 }, { 144, 128, 112 } }
};

static libspectrum_dword
pack_32( scaler_bitformat_t format, test_rgb colour )
{
#ifdef WORDS_BIGENDIAN
  if( format == BITFORMAT_X8R8G8B8 )
    return colour.green << 8 | colour.red << 16 | colour.blue;
  return colour.red << 24 | colour.green << 16 | colour.blue << 8;
#else
  if( format == BITFORMAT_X8R8G8B8 )
    return colour.blue | colour.green << 8 | colour.red << 16;
  return colour.red | colour.green << 8 | colour.blue << 16;
#endif
}

static test_rgb
unpack_32( scaler_bitformat_t format, libspectrum_dword pixel )
{
  test_rgb colour;

#ifdef WORDS_BIGENDIAN
  if( format == BITFORMAT_X8R8G8B8 ) {
    colour.red = pixel >> 16;
    colour.green = pixel >> 8;
    colour.blue = pixel;
  } else {
    colour.red = pixel >> 24;
    colour.green = pixel >> 16;
    colour.blue = pixel >> 8;
  }
#else
  if( format == BITFORMAT_X8R8G8B8 ) {
    colour.red = pixel >> 16;
    colour.green = pixel >> 8;
    colour.blue = pixel;
  } else {
    colour.red = pixel;
    colour.green = pixel >> 8;
    colour.blue = pixel >> 16;
  }
#endif

  return colour;
}

static libspectrum_word
pack_16( scaler_bitformat_t format, test_rgb colour )
{
  if( format == BITFORMAT_565 )
    return ( colour.red >> 3 ) << 11 | ( colour.green >> 2 ) << 5 |
           ( colour.blue >> 3 );

  return ( colour.red >> 3 ) << 10 | ( colour.green >> 3 ) << 5 |
         ( colour.blue >> 3 );
}

static void
build_32_source( scaler_bitformat_t format,
                 libspectrum_dword source[ TEST_HEIGHT ][ TEST_ROW_PIXELS ] )
{
  int row, column;

  for( row = 0; row < TEST_HEIGHT; row++ ) {
    for( column = 0; column < TEST_WIDTH; column++ )
      source[row][column + 1] = pack_32( format,
                                         source_colours[row][column] );
    source[row][0] = source[row][1];
    source[row][TEST_WIDTH + 1] = source[row][TEST_WIDTH];
    source[row][TEST_WIDTH + 2] = source[row][TEST_WIDTH];
  }
}

static void
build_16_source( scaler_bitformat_t format,
                 libspectrum_word source[ TEST_HEIGHT ][ TEST_ROW_PIXELS ] )
{
  int row, column;

  for( row = 0; row < TEST_HEIGHT; row++ ) {
    for( column = 0; column < TEST_WIDTH; column++ )
      source[row][column + 1] = pack_16( format,
                                         source_colours[row][column] );
    source[row][0] = source[row][1];
    source[row][TEST_WIDTH + 1] = source[row][TEST_WIDTH];
    source[row][TEST_WIDTH + 2] = source[row][TEST_WIDTH];
  }
}

static void
run_32( scaler_bitformat_t format, ScalerProc *scaler, int scale,
        const libspectrum_dword *source, int height,
        libspectrum_dword output[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ] )
{
  libspectrum_dword padded[ TEST_MAX_INPUT_HEIGHT + 1 ][ TEST_ROW_PIXELS ];

  memcpy( padded[1], source, height * sizeof( padded[0] ) );
  memcpy( padded[0], padded[1], sizeof( padded[0] ) );
  memset( output, 0, TEST_OUTPUT_HEIGHT * TEST_OUTPUT_WIDTH *
          sizeof( output[0][0] ) );
  scaler_select_bitformat_32( format );
  scaler( (const libspectrum_byte*)&padded[1][1],
          TEST_ROW_PIXELS * sizeof( padded[0][0] ), (libspectrum_byte*)output,
          TEST_OUTPUT_WIDTH * sizeof( output[0][0] ), TEST_WIDTH, height );
  (void)scale;
}

static void
run_16( scaler_bitformat_t format, ScalerProc *scaler, int scale,
        const libspectrum_word *source, int height,
        libspectrum_word output[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ] )
{
  libspectrum_word padded[ TEST_MAX_INPUT_HEIGHT + 1 ][ TEST_ROW_PIXELS ];

  memcpy( padded[1], source, height * sizeof( padded[0] ) );
  memcpy( padded[0], padded[1], sizeof( padded[0] ) );
  memset( output, 0, TEST_OUTPUT_HEIGHT * TEST_OUTPUT_WIDTH *
          sizeof( output[0][0] ) );
  scaler_select_bitformat_16( format );
  scaler( (const libspectrum_byte*)&padded[1][1],
          TEST_ROW_PIXELS * sizeof( padded[0][0] ), (libspectrum_byte*)output,
          TEST_OUTPUT_WIDTH * sizeof( output[0][0] ), TEST_WIDTH, height );
  (void)scale;
}

static libspectrum_signed_dword
rgb_to_y( test_rgb colour )
{
  return ( 2449L * colour.red + 4809L * colour.green + 934L * colour.blue +
           1024 ) >> 11;
}

static libspectrum_signed_dword
rgb_to_u( test_rgb colour )
{
  return ( 4096L * colour.blue - 1383L * colour.red - 2713L * colour.green +
           1024 ) >> 11;
}

static libspectrum_signed_dword
rgb_to_v( test_rgb colour )
{
  return ( 4096L * colour.red - 3430L * colour.green - 666L * colour.blue +
           1024 ) >> 11;
}

static libspectrum_byte
clamp_colour( libspectrum_signed_dword colour )
{
  if( colour < 0 ) return 0;
  if( colour > 255 ) return 255;
  return colour;
}

static test_rgb
chroma_average( test_rgb previous, test_rgb current )
{
  libspectrum_signed_dword y = rgb_to_y( current );
  libspectrum_signed_dword u = ( rgb_to_u( current ) + rgb_to_u( previous ) ) / 2;
  libspectrum_signed_dword v = ( rgb_to_v( current ) + rgb_to_v( previous ) ) / 2;
  test_rgb result;

  result.red = clamp_colour( ( 8192L * y + 11485L * v + 16384 ) >> 15 );
  result.green = clamp_colour( ( 8192L * y - 2819L * u - 5850L * v +
                                 16384 ) >> 15 );
  result.blue = clamp_colour( ( 8192L * y + 14516L * u + 16384 ) >> 15 );
  return result;
}

static int
same_colour( test_rgb a, test_rgb b )
{
  return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static int
check_32_delay_line( scaler_bitformat_t format, ScalerProc *scaler,
                     int scale, const char *name )
{
  libspectrum_dword source[ TEST_HEIGHT ][ TEST_ROW_PIXELS ];
  libspectrum_dword top[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  libspectrum_dword bottom[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  libspectrum_dword combined[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  int row, column;
  int differs_from_rgb_average = 0;

  build_32_source( format, source );
  run_32( format, scaler, scale, &source[0][0], 1, top );
  run_32( format, scaler, scale, &source[1][0], 1, bottom );
  run_32( format, scaler, scale, &source[0][0], TEST_HEIGHT, combined );

  for( row = 0; row < scale; row++ ) {
    for( column = 0; column < TEST_WIDTH * scale; column++ ) {
      test_rgb first = unpack_32( format, combined[row][column] );
      test_rgb expected = unpack_32( format, top[row][column] );
      test_rgb previous = unpack_32( format, top[row][column] );
      test_rgb current = unpack_32( format, bottom[row][column] );
      test_rgb mixed = unpack_32( format, combined[row + scale][column] );
      test_rgb chroma = chroma_average( previous, current );
      test_rgb rgb_average;

      if( !same_colour( first, expected ) ) {
        fprintf( stderr, "%s changed first-line output at row %d, pixel %d\n",
                 name, row, column );
        return 1;
      }

      if( !same_colour( mixed, chroma ) ) {
        fprintf( stderr, "%s did not apply chroma-only delay at row %d, pixel %d\n",
                 name, row, column );
        return 1;
      }

      rgb_average.red = ( previous.red + current.red ) / 2;
      rgb_average.green = ( previous.green + current.green ) / 2;
      rgb_average.blue = ( previous.blue + current.blue ) / 2;
      if( !same_colour( mixed, rgb_average ) ) differs_from_rgb_average = 1;
    }
  }

  if( !differs_from_rgb_average ) {
    fprintf( stderr, "%s appears to average whole RGB values\n", name );
    return 1;
  }

  return 0;
}

static int
check_colourmixer_delay_line_32( void )
{
  /* Colour code 22 followed by the alternating green/magenta code 34. */
  static const test_rgb colours[ 3 ] = {
    { 255, 0, 0 }, { 0, 255, 0 }, { 255, 0, 255 }
  };
  libspectrum_dword source[ 3 ][ TEST_ROW_PIXELS ];
  libspectrum_dword first[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  libspectrum_dword second[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  libspectrum_dword third[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  libspectrum_dword combined[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  int row, column;

  for( row = 0; row < 3; row++ ) {
    for( column = 0; column < TEST_WIDTH; column++ )
      source[row][column + 1] = pack_32( BITFORMAT_X8B8G8R8,
                                         colours[row] );
    source[row][0] = source[row][1];
    source[row][TEST_WIDTH + 1] = source[row][TEST_WIDTH];
    source[row][TEST_WIDTH + 2] = source[row][TEST_WIDTH];
  }

  run_32( BITFORMAT_X8B8G8R8, scaler_PalTV2x_32, 2, &source[0][0], 1,
          first );
  run_32( BITFORMAT_X8B8G8R8, scaler_PalTV2x_32, 2, &source[1][0], 1,
          second );
  run_32( BITFORMAT_X8B8G8R8, scaler_PalTV2x_32, 2, &source[2][0], 1,
          third );
  run_32( BITFORMAT_X8B8G8R8, scaler_PalTV2x_32, 2, &source[0][0], 3,
          combined );

  for( row = 0; row < 2; row++ ) {
    for( column = 0; column < TEST_WIDTH * 2; column++ ) {
      test_rgb first_colour = unpack_32( BITFORMAT_X8B8G8R8,
                                         first[row][column] );
      test_rgb second_colour = unpack_32( BITFORMAT_X8B8G8R8,
                                          second[row][column] );
      test_rgb third_colour = unpack_32( BITFORMAT_X8B8G8R8,
                                         third[row][column] );
      test_rgb second_actual = unpack_32( BITFORMAT_X8B8G8R8,
                                          combined[row + 2][column] );
      test_rgb third_actual = unpack_32( BITFORMAT_X8B8G8R8,
                                         combined[row + 4][column] );
      test_rgb second_expected = chroma_average( first_colour, second_colour );
      test_rgb third_expected = chroma_average( second_colour, third_colour );
      test_rgb recursive = chroma_average( second_expected, third_colour );

      if( !same_colour( second_actual, second_expected ) ) {
        fprintf( stderr, "ColourMixer transition did not blend line 2\n" );
        return 1;
      }

      if( !same_colour( third_actual, third_expected ) ) {
        fprintf( stderr,
                 "ColourMixer transition did not use raw line 2 chroma\n" );
        return 1;
      }

      if( same_colour( third_expected, recursive ) ) {
        fprintf( stderr, "ColourMixer fixture cannot detect recursive blending\n" );
        return 1;
      }

      if( same_colour( third_expected, third_colour ) ) {
        fprintf( stderr, "ColourMixer fixture cannot detect paired blending\n" );
        return 1;
      }
    }
  }

  return 0;
}

static int
check_16_delay_line( scaler_bitformat_t format, ScalerProc *scaler,
                     int scale, const char *name )
{
  libspectrum_word source[ TEST_HEIGHT ][ TEST_ROW_PIXELS ];
  libspectrum_word top[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  libspectrum_word bottom[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  libspectrum_word combined[ TEST_OUTPUT_HEIGHT ][ TEST_OUTPUT_WIDTH ];
  int row, column;
  int changed = 0;

  build_16_source( format, source );
  run_16( format, scaler, scale, &source[0][0], 1, top );
  run_16( format, scaler, scale, &source[1][0], 1, bottom );
  run_16( format, scaler, scale, &source[0][0], TEST_HEIGHT, combined );

  for( row = 0; row < scale; row++ ) {
    for( column = 0; column < TEST_WIDTH * scale; column++ ) {
      if( combined[row][column] != top[row][column] ) {
        fprintf( stderr, "%s changed 16-bit first-line output at row %d, pixel %d\n",
                 name, row, column );
        return 1;
      }
      if( combined[row + scale][column] != bottom[row][column] ) changed = 1;
    }
  }

  if( !changed ) {
    fprintf( stderr, "%s did not blend 16-bit second-line chroma\n", name );
    return 1;
  }

  return 0;
}

static int
check_partial_redraw_32( void )
{
  libspectrum_dword old_source[ 4 ][ TEST_ROW_PIXELS ];
  libspectrum_dword new_source[ 4 ][ TEST_ROW_PIXELS ];
  libspectrum_dword full[ 3 * 2 ][ TEST_WIDTH * 2 ];
  libspectrum_dword partial[ 3 * 2 ][ TEST_WIDTH * 2 ];
  int column;

  {
    libspectrum_dword tmp[ TEST_HEIGHT ][ TEST_ROW_PIXELS ];
    build_32_source( BITFORMAT_X8B8G8R8, tmp );
    memcpy( old_source[1], tmp[0], sizeof( old_source[0] ) );
  }
  memcpy( old_source[2], old_source[1], sizeof( old_source[0] ) );
  memcpy( old_source[3], old_source[1], sizeof( old_source[0] ) );
  memcpy( old_source[0], old_source[1], sizeof( old_source[0] ) );
  memcpy( new_source, old_source, sizeof( new_source ) );

  for( column = 0; column < TEST_WIDTH; column++ )
    new_source[2][column + 1] = pack_32( BITFORMAT_X8B8G8R8,
                                         source_colours[1][column] );
  new_source[2][0] = new_source[2][1];
  new_source[2][TEST_WIDTH + 1] = new_source[2][TEST_WIDTH];
  new_source[2][TEST_WIDTH + 2] = new_source[2][TEST_WIDTH];

  scaler_select_bitformat_32( BITFORMAT_X8B8G8R8 );
  scaler_PalTV2x_32( (const libspectrum_byte*)&old_source[1][1],
                     TEST_ROW_PIXELS * sizeof( old_source[0][0] ),
                     (libspectrum_byte*)partial, sizeof( partial[0] ),
                     TEST_WIDTH, 3 );
  scaler_PalTV2x_32( (const libspectrum_byte*)&new_source[1][1],
                     TEST_ROW_PIXELS * sizeof( new_source[0][0] ),
                     (libspectrum_byte*)full, sizeof( full[0] ),
                     TEST_WIDTH, 3 );

  /* Redraw the changed row and its successor without overwriting its
     predecessor. The scaler reads the preceding source row for chroma. */
  scaler_PalTV2x_32( (const libspectrum_byte*)&new_source[2][1],
                     TEST_ROW_PIXELS * sizeof( new_source[0][0] ),
                     (libspectrum_byte*)&partial[2][0], sizeof( partial[0] ),
                     TEST_WIDTH, 2 );

  if( memcmp( &partial[2][0], &full[2][0],
              4 * TEST_WIDTH * sizeof( full[0][0] ) ) ) {
    fprintf( stderr, "PAL TV partial redraw differs from full redraw\n" );
    return 1;
  }

  return 0;
}

static libspectrum_dword
output_checksum( const libspectrum_dword *output, int width, int height )
{
  libspectrum_dword checksum = 2166136261UL;
  int row, column;

  for( row = 0; row < height; row++ ) {
    for( column = 0; column < width; column++ ) {
      test_rgb colour = unpack_32( BITFORMAT_X8B8G8R8,
                                   output[row * width + column] );

      checksum = ( checksum ^ colour.red ) * 16777619UL;
      checksum = ( checksum ^ colour.green ) * 16777619UL;
      checksum = ( checksum ^ colour.blue ) * 16777619UL;
    }
  }

  return checksum;
}

static int
check_ntsc_regression( void )
{
  libspectrum_dword source[ TEST_HEIGHT ][ TEST_ROW_PIXELS ];
  libspectrum_dword output[ TEST_HEIGHT * 2 ][ TEST_WIDTH * 2 ];

  build_32_source( BITFORMAT_X8B8G8R8, source );
  memset( output, 0, sizeof( output ) );
  scaler_select_bitformat_32( BITFORMAT_X8B8G8R8 );
  scaler_blargg_NTSC_2x_32( (const libspectrum_byte*)&source[0][1],
                            TEST_ROW_PIXELS * sizeof( source[0][0] ),
                            (libspectrum_byte*)output,
                            sizeof( output[0] ), TEST_WIDTH, TEST_HEIGHT );

  if( output_checksum( &output[0][0], TEST_WIDTH * 2, TEST_HEIGHT * 2 ) !=
      0x2b785bb3UL ) {
    fprintf( stderr, "NTSC composite checksum mismatch: got %08x\n",
             output_checksum( &output[0][0], TEST_WIDTH * 2,
                              TEST_HEIGHT * 2 ) );
    return 1;
  }

  return 0;
}

static int
check_scaler( ScalerProc *scaler_32, ScalerProc *scaler_16, int scale,
              const char *name )
{
  if( check_32_delay_line( BITFORMAT_X8B8G8R8, scaler_32, scale, name ) ) return 1;
  if( check_32_delay_line( BITFORMAT_X8R8G8B8, scaler_32, scale, name ) ) return 1;
  if( check_16_delay_line( BITFORMAT_565, scaler_16, scale, name ) ) return 1;
  return check_16_delay_line( BITFORMAT_555, scaler_16, scale, name );
}

#ifdef main
/* SDL headers redefine main on Windows, but this test needs a normal entry point. */
#undef main
#endif

int
main( void )
{
  memset( &settings_current, 0, sizeof( settings_current ) );

  if( check_scaler( scaler_PalTV2x_32, scaler_PalTV2x_16, 2,
                    "PAL TV 2x" ) ) return 1;
  if( check_scaler( scaler_PalTV3x_32, scaler_PalTV3x_16, 3,
                    "PAL TV 3x" ) ) return 1;
  if( check_scaler( scaler_PalTV4x_32, scaler_PalTV4x_16, 4,
                    "PAL TV 4x" ) ) return 1;
  if( check_colourmixer_delay_line_32() ) return 1;
  if( check_partial_redraw_32() ) return 1;
  return check_ntsc_regression();
}
