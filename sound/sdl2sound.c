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
#include "pcm_fifo.h"
#include "audio_pacing.h"
#include "audio_progress.h"
#include "sound.h"
#include "ui/ui.h"

static void sdl2write( void *userdata, Uint8 *stream, int len );

sfifo_t sound_fifo;

/* Number of Spectrum frames audio latency to use */
#define NUM_FRAMES 2

static SDL_AudioDeviceID audio_device;
static int audio_output_started;
static int audio_failed;
static struct audio_pacing pacing;
static struct audio_progress progress;
_Static_assert( SFIFO_MAX_BUFFER_SIZE < UINT32_MAX - 2,
                "FIFO must bound generation wrap during a producer wait" );
static int progress_initialized;
/* Immutable while the device is open; obtained callback PCM geometry. */
static unsigned int audio_channels, bytes_per_frame;

int
sound_lowlevel_init( const char *device, int *freqptr, int *stereoptr )
{
  SDL_AudioSpec requested, received;
  int error;
  float hz;
  int sound_framesiz;

  if( audio_device || sound_fifo.buffer ) {
    ui_error( UI_ERROR_ERROR, "Previous SDL audio output has not been closed" );
    return 1;
  }

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

  /* received describes callback PCM, not necessarily the hardware format.
     SDL may convert internally, but Fuse supplies only native signed 16-bit
     interleaved mono or stereo. Do not infer stereo from arbitrary channels. */
  if( received.format != AUDIO_S16SYS ||
      ( received.channels != 1 && received.channels != 2 ) ||
      received.freq <= 0 || !received.samples ) {
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "Unsupported SDL audio PCM specification" );
    return 1;
  }
  audio_channels = received.channels;
  bytes_per_frame = audio_channels * sizeof( libspectrum_signed_word );
  *freqptr = received.freq;
  *stereoptr = audio_channels == 2;

  sound_framesiz = *freqptr / hz;

  /* Preserve the historical byte allocation (including its extra byte) and
     sfifo rounding: physical capacity remains the buffering limit. */
  if( ( error = sfifo_init( &sound_fifo, NUM_FRAMES
                            * bytes_per_frame * sound_framesiz + 1 ) ) ) {
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
    ui_error( UI_ERROR_ERROR, "Problem initialising sound fifo: %s",
              strerror( -error ) );
    return 1;
  }

  /* SDL's obtained samples bound the fixed callback client request. A later
     larger request invalidates qualification rather than silently learning D. */
  if( audio_pacing_init_rate( &pacing, &sound_fifo, bytes_per_frame,
        *freqptr, sound_get_effective_processor_speed(),
        machine_current->timings.tstates_per_frame, received.samples ) ) {
    sound_lowlevel_end();
    ui_error( UI_ERROR_ERROR, "Cannot qualify SDL audio pacing geometry" );
    return 1;
  }
  if( audio_progress_init( &progress ) ) {
    sound_lowlevel_end();
    ui_error( UI_ERROR_ERROR, "Cannot initialize SDL audio progress wake" );
    return 1;
  }
  progress_initialized = 1;
  audio_output_started = 0;
  audio_failed = 0;

  return 0;
}

void
sound_lowlevel_end( void )
{
  /* SDL provides no bounded close for a wedged callback. Retain the device
     and its storage rather than deadlocking pause/menu handling or freeing
     memory a late callback can still access. Reinit will refuse this device. */
  if( audio_failed ) return;
  if( audio_device ) {
    SDL_PauseAudioDevice( audio_device, 1 );
    SDL_LockAudioDevice( audio_device );
    SDL_UnlockAudioDevice( audio_device );
    SDL_CloseAudioDevice( audio_device );
    audio_device = 0;
  }

  if( SDL_WasInit( SDL_INIT_AUDIO ) ) SDL_QuitSubSystem( SDL_INIT_AUDIO );

  /* Lifecycle and production share the emulator thread. Close/join the
     callback before destroying its notification or FIFO; no waiter remains. */
  if( progress_initialized ) {
    if( atomic_load( &progress.error ) )
      ui_error( UI_ERROR_ERROR, "SDL audio progress wake: %s",
                strerror( atomic_load( &progress.error ) ) );
    audio_progress_close( &progress );
    progress_initialized = 0;
  }
  sfifo_close( &sound_fifo );
  audio_output_started = 0;
  audio_channels = bytes_per_frame = 0;
  pacing.ready = false;
}

/* A bounded native park alone cannot detect a dead consumer: its caller
   would simply retry forever. Watch actual FIFO consumption, not notifications
   (which can be spurious), across those retries. Two seconds is a failure
   guard, not a pacing target. Leave the device and callback storage intact:
   closing a wedged SDL device here could block on the very same audio lock. */
static int
sdl2_wait_for_space( unsigned int frames, int admission )
{
  compat_wait_deadline end = compat_wait_until( 2000 );
  int previous = pcm_fifo_producer_space( &sound_fifo, bytes_per_frame );
  for(;;) {
    int available = admission ?
      audio_pacing_can_admit( &pacing, &sound_fifo, bytes_per_frame, frames ) :
      pcm_fifo_producer_space( &sound_fifo, bytes_per_frame );
    int space, error;
    if( available < 0 ) return available;
    if( admission ? available : (unsigned int)available >= frames ) return 0;
    space = pcm_fifo_producer_space( &sound_fifo, bytes_per_frame );
    if( space < 0 ) return space;
    if( space > previous ) end = compat_wait_until( 2000 );
    previous = space;
    if( compat_wait_expired( end ) ) return -ETIMEDOUT;
    error = audio_pacing_wait( &pacing, &progress, &sound_fifo,
                               bytes_per_frame, frames, admission );
    if( error ) return error;
  }
}

static void
sdl2_output_failed( int error )
{
  if( audio_failed ) return;
  audio_failed = 1;
  /* Keep synthesis/recording alive and restore wall-clock timer pacing. */
  settings_current.sound = 0;
  /* A modal warning pauses emulation and would try to close SDL audio. */
  ui_error_console( UI_ERROR_WARNING, "SDL audio output disabled: %s",
                    strerror( -error ) );
}

int
sound_lowlevel_reserve( unsigned int frames )
{
  int error;
  if( audio_failed ) return 0;
  error = audio_pacing_pending( &pacing, &sound_fifo, bytes_per_frame, frames,
                                sound_normal_producer_context(),
                                audio_output_started );
  if( error ) return error;
  error = sdl2_wait_for_space( frames, 1 );
  if( error == -ETIMEDOUT ) {
    sdl2_output_failed( error );
    return 0;
  }
  if( error ) return error;
  if( frames && pacing.ready ) pacing.reserved = true;
  return 0;
}

void
sound_lowlevel_frame( libspectrum_signed_word *data, int len )
{
  int i = 0;
  const char *bytes = (const char *)data;

  if( audio_failed ) return;
  if( len < 0 || !audio_channels || len % audio_channels ) {
    ui_error( UI_ERROR_ERROR, "Invalid SDL audio sample count" );
    return;
  }
  len /= audio_channels;

  while( len ) {
    if( ( i = audio_pacing_write( &pacing, &sound_fifo, bytes_per_frame,
                                   bytes, len, audio_output_started ) ) < 0 ) {
      break;
    } else if( !i ) {
      int error = sdl2_wait_for_space( 1, 0 );
      if( error ) {
        i = error;
        break;
      }
    }

    bytes += i * bytes_per_frame;
    len -= i;
  }

  if( i < 0 ) {
    sdl2_output_failed( i );
    return;
  }

  if( !audio_output_started && audio_device ) {
    SDL_PauseAudioDevice( audio_device, 0 );
    audio_output_started = 1;
  }
}

static void
sdl2write( void *userdata GCC_UNUSED, Uint8 *stream, int len )
{
  int delivered;

  if( len <= 0 || !stream ) return;
  audio_pacing_callback_begin( &pacing );
  /* SDL promises writable len-byte storage. A malformed frame request must
     neither consume queued PCM nor leave stale bytes in that storage. */
  if( !bytes_per_frame || len % bytes_per_frame ) {
    memset( stream, 0, len );
    audio_pacing_callback_finish( &pacing, &progress, 0, 0, true );
    return;
  }

  delivered = pcm_fifo_read( &sound_fifo, bytes_per_frame, stream,
                             len / bytes_per_frame );
  if( delivered < 0 ) delivered = 0;
  memset( stream + delivered * bytes_per_frame, 0,
          len - delivered * bytes_per_frame );
  audio_pacing_callback_finish( &pacing, &progress, len / bytes_per_frame,
                                delivered, false );
}
