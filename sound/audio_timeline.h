/* audio_timeline.h: Internal frame-relative synthesis time conversion
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/
#ifndef FUSE_AUDIO_TIMELINE_H
#define FUSE_AUDIO_TIMELINE_H

#include "libspectrum.h"

/* Source events retain machine-frame timestamps. Blip buffers have already
   consumed the interval preceding the private audio position. Future events
   (including instruction overshoot) are not clamped to the frame boundary. */
libspectrum_dword sound_interval_time( libspectrum_dword frame_time );
libspectrum_dword sound_audio_position( void );

#endif
