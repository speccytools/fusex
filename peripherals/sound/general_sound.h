/* general_sound.h: Routines for handling the General Sound card
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

#ifndef FUSE_GENERAL_SOUND_H
#define FUSE_GENERAL_SOUND_H

#include "libspectrum.h"

void general_sound_register_startup( void );

/* Run the card to the end of the host frame. Called from spectrum_frame
   before the host rebases its T-state count. */
void general_sound_frame( libspectrum_dword frame_length );

/* Called when the host restarts its T-state count outside a frame. */
void general_sound_reanchor( void );

/* The card's four 16K banks as currently paged, for the GS Z80's memory
   macros. A NULL write pointer marks the bank read-only. */
extern libspectrum_byte *general_sound_bank_read[4];
extern libspectrum_byte *general_sound_bank_write[4];

/* The card's own I/O map, as seen by its Z80 (not the host's ports). */
libspectrum_byte general_sound_port_read( libspectrum_word port );
void general_sound_port_write( libspectrum_word port, libspectrum_byte b );

#endif			/* #ifndef FUSE_GENERAL_SOUND_H */
