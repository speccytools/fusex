/* ay_engine.h: AY sound generation */

#ifndef FUSE_AY_ENGINE_H
#define FUSE_AY_ENGINE_H

#include "libspectrum.h"
#include "sound/blipbuffer.h"

int ay_engine_init( int volume, int stereo, int output_stereo );
void ay_engine_end( void );
void ay_engine_set_outputs( Blip_Buffer *left, Blip_Buffer *right, int stereo );
/* Ordered frame-relative endpoints; end_frame rebases the continuous lattice. */
void ay_engine_render( libspectrum_dword endpoint );
void ay_engine_end_frame( void );
void ay_engine_write( int reg, int val, libspectrum_dword now );
void ay_engine_reset( void );

/* Test-support accessors used by the regression tests in
 * unittests/unittests.c. */
libspectrum_dword ay_engine_next_tick_offset( void );
libspectrum_byte ay_engine_register_value( int reg );

#endif
