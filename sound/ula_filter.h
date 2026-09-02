/* ula_filter.h: ULA electrical/MIC output approximation
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

#ifndef FUSE_ULA_FILTER_H
#define FUSE_ULA_FILTER_H

/* Frozen listening-test candidate parameters; do not retune. */
#define ULA_FILTER_RISE_TAU 36.53558495933805e-6
#define ULA_FILTER_FALL_TAU 68.86073172982108e-6

typedef struct ula_filter_tag {
  double alpha_rise, alpha_fall;
  double state;
  int initialised;
} ula_filter_t;

int ula_filter_configure( ula_filter_t *filter, int sample_rate );
void ula_filter_reset( ula_filter_t *filter );
double ula_filter_apply( ula_filter_t *filter, double input );

#endif                  /* #ifndef FUSE_ULA_FILTER_H */
