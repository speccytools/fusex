/* ulaplus.h: ULA+ palette extension
   Copyright (c) 2026 The FuseX authors

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

#ifndef FUSE_ULAPLUS_H
#define FUSE_ULAPLUS_H

#include "libspectrum.h"

#define ULAPLUS_PALETTE_SIZE 64

void ulaplus_register_startup( void );

/* Is the ULA+ palette mode currently switched on? */
int ulaplus_is_enabled( void );

/* The GGGRRRBB colour held in the given palette entry */
libspectrum_byte ulaplus_get_colour( int entry );

#endif                          /* #ifndef FUSE_ULAPLUS_H */
