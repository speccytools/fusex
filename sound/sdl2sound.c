/* sdl2sound.c: SDL 2 sound I/O
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 */

#include "config.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <SDL.h>

#include "settings.h"
#include "sfifo.h"
#include "sound.h"
#include "ui/ui.h"

static void sdl2write( void *userdata, Uint8 *stream, int len );

sfifo_t sound_fifo;

/* Number of Spectrum frames audio latency to use */
#define NUM_FRAMES 2

static SDL_AudioDeviceID audio_device;
static int audio_output_started;

int
sound_lowlevel_init( const char *device, int *freqptr, int *stereoptr )
{
  SDL_AudioSpec requested, received;
  int error;
  float hz;
  int sound_framesiz;

  if( device ) {
    error = SDL_setenv( "SDL_AUDIODRIVER", device, 1 );
    if( error ) {
      settings_current.sound = 0;
      ui_error( UI_ERROR_ERROR, "Couldn't set SDL_AUDIODRIVER: %s",
                SDL_GetError() );
      return 1;
    }
  }

  if( !SDL_WasInit( SDL_INIT_AUDIO ) && SDL_InitSubSystem( SDL_INIT_AUDIO ) ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "Couldn't initialize SDL audio: %s",
              SDL_GetError() );
    return 1;
  }

  memset( &requested, 0, sizeof( requested ) );

  requested.freq = *freqptr;
  requested.channels = *stereoptr ? 2 : 1;
  requested.format = AUDIO_S16SYS;
  requested.callback = sdl2write;

  hz = (float)sound_get_effective_processor_speed() /
       machine_current->timings.tstates_per_frame;
  if( hz > 100.0 ) hz = 100.0;
  sound_framesiz = *freqptr / hz;
#ifdef __FreeBSD__
  requested.samples = pow( 2.0, floor( log2( sound_framesiz ) ) );
#else
  requested.samples = sound_framesiz;
#endif

  audio_device = SDL_OpenAudioDevice( NULL, 0, &requested, &received,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                      SDL_AUDIO_ALLOW_CHANNELS_CHANGE );
  if( !audio_device ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "Couldn't open sound device: %s",
              SDL_GetError() );
    return 1;
  }

  *freqptr = received.freq;
  *stereoptr = received.channels == 1 ? 0 : 1;

  sound_framesiz = *freqptr / hz;
  sound_framesiz <<= 1;

  if( ( error = sfifo_init( &sound_fifo, NUM_FRAMES
                            * received.channels
                            * sound_framesiz + 1 ) ) ) {
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
    ui_error( UI_ERROR_ERROR, "Problem initialising sound fifo: %s",
              strerror( -error ) );
    return 1;
  }

  audio_output_started = 0;

  return 0;
}

void
sound_lowlevel_end( void )
{
  if( audio_device ) {
    SDL_PauseAudioDevice( audio_device, 1 );
    SDL_LockAudioDevice( audio_device );
    SDL_UnlockAudioDevice( audio_device );
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
  }

  if( SDL_WasInit( SDL_INIT_AUDIO ) ) SDL_QuitSubSystem( SDL_INIT_AUDIO );

  sfifo_flush( &sound_fifo );
  sfifo_close( &sound_fifo );
}

void
sound_lowlevel_frame( libspectrum_signed_word *data, int len )
{
  int i = 0;
  libspectrum_signed_byte *bytes = (libspectrum_signed_byte *)data;

  len <<= 1;

  while( len ) {
    if( ( i = sfifo_write( &sound_fifo, bytes, len ) ) < 0 ) {
      break;
    } else if( !i ) {
      SDL_Delay( 10 );
    }

    bytes += i;
    len -= i;
  }

  if( i < 0 ) {
    ui_error( UI_ERROR_ERROR, "Couldn't write sound fifo: %s",
              strerror( -i ) );
  }

  if( !audio_output_started && audio_device ) {
    SDL_PauseAudioDevice( audio_device, 0 );
    audio_output_started = 1;
  }
}

#ifndef MIN
#define MIN( a, b ) ( ( ( a ) < ( b ) ) ? ( a ) : ( b ) )
#endif

static void
sdl2write( void *userdata GCC_UNUSED, Uint8 *stream, int len )
{
  int f;

  len = MIN( len, sfifo_used( &sound_fifo ) );
  len &= sound_stereo_ay ? 0xfffc : 0xfffe;

  while( ( f = sfifo_read( &sound_fifo, stream, len ) ) > 0 ) {
    stream += f;
    len -= f;
  }
}
