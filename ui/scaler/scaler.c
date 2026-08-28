/* scaler.c: code for selecting (etc) scalers
 * Copyright (C) 2003-2026 Fredrick Meunier, Philip Kendall
 * Copyright (c) 2015-2019 Sergio Baldoví
 * 
 * Originally taken from ScummVM - Scumm Interpreter
 * Copyright (C) 2001  Ludvig Strigeus
 * Copyright (C) 2001/2002 The ScummVM project
 *
 * HQ2x and HQ3x scalers taken from HiEnd3D Demos (http://www.hiend3d.com)
 * Copyright (C) 2003 MaxSt ( maxst@hiend3d.com )
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#include "config.h"

#include <string.h>

#include "libspectrum.h"

#include "machine.h"
#include "scaler.h"
#include "scaler_internals.h"
#include "settings.h"
#include "ui/ui.h"
#include "ui/uidisplay.h"
#include "utils.h"

static int scaler_supported[ SCALER_NUM ] = {0};

int scalers_registered = 0;

struct scaler_info {

  const char *name;
  const char *id;
  enum scaler_flags_t flags;
  scale_factor_t scaling_factor;
  ScalerProc *scaler16, *scaler32;
  scaler_expand_fn *expander;

};

/* Every build links scalers32.o (the 32-bit scaler routines) except the Cocoa
   app, whose Xcode project compiles only the 16-bit variant.  Wire the 32-bit
   routines into the table everywhere they exist; the Cocoa build renders at
   16bpp and never uses them. */
#ifdef UI_COCOA
#define SCALER32( name ) NULL
#else
#define SCALER32( name ) name##_32
#endif

/* The expander functions */

/* Clip back to screen size after expansion */
static void clip( int *x, int *y, int *w, int *h,
		  int image_width, int image_height );

static void expand_1( int *x, int *y, int *w, int *h,
		      int image_width, int image_height );
static void expand_sai( int *x, int *y, int *w, int *h,
			int image_width, int image_height );
static void expand_full_width_and_1_line( int *x, int *y, int *w, int *h,
                                           int image_width, int image_height );
static void expand_dotmatrix( int *x, int *y, int *w, int *h,
			      int image_width, int image_height );

/* Information on each of the available scalers. Make sure this array stays
   in the same order as scaler.h:scaler_type */
static struct scaler_info available_scalers[] = {

  { "Timex Half (smoothed)", "half", SCALER_FLAGS_NONE, SCALE_FACTOR_HALF,
    scaler_Half_16,       SCALER32( scaler_Half ),       NULL                },
  { "Timex Half (skipping)", "halfskip", SCALER_FLAGS_NONE, SCALE_FACTOR_HALF,
    scaler_HalfSkip_16,   SCALER32( scaler_HalfSkip ),   NULL                },
  { "Normal",	       "normal",     SCALER_FLAGS_NONE, SCALE_FACTOR_ONE,
    scaler_Normal1x_16,   SCALER32( scaler_Normal1x ),   NULL                },
  { "Double size",     "2x",	     SCALER_FLAGS_NONE, SCALE_FACTOR_TWO,
    scaler_Normal2x_16,   SCALER32( scaler_Normal2x ),   NULL                },
  { "Triple size",     "3x",	     SCALER_FLAGS_NONE, SCALE_FACTOR_THREE,
    scaler_Normal3x_16,   SCALER32( scaler_Normal3x ),   NULL		    },
  { "Quadruple size",  "4x",	     SCALER_FLAGS_NONE, SCALE_FACTOR_FOUR,
    scaler_Normal4x_16,   SCALER32( scaler_Normal4x ),   NULL        },
  { "2xSaI",	       "2xsai",	     SCALER_FLAGS_EXPAND, SCALE_FACTOR_TWO,
    scaler_2xSaI_16,      SCALER32( scaler_2xSaI ),      expand_sai          },
  { "Super 2xSaI",     "super2xsai", SCALER_FLAGS_EXPAND, SCALE_FACTOR_TWO,
    scaler_Super2xSaI_16, SCALER32( scaler_Super2xSaI ), expand_sai          },
  { "SuperEagle",      "supereagle", SCALER_FLAGS_EXPAND, SCALE_FACTOR_TWO,
    scaler_SuperEagle_16, SCALER32( scaler_SuperEagle ), expand_sai          },
  { "AdvMAME 2x",      "advmame2x",  SCALER_FLAGS_EXPAND, SCALE_FACTOR_TWO,
    scaler_AdvMame2x_16,  SCALER32( scaler_AdvMame2x ),  expand_1            },
  { "AdvMAME 3x",      "advmame3x",  SCALER_FLAGS_EXPAND, SCALE_FACTOR_THREE,
    scaler_AdvMame3x_16,  SCALER32( scaler_AdvMame3x ),  expand_1            },
  { "TV 2x",	       "tv2x",	     SCALER_FLAGS_NONE,   SCALE_FACTOR_TWO,
    scaler_TV2x_16,       SCALER32( scaler_TV2x ),       NULL                },
  { "TV 3x",	       "tv3x",	     SCALER_FLAGS_NONE,   SCALE_FACTOR_THREE,
    scaler_TV3x_16,       SCALER32( scaler_TV3x ),       NULL                },
  { "TV 4x",	       "tv4x",	     SCALER_FLAGS_NONE,   SCALE_FACTOR_FOUR,
    scaler_TV4x_16,       SCALER32( scaler_TV4x ),       NULL                },
  { "Timex TV",	       "timextv",    SCALER_FLAGS_NONE,   SCALE_FACTOR_ONE,
    scaler_TimexTV_16,    SCALER32( scaler_TimexTV ),    NULL                },
  { "Dot Matrix",      "dotmatrix",  SCALER_FLAGS_EXPAND, SCALE_FACTOR_TWO,
    scaler_DotMatrix_16,  SCALER32( scaler_DotMatrix ),  expand_dotmatrix    },
  { "Timex 1.5x",      "timex15x",   SCALER_FLAGS_NONE,   SCALE_FACTOR_ONE_HALF,
    scaler_Timex1_5x_16,  SCALER32( scaler_Timex1_5x ),  NULL                },
  { "Timex 2x",        "timex2x",    SCALER_FLAGS_NONE,   SCALE_FACTOR_TWO,
    scaler_Normal2x_16,  SCALER32( scaler_Normal2x ),    NULL                },
  { "PAL TV 2x",       "paltv2x",   SCALER_FLAGS_EXPAND,  SCALE_FACTOR_TWO,
    scaler_PalTV2x_16,    SCALER32( scaler_PalTV2x ),
    expand_full_width_and_1_line },
  { "PAL TV 3x",       "paltv3x",   SCALER_FLAGS_EXPAND,  SCALE_FACTOR_THREE,
    scaler_PalTV3x_16,    SCALER32( scaler_PalTV3x ),
    expand_full_width_and_1_line },
  { "PAL TV 4x",       "paltv4x",   SCALER_FLAGS_EXPAND,  SCALE_FACTOR_FOUR,
    scaler_PalTV4x_16,    SCALER32( scaler_PalTV4x ),
    expand_full_width_and_1_line },
  { "HQ 2x",           "hq2x",      SCALER_FLAGS_EXPAND,  SCALE_FACTOR_TWO,
    scaler_HQ2x_16,       SCALER32( scaler_HQ2x ),    expand_1            },
  { "HQ 3x",           "hq3x",      SCALER_FLAGS_EXPAND,  SCALE_FACTOR_THREE,
    scaler_HQ3x_16,       SCALER32( scaler_HQ3x ),    expand_1            },
  { "HQ 4x",           "hq4x",      SCALER_FLAGS_EXPAND,  SCALE_FACTOR_FOUR,
    scaler_HQ4x_16,       SCALER32( scaler_HQ4x ),    expand_1            },
  { "NTSC TV 2x",      "ntsctv2x",  SCALER_FLAGS_FULL_REFRESH, SCALE_FACTOR_TWO,
    scaler_blargg_NTSC_2x_16, SCALER32( scaler_blargg_NTSC_2x ), NULL },
  { "NTSC TV 3x",      "ntsctv3x",  SCALER_FLAGS_FULL_REFRESH, SCALE_FACTOR_THREE,
    scaler_blargg_NTSC_3x_16, SCALER32( scaler_blargg_NTSC_3x ), NULL },
  { "NTSC TV 4x",      "ntsctv4x",  SCALER_FLAGS_FULL_REFRESH, SCALE_FACTOR_FOUR,
    scaler_blargg_NTSC_4x_16, SCALER32( scaler_blargg_NTSC_4x ), NULL },
};

/* Scalers that share the same look but render the image at a different scale
   belong to the same family. Single-size scalers (2xSaI, Dot Matrix, ...) are
   not listed here and are left untouched when the display is resized.

   Important: a machine cannot have a scaler listed in more than one family,
   that is, a scaler cannot be in two different rows unless those two rows
   are for different machines. In this case SCALER_NORMAL appears twice
   precisely because no machine can use both families of scalers. */
#define SCALER_SIZES 4
static const scaler_type scaler_family_table[][ SCALER_SIZES ] = {
  /*   1x                 2x                 3x                 4x          */
  { SCALER_NORMAL,    SCALER_DOUBLESIZE, SCALER_TRIPLESIZE, SCALER_QUADSIZE  },
  { SCALER_TV2X,      SCALER_TV2X,       SCALER_TV3X,       SCALER_TV4X      },
  { SCALER_PALTV2X,   SCALER_PALTV2X,    SCALER_PALTV3X,    SCALER_PALTV4X   },
  { SCALER_HQ2X,      SCALER_HQ2X,       SCALER_HQ3X,       SCALER_HQ4X      },
  { SCALER_NTSC2X,    SCALER_NTSC2X,     SCALER_NTSC3X,     SCALER_NTSC4X    },
  { SCALER_ADVMAME2X, SCALER_ADVMAME2X,  SCALER_ADVMAME3X,  SCALER_ADVMAME3X },
  /* Timex sizes 1x-4x correspond to scaling factors 0.5x, 1x, 1.5x and 2x */
  { SCALER_HALF,      SCALER_NORMAL,     SCALER_TIMEX1_5X,  SCALER_TIMEX2X   },
};

scaler_type current_scaler = SCALER_NUM;
ScalerProc *scaler_proc16, *scaler_proc32;
scaler_flags_t scaler_flags;
scaler_expand_fn *scaler_expander;

int
scaler_activate_scaler( scaler_type scaler )
{
  if( !scaler_is_supported( scaler ) ) return 1;

  if( current_scaler == scaler ) return 0;

  current_scaler = scaler;

  if( settings_current.start_scaler_mode ) libspectrum_free( settings_current.start_scaler_mode );
  settings_current.start_scaler_mode =
    utils_safe_strdup( available_scalers[current_scaler].id );

  scaler_proc16 = scaler_get_proc16( current_scaler );
  scaler_proc32 = scaler_get_proc32( current_scaler );
  scaler_flags = scaler_get_flags( current_scaler );
  scaler_expander = scaler_get_expander( current_scaler );

  return 0;
}

int
scaler_select_scaler( scaler_type scaler )
{
  if( scaler_activate_scaler( scaler ) ) return 1;

  uidisplay_set_next_hotswap_reason( UIDISPLAY_HOTSWAP_REASON_SCALER_EXPLICIT );
  return uidisplay_hotswap_gfx_mode();
}

int
scaler_select_bitformat( scaler_bitformat_t bitformat )
{
  switch( bitformat ) {
  case BITFORMAT_555:
  case BITFORMAT_565:
    return scaler_select_bitformat_16( bitformat );

  case BITFORMAT_X8B8G8R8:
  case BITFORMAT_X8R8G8B8:
    return scaler_select_bitformat_32( bitformat );

  default:
    ui_error( UI_ERROR_ERROR, "unknown bitformat %d", bitformat );
    return 1;
  }
}

int
scaler_select_id( const char *id )
{
  int i = scaler_get_type( id );

  if( i >= 0 ) {
    scaler_select_scaler( i );
    return 0;
  }

  ui_error( UI_ERROR_ERROR, "Scaler id '%s' unknown", id );
  return 1;
}

int
scaler_get_type( const char *id )
{
  scaler_type i;

  if( !strcmp( id, "paltv" ) ) {
    ui_error( UI_ERROR_WARNING,
              "Scaler id '%s' has been removed; using 'normal'", id );
    return scaler_select_scaler( SCALER_NORMAL );
  }

  for( i=0; i < SCALER_NUM; i++ ) {
    if( ! strcmp( available_scalers[i].id, id ) ) {
      return i;
    }
  }

  return -1;
}

void
scaler_register_clear( void )
{
  scalers_registered = 0;
  memset( scaler_supported, 0, sizeof(int) * SCALER_NUM );
}

void
scaler_register( scaler_type scaler )
{
  if( scaler_supported[scaler] == 1 ) return;
  scalers_registered++;
  scaler_supported[scaler] = 1;
}

int
scaler_is_supported( scaler_type scaler )
{
  return ( scaler >= SCALER_NUM ? 0 : scaler_supported[scaler] );
}

const char *
scaler_name( scaler_type scaler )
{
  return available_scalers[scaler].name;
}

const char *
scaler_id( scaler_type scaler )
{
  return available_scalers[scaler].id;
}

ScalerProc*
scaler_get_proc16( scaler_type scaler )
{
  return available_scalers[scaler].scaler16;
}

ScalerProc*
scaler_get_proc32( scaler_type scaler )
{
  return available_scalers[scaler].scaler32;
}

scaler_flags_t
scaler_get_flags( scaler_type scaler )
{
  return available_scalers[scaler].flags;
}

float
scaler_get_scaling_factor( scaler_type scaler )
{
  switch ( available_scalers[scaler].scaling_factor ) {
  case SCALE_FACTOR_HALF: return 0.5f;
  case SCALE_FACTOR_ONE: return 1.0f;
  case SCALE_FACTOR_TWO: return 2.0f;
  case SCALE_FACTOR_THREE: return 3.0f;
  case SCALE_FACTOR_FOUR: return 4.0f;
  case SCALE_FACTOR_ONE_HALF: return 1.5f;
  }
  /* Silence warning */
  return 1.0f;
}

int
scaler_scale_number( scaler_type scaler, int num )
{
  switch ( available_scalers[scaler].scaling_factor ) {
  case SCALE_FACTOR_HALF: return num>>1;
  case SCALE_FACTOR_ONE: return num;
  case SCALE_FACTOR_TWO: return num<<1;
  case SCALE_FACTOR_THREE: return num*3;
  case SCALE_FACTOR_FOUR: return num*4;
  case SCALE_FACTOR_ONE_HALF: return num + (num>>1);
  }
  /* Silence warning */
  return num;
}

scaler_expand_fn*
scaler_get_expander( scaler_type scaler )
{
  return available_scalers[scaler].expander;
}

/* Return the scaler that should be used at the given size (1x to 4x)
   from the same family as 'scaler' (e.g. PAL TV, HQ, etc.).

   SCALER_NORMAL belongs to two families (the regular one and the Timex one),
   so we only consider families whose scalers are all currently registered.
   That is the family for the current machine. */
scaler_type
scaler_for_size( scaler_type scaler, int size )
{
  size_t family, i;

  if( size < 1 ) size = 1;
  if( size > SCALER_SIZES ) size = SCALER_SIZES;

  for( family = 0; family < ARRAY_SIZE( scaler_family_table ); family++ ) {
    int found = 0, supported = 1;

    /* For every row of scalers, check if:
       - The current one ('scaler') is there.
       - All four scalers are supported by the current machine */
    for( i = 0; i < SCALER_SIZES; i++ ) {
      if( scaler_family_table[family][i] == scaler ) found = 1;
      if( !scaler_is_supported( scaler_family_table[family][i] ) )
        supported = 0;
    }

    /* If that's the case, select the best scaler for the given size */
    if( found && supported )
      return scaler_family_table[family][size - 1];
  }

  /* Single-size scaler (or not in any family): leave it as it is */
  return scaler;
}

/* The expansion functions */

/* Clip after expansion */
static inline void
clip( int *x, int *y, int *w, int *h, int image_width, int image_height )
{
  if ( *x < 0 ) { *w += *x; *x=0; }
  if ( *y < 0 ) { *h += *y; *y=0; }
  if ( *w > image_width - *x ) *w = image_width - *x;
  if ( *h > image_height - *y ) *h = image_height - *y;
}

/* Expand one pixel in all directions */
static void
expand_1( int *x, int *y, int *w, int *h, int image_width, int image_height )
{
  (*x)--; (*y)--; (*w)+=2; (*h)+=2;
  clip( x, y, w, h, image_width, image_height );
}

/* Expand two pixels up and left and one pixel down and right */
static void
expand_sai( int *x, int *y, int *w, int *h, int image_width, int image_height )
{
  (*x)-=2; (*y)-=2; (*w)+=3; (*h)+=3;
  clip( x, y, w, h, image_width, image_height );
}

/* Expand to the full source-line width and the following line. */
static void
expand_full_width_and_1_line( int *x, int *y, int *w, int *h,
                   int image_width, int image_height )
{
  *x = 0;
  *w = image_width;
  (*h)++;
  clip( x, y, w, h, image_width, image_height );
}

/* Expand to a even y co-ord */
static void
expand_dotmatrix( int *x GCC_UNUSED, int *y GCC_UNUSED, int *w GCC_UNUSED,
		  int *h, int image_width GCC_UNUSED,
		  int image_height GCC_UNUSED )
{
  int y_mod = (*y) % 2;

  (*y)-=y_mod;
  (*h)+=y_mod;
}
