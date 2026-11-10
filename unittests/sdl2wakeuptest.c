/* sdl2wakeuptest.c: device-free SDL2 producer/callback synchronization test
   Copyright (c) 2026 Fredrick Meunier

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

/* No SDL dylib constructor or audio thread is needed: test the actual backend
   callback concurrently with its producer, using native wait/wake primitives. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
/* SDL is mocked: keep the normal entry point without linking SDL2main. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef SDL2_TEST_THREADS
#include <pthread.h>
#endif
#include "machine.h"
#include "settings.h"
#include "sound/audio_progress.h"
#include "ui/ui.h"
#define CHECK( x ) do { if( !( x ) ) { \
                          fprintf( stderr, "sdl2wakeuptest:%d: %s\n", __LINE__, \
                                   #x ); exit( 1 ); \
                        } } while( 0 )
static SDL_AudioCallback callback;
static unsigned int output_errors, injected_timeouts;
static int wake_before_wait( audio_progress_deadline );
static SDL_AudioDeviceID
mock_open( const char *name, int capture, const SDL_AudioSpec *want,
           SDL_AudioSpec *have, int flags )
{
  (void)name; (void)capture; (void)flags;
  *have = *want; have->samples = 512; callback = want->callback;
  return 1;
}

static int
mock_wait( struct audio_progress *p, uint32_t generation,
           audio_progress_deadline end )
{
  if( injected_timeouts ) {
    injected_timeouts--;
    return ETIMEDOUT;
  }
  if( wake_before_wait( end ) ) return 0;
  return audio_progress_wait( p, generation, end );
}

#define SDL_OpenAudioDevice mock_open
#define SDL_PauseAudioDevice( id, pause ) ( (void)( id ), (void)( pause ) )
#define SDL_CloseAudioDevice( id ) ( (void)( id ) )
#define SDL_LockAudioDevice( id ) ( (void)( id ) )
#define SDL_UnlockAudioDevice( id ) ( (void)( id ) )
#define SDL_WasInit( flags ) ( flags )
#define SDL_InitSubSystem( flags ) ( (void)( flags ), 0 )
#define SDL_QuitSubSystem( flags ) ( (void)( flags ) )
#define SDL_setenv( name, value, overwrite ) \
        ( (void)( name ), (void)( value ), (void)( overwrite ), 0 )
#define SDL_GetError() "mock SDL error"
#define audio_progress_wait mock_wait
#include "sound/sdl2sound.c"
#undef audio_progress_wait
static fuse_machine_info test_machine;
fuse_machine_info *machine_current = &test_machine;
settings_info settings_current;
libspectrum_dword
sound_get_effective_processor_speed( void )
{
  return 3500000;
}

int
sound_normal_producer_context( void )
{
  return 1;
}

int
ui_error( ui_error_level level, const char *message, ... )
{
  (void)level; (void)message; output_errors++; return 0;
}

int
ui_error_console( ui_error_level level, const char *message, ... )
{
  (void)level; (void)message; output_errors++; return 0;
}

static void
wake_init( void )
{
}

static void
wake_consume( unsigned int frames )
{
  Uint8 output[1024];
  CHECK( frames <= 512 ); callback( NULL, output, frames * 2 );
}

#ifdef SDL2_TEST_THREADS
#define AUDIO_WAKE_THREADS
#endif
#include "audio-wakeup-cases.h"
static void
stalled_consumer( void )
{
  libspectrum_signed_word sample = 123;
  unsigned int errors = output_errors;

  /* Native 10 ms timeouts are not themselves fatal. */
  wake_fixture( 2047, 2047 );
  wake_deadline_valid = -1;
  injected_timeouts = 3;
  wake_steps[0] = 1; wake_count = 1;
  settings_current.sound = 1;
  sound_lowlevel_frame( &sample, 1 );
  CHECK( !audio_failed && settings_current.sound && output_errors == errors );
  sound_lowlevel_end();

  /* No callback at all: admission must return without granting capacity or
     closing the device. Subsequent production is inert, even if re-enabled. */
  wake_fixture( 2047, 2047 );
  wake_deadline_valid = -1;
  settings_current.sound = 1;
  compat_wait_deadline deadline = compat_wait_until( 4000 );
  CHECK( !sound_lowlevel_reserve( 240 ) );
  CHECK( !compat_wait_expired( deadline ) );
  CHECK( audio_failed && !settings_current.sound );
  CHECK( output_errors == ++errors && audio_device );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 2047 );
  settings_current.sound = 1;
  CHECK( !sound_lowlevel_reserve( 240 ) );
  sound_lowlevel_frame( &sample, 1 );
  CHECK( output_errors == errors );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 2047 );
  /* A late callback still has valid storage. */
  wake_consume( 512 );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 1535 );
  sound_lowlevel_end();
  CHECK( audio_device && sound_fifo.buffer );
  /* Mock device is quiescent; permit test-only cleanup. Real stalled devices
     remain retained, and cannot be reopened safely. */
  audio_failed = 0;
  sound_lowlevel_end();

  /* The defensive partial-write path needs the same watchdog. */
  wake_fixture( 2047, 2047 );
  CHECK( !audio_failed );
  wake_deadline_valid = -1;
  settings_current.sound = 1;
  deadline = compat_wait_until( 4000 );
  sound_lowlevel_frame( &sample, 1 );
  CHECK( !compat_wait_expired( deadline ) );
  CHECK( audio_failed && !settings_current.sound );
  CHECK( output_errors == ++errors );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 2047 );
  sound_lowlevel_end();
  CHECK( audio_device && sound_fifo.buffer );
  audio_failed = 0;
  sound_lowlevel_end();
  wake_fixture( 0, 2047 );
  CHECK( !audio_failed );
  wake_publish( 240 );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 240 );
  sound_lowlevel_end();
}

#ifdef main
/* SDL headers redefine main on Windows, but this test needs a normal entry point. */
#undef main
#endif

int
main( void )
{
  test_machine.timings.tstates_per_frame = 70000;
  wake_regressions();
  stalled_consumer();
#ifdef AUDIO_WAKE_THREADS
  wake_threaded();
#endif
  puts( "SDL2 native wakeup tests passed" );
  return 0;
}
