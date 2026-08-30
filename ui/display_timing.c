/* display_timing.c: optional display timing instrumentation
   Copyright (c) 2026 Fredrick Meunier

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

*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <glib.h>
#endif

#include "display_timing.h"

struct display_timing {
  const char *backend;
  int enabled;
  unsigned long frame;
  unsigned long source_regions;
  unsigned long source_pixels;
  unsigned long presentation_count;
  unsigned long paint_count;
  long long input_us;
  long long scaler_us;
  long long presentation_us;
  long long paint_us;
  long long frame_start;
  long long input_start;
  long long scaler_start;
  long long presentation_start;
  long long paint_start;
};

static struct display_timing timing;

static long long
now_us( void )
{
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  LARGE_INTEGER now;

  if( !frequency.QuadPart ) QueryPerformanceFrequency( &frequency );
  QueryPerformanceCounter( &now );

  return (long long)( (double)now.QuadPart * 1000000 / frequency.QuadPart );
#else
  return g_get_monotonic_time();
#endif
}

void
display_timing_init( const char *backend )
{
  const char *value = getenv( "FUSE_DISPLAY_TIMING" );

  memset( &timing, 0, sizeof( timing ) );
  timing.backend = backend;
  timing.enabled = value && *value && strcmp( value, "0" );

  if( timing.enabled )
    fprintf( stderr, "display-timing backend=%s enabled\n", backend );
}

void
display_timing_input_begin( void )
{
  if( timing.enabled ) timing.input_start = now_us();
}

void
display_timing_input_end( void )
{
  if( timing.enabled ) timing.input_us += now_us() - timing.input_start;
}

void
display_timing_area( int width, int height )
{
  if( !timing.enabled ) return;

  if( !timing.frame_start ) timing.frame_start = now_us();
  timing.source_regions++;
  timing.source_pixels += width * height;
}

void
display_timing_scaler_begin( void )
{
  if( timing.enabled ) timing.scaler_start = now_us();
}

void
display_timing_scaler_end( void )
{
  if( timing.enabled ) timing.scaler_us += now_us() - timing.scaler_start;
}

void
display_timing_presentation_begin( void )
{
  if( timing.enabled ) timing.presentation_start = now_us();
}

void
display_timing_presentation_end( void )
{
  if( timing.enabled ) {
    timing.presentation_us += now_us() - timing.presentation_start;
    timing.presentation_count++;
  }
}

void
display_timing_paint_begin( void )
{
  if( timing.enabled ) timing.paint_start = now_us();
}

void
display_timing_paint_end( void )
{
  if( timing.enabled ) {
    timing.paint_us += now_us() - timing.paint_start;
    timing.paint_count++;
  }
}

void
display_timing_frame_end( void )
{
  long long frame_us;

  if( !timing.enabled ) return;

  frame_us = timing.frame_start ? now_us() - timing.frame_start : 0;
  fprintf( stderr,
           "display-timing backend=%s frame=%lu input_us=%lld "
           "source_regions=%lu source_pixels=%lu scaler_us=%lld "
           "presentation_count=%lu presentation_us=%lld paint_count=%lu "
           "paint_us=%lld frame_us=%lld\n",
           timing.backend, ++timing.frame, timing.input_us,
           timing.source_regions, timing.source_pixels, timing.scaler_us,
           timing.presentation_count, timing.presentation_us,
           timing.paint_count, timing.paint_us, frame_us );

  timing.source_regions = 0;
  timing.source_pixels = 0;
  timing.input_us = 0;
  timing.scaler_us = 0;
  timing.presentation_count = 0;
  timing.presentation_us = 0;
  timing.paint_count = 0;
  timing.paint_us = 0;
  timing.frame_start = 0;
}
