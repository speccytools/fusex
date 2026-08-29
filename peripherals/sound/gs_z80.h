/* gs_z80.h: the General Sound card's Z80
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

#ifndef FUSE_GS_Z80_H
#define FUSE_GS_Z80_H

#include "libspectrum.h"

/* The card's Z80 runs at 24MHz and takes a maskable interrupt at 37.5kHz,
   so one interrupt every 640 of its own T-states. Both figures are fixed
   in the hardware and independent of the host's clock. */
#define GS_Z80_CLOCK_SPEED 24000000
#define GS_Z80_INT_FREQ    37500
#define GS_Z80_INT_PERIOD  ( GS_Z80_CLOCK_SPEED / GS_Z80_INT_FREQ )

/* T-states completed in whole interrupt periods, plus the count within the
   period in progress. The card's absolute time is the sum. */
extern libspectrum_qword gs_z80_period_base;
extern libspectrum_dword gs_z80_tstates;

void gs_z80_reset( void );
void gs_z80_reset_clock( void );
void gs_z80_nmi( void );

/* The card's absolute T-state count. */
libspectrum_qword gs_z80_now( void );

/* Run the card's Z80 until its absolute T-state count reaches target. */
void gs_z80_run( libspectrum_qword target );

#endif			/* #ifndef FUSE_GS_Z80_H */
