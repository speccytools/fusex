/* speaker_filter.c: Built-in Spectrum speaker acoustic response
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

#include <math.h>

#include "sound/speaker_filter.h"

/* This is the minimal acoustic model for the Spectrum's built-in moving-coil
 * speaker: a second-order resonant high-pass. It is applied only to the
 * separate internal-speaker signal, never to the MIC socket output. */

#define SPEAKER_FILTER_PI 3.14159265358979323846
#define SPEAKER_FILTER_DENORMAL_LIMIT 1.0e-20

int
speaker_filter_configure( speaker_filter_t *filter, int sample_rate,
                          double frequency, double q )
{
  double omega, alpha, cosine, a0;

  if( sample_rate <= 0 || frequency <= 0.0 ||
      frequency >= sample_rate / 2.0 || q <= 0.0 )
    return 1;

  omega = 2.0 * SPEAKER_FILTER_PI * frequency / sample_rate;
  alpha = sin( omega ) / ( 2.0 * q );
  cosine = cos( omega );
  a0 = 1.0 + alpha;

  filter->b0 = ( 1.0 + cosine ) / ( 2.0 * a0 );
  filter->b1 = -( 1.0 + cosine ) / a0;
  filter->b2 = filter->b0;
  filter->a1 = -2.0 * cosine / a0;
  filter->a2 = ( 1.0 - alpha ) / a0;
  speaker_filter_reset( filter );

  return 0;
}

void
speaker_filter_reset( speaker_filter_t *filter )
{
  filter->z1 = 0.0;
  filter->z2 = 0.0;
}

double
speaker_filter_apply( speaker_filter_t *filter, double input )
{
  double output = filter->b0 * input + filter->z1;

  filter->z1 = filter->b1 * input - filter->a1 * output + filter->z2;
  filter->z2 = filter->b2 * input - filter->a2 * output;

  /* Very small residual state serves no audible purpose, but can cause a
   * sustained denormal slowdown on processors which do not flush denormals. */
  if( fabs( filter->z1 ) < SPEAKER_FILTER_DENORMAL_LIMIT ) filter->z1 = 0.0;
  if( fabs( filter->z2 ) < SPEAKER_FILTER_DENORMAL_LIMIT ) filter->z2 = 0.0;

  return output;
}
