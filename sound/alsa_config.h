/* ALSA configuration shared by the backend and its configuration module.
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/
#ifndef FUSE_ALSA_CONFIG_H
#define FUSE_ALSA_CONFIG_H

#include <stdbool.h>
#include <alsa/asoundlib.h>

#include "alsa_pacing.h"

struct alsa_configuration {
  snd_pcm_t *pcm_handle;
  const char *pcm_name;
  int verb, ch, batch;
  unsigned int rate, target;
  snd_pcm_uframes_t exact_periodsize, exact_bsize;
  enum alsa_pacing_mode mode;
};

/* On success, the caller owns pcm_handle. On failure, no PCM is retained.
   pcm_name borrows storage from device or a static default name. */
int alsa_configure( const char *device, int *frequency, int *stereo,
                    bool first_init, struct alsa_configuration *config );

#endif
