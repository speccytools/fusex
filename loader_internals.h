/* loader_internals.h: internal loader detection interfaces
   Copyright (c) 2006-2018 Philip Kendall

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

#ifndef FUSE_LOADER_INTERNALS_H
#define FUSE_LOADER_INTERNALS_H

#include <stddef.h>

#include "libspectrum.h"

#define LOADER_PATTERN_MAX_ALTERNATIVES 4

typedef enum acceleration_mode_t {
  ACCELERATION_MODE_NONE = 0,
  ACCELERATION_MODE_INCREASING,
  ACCELERATION_MODE_DECREASING,
  ACCELERATION_MODE_SOFTWARE_PROJECTS,
  ACCELERATION_MODE_GREMLIN_RISING,
  ACCELERATION_MODE_GREMLIN_FALLING,
} acceleration_mode_t;

typedef struct loader_pattern_byte_t {
  libspectrum_byte values[ LOADER_PATTERN_MAX_ALTERNATIVES ];
  libspectrum_byte masks[ LOADER_PATTERN_MAX_ALTERNATIVES ];
  size_t alternatives;
} loader_pattern_byte_t;

#define LOADER_PATTERN_BYTE( value ) \
  { .values = { value }, .masks = { 0xff }, .alternatives = 1 }
#define LOADER_PATTERN_MASKED( value, mask ) \
  { .values = { value }, .masks = { mask }, .alternatives = 1 }
#define LOADER_PATTERN_ANY { .alternatives = 0 }
#define LOADER_PATTERN_ONE_OF_2( a, b ) \
  { .values = { a, b }, .masks = { 0xff, 0xff }, .alternatives = 2 }
#define LOADER_PATTERN_ONE_OF_3( a, b, c ) \
  { .values = { a, b, c }, .masks = { 0xff, 0xff, 0xff }, \
    .alternatives = 3 }
#define LOADER_PATTERN_ONE_OF_4( a, b, c, d ) \
  { .values = { a, b, c, d }, .masks = { 0xff, 0xff, 0xff, 0xff }, \
    .alternatives = 4 }

extern acceleration_mode_t acceleration_mode;
extern size_t acceleration_pc;

int loader_pattern_matches( libspectrum_word address,
                            const loader_pattern_byte_t *pattern,
                            size_t length );
int loader_word_matches( libspectrum_word address, libspectrum_word value );

acceleration_mode_t acceleration_detector( libspectrum_word pc );
acceleration_mode_t acceleration_detector_at_in( libspectrum_word pc );
int modified_rom_loader_detector( libspectrum_word pc );
int movieload_loader_detector( libspectrum_word pc );
int sign_flag_loader_detector( libspectrum_word pc );
int loader_loop_detector( libspectrum_word pc );
int ula_read_uses_ear( libspectrum_word pc );
int ula_read_is_keyboard_scan( libspectrum_word pc );

void accelerate_loader( int long_pulse );

#endif /* #ifndef FUSE_LOADER_INTERNALS_H */
