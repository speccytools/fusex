/* output_mixer.h: ULA and TV routed sound output */

#ifndef FUSE_OUTPUT_MIXER_H
#define FUSE_OUTPUT_MIXER_H

#include "sound/blipbuffer.h"

int output_mixer_init( libspectrum_dword clock_rate, int sample_rate,
                       int frame_size, int channels, int beeper_volume );
void output_mixer_end( void );
void output_mixer_reset( blip_resampled_time_t offset );
int output_mixer_speaker_type( void );
Blip_Buffer *output_mixer_tv_left( void );
Blip_Buffer *output_mixer_tv_right( void );
void output_mixer_route_changed( void );
void output_mixer_advance( libspectrum_dword elapsed,
                           blip_sample_t *main_samples, long count );
void output_mixer_end_frame( void );

#endif
