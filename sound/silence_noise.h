/* silence_noise.h: balanced one-LSB filler for DAC silence detection
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef FUSE_SILENCE_NOISE_H
#define FUSE_SILENCE_NOISE_H

#include <stdint.h>
#include <libspectrum.h>

typedef struct sound_silence_noise_channel {
  uint32_t rng;
  int pending;
} sound_silence_noise_channel;

typedef struct sound_silence_noise {
  sound_silence_noise_channel channel[2];
} sound_silence_noise;

static inline void
sound_silence_noise_reset( sound_silence_noise *state )
{
  int channel;
  for( channel = 0; channel < 2; channel++ ) {
    state->channel[channel].rng = UINT32_C( 0x12345678 ) + channel;
    state->channel[channel].pending = 0;
  }
}

/* count is interleaved samples, in complete mono/stereo frames. Preserve
   state across batches and nonzero samples: each pair of inserted values
   cancels, with at most one LSB-sample of outstanding imbalance per channel.
   This is a silence workaround, not quantisation dither. */
static inline void
sound_silence_noise_apply( sound_silence_noise *state,
                          libspectrum_signed_word *samples, long count,
                          int channels )
{
  long frame;
  int channel;
  for( frame = 0; frame < count; frame += channels ) {
    for( channel = 0; channel < channels; channel++ ) {
      sound_silence_noise_channel *s = &state->channel[channel];
      int value;
      if( samples[frame + channel] ) continue;
      if( s->pending ) {
        value = s->pending;
        s->pending = 0;
      } else {
        uint32_t x = s->rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s->rng = x;
        value = x & 1 ? 1 : -1;
        s->pending = -value;
      }
      samples[frame + channel] = value;
    }
  }
}

#endif /* FUSE_SILENCE_NOISE_H */
