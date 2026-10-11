/* display_internal.h: Internal interfaces shared by display modules
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef FUSE_DISPLAY_INTERNAL_H
#define FUSE_DISPLAY_INTERNAL_H

#include "libspectrum.h"

#include "display.h"

void display_dirty_init( void );
void display_dirty_frame_begin( void );
void display_dirty_frame_end( void );
void display_dirty_refresh_all( void );
void display_get_beam_position( int *x, int *y );
void display_mark_screen_dirty( int x, int y );
void display_dirty8( libspectrum_word address );
void display_dirty64( libspectrum_word address );

void display_render_init( void );
void display_render_frame( void );

/* A cell drawn in ULA+ mode has this bit set in display_last_screen, with
   the pixel byte in bits 0-7 and the attribute byte in bits 8-15 as usual */
#define DISPLAY_LAST_SCREEN_ULAPLUS 0x02000000

/* The colours a ULA+ cell was drawn with, as ink << 16 | paper, each a colour
   index. Only meaningful where display_last_screen has
   DISPLAY_LAST_SCREEN_ULAPLUS set */
extern libspectrum_dword
display_last_colours[ DISPLAY_SCREEN_WIDTH_COLS * DISPLAY_SCREEN_HEIGHT ];

int display_border_init( void );
void display_border_frame( void );

#endif /* #ifndef FUSE_DISPLAY_INTERNAL_H */
