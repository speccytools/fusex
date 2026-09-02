/* ula_filter.c: ULA electrical/MIC output approximation
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

#include "sound/ula_filter.h"

/*
 * Effective model of the ZX Spectrum's electrical/MIC audio path.
 *
 * This asymmetric one-pole filter is a deliberately simple perceptual
 * approximation of behaviour measured at the MIC socket, rather than a
 * component-level circuit model.  Its parameters were selected through
 * listening tests against recordings of real machines.
 *
 * Physical speaker response is modelled separately, downstream.
 */

int
ula_filter_configure( ula_filter_t *filter, int sample_rate )
{
  if( sample_rate <= 0 ) return 1;

  filter->alpha_rise = -expm1( -1.0 /
                               ( sample_rate * ULA_FILTER_RISE_TAU ) );
  filter->alpha_fall = -expm1( -1.0 /
                               ( sample_rate * ULA_FILTER_FALL_TAU ) );
  ula_filter_reset( filter );

  return 0;
}

void
ula_filter_reset( ula_filter_t *filter )
{
  /* The listening-test generator starts a fresh stream at its first PCM
   * target. This avoids adding an artificial transition at stream start. */
  filter->state = 0.0;
  filter->initialised = 0;
}

double
ula_filter_apply( ula_filter_t *filter, double input )
{
  double alpha;

  if( !filter->initialised ) {
    filter->state = input;
    filter->initialised = 1;
    return filter->state;
  }

  /* This deliberately compares the post-Blip target with the filtered state,
   * not with the preceding target. */
  alpha = input > filter->state ? filter->alpha_rise : filter->alpha_fall;
  filter->state += alpha * ( input - filter->state );

  return filter->state;
}
