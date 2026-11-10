/* tape_internals.h: internal tape interfaces
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef FUSE_TAPE_INTERNALS_H
#define FUSE_TAPE_INTERNALS_H

#include "libspectrum.h"

extern libspectrum_tape *tape;
extern int tape_autoplay;
extern int tape_autoplay_blocked;
extern int tape_stop_pending;
extern int trap_resume_pending;
extern int trap_pzx_ready;
int tape_trap_pzx_data( libspectrum_tape_block *block );

int tape_autoload( libspectrum_machine hardware );
int tape_play( int autoplay );
int tape_edge_requests_stop( const libspectrum_tape_edge *edge );
void tape_handle_stop_request( const libspectrum_tape_edge *edge );
int tape_should_stop_for_rom_block( libspectrum_tape_block *block );
void tape_schedule_edge( libspectrum_dword last_tstates,
                         const libspectrum_tape_edge *edge,
                         int from_acceleration );
libspectrum_error tape_trap_finish_rom_block( void );
int tape_trap_load_block( libspectrum_tape_block *block,
                          size_t *bytes_consumed );
void tape_update_microphone( const libspectrum_tape_edge *edge );

void tape_record_init( libspectrum_tape *current_tape );
void tape_record_set_tape( libspectrum_tape *current_tape );
void tape_record_edge( int level );
void tape_record_frame( libspectrum_dword frame_length );

#endif
