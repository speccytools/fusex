/* sdl2soundtest.c: exercise the SDL2 backend with mocked device lifecycle

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#ifdef SDL2_TEST_THREADS
#include <pthread.h>
#endif
#include "machine.h"
#include "settings.h"
#include "ui/ui.h"
#include "sound/audio_progress.h"

#define CHECK(x) do { if( !(x) ) { \
  fprintf( stderr, "sdl2soundtest:%d: %s\n", __LINE__, #x ); exit( 1 ); \
} } while( 0 )

static int paused, live, starts, closes, dummy;
static Uint8 channels;
static SDL_AudioFormat format;
static SDL_AudioCallback callback;
static int release_bytes, waits;
static Uint16 demand_override;
static int wake_before_wait( audio_progress_deadline );
static int
mock_progress_wait( struct audio_progress *p, uint32_t expected,
                    audio_progress_deadline end )
{
  Uint8 output[4096];
  if( wake_before_wait( end ) ) return 0;
  if( release_bytes ) {
    waits++;
    callback( NULL, output, release_bytes );
    release_bytes = 0;
  }
  return audio_progress_wait( p, expected, end );
}
static SDL_AudioDeviceID
mock_open( const char *name, int capture, const SDL_AudioSpec *want,
           SDL_AudioSpec *have, int flags )
{
  (void)name; (void)capture;
  CHECK( flags == ( SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                    SDL_AUDIO_ALLOW_CHANNELS_CHANGE ) );
  *have = *want;
  have->channels = channels;
  have->format = format;
  if( demand_override ) have->samples = demand_override;
  callback = want->callback;
  paused = live = 1;
  if( dummy ) return SDL_OpenAudioDevice( name, capture, want, have, flags );
  return 1;
}
static void mock_pause( SDL_AudioDeviceID id, int pause )
{
  CHECK( live ); paused = pause; if( !pause ) starts++;
  if( dummy ) SDL_PauseAudioDevice( id, pause );
}
static void mock_close( SDL_AudioDeviceID id )
{
  CHECK( live && paused ); live = 0; closes++;
  if( dummy ) SDL_CloseAudioDevice( id );
}
static void mock_lock( SDL_AudioDeviceID id )
{ CHECK( paused ); if( dummy ) SDL_LockAudioDevice( id ); }
static void mock_unlock( SDL_AudioDeviceID id )
{ CHECK( paused ); if( dummy ) SDL_UnlockAudioDevice( id ); }
#define audio_progress_wait mock_progress_wait
#define SDL_OpenAudioDevice mock_open
#define SDL_PauseAudioDevice mock_pause
#define SDL_CloseAudioDevice mock_close
#define SDL_LockAudioDevice mock_lock
#define SDL_UnlockAudioDevice mock_unlock
#include "sound/sdl2sound.c"
#undef audio_progress_wait

static fuse_machine_info test_machine;
fuse_machine_info *machine_current = &test_machine;
settings_info settings_current;
libspectrum_dword sound_get_effective_processor_speed( void )
{ return 3500000; }
int sound_normal_producer_context( void ) { return 1; }
int ui_error( ui_error_level level, const char *message, ... )
{ (void)level; (void)message; return 0; }

int ui_error_console( ui_error_level level, const char *message, ... )
{ (void)level; (void)message; return 0; }

static void
transfers( unsigned int width )
{
  unsigned char input[128], output[160];
  int n, request, i, capacity;
  /* Small non-frame-aligned byte ring forces wrapping inside PCM frames. */
  CHECK( !audio_progress_init( &progress ) );
  CHECK( !sfifo_init( &sound_fifo, 31 ) );
  bytes_per_frame = width;
  capacity = pcm_fifo_capacity( &sound_fifo, width );
  for( i = 0; i < 128; i++ ) input[i] = i + 1;
  for( n = 0; n <= capacity; n++ ) {
    for( request = 1; request <= capacity + 2; request++ ) {
      int delivered = n < request ? n : request;
      CHECK( pcm_fifo_write( &sound_fifo, width, input, n ) == n );
      memset( output, 0xa5, sizeof( output ) );
      callback( NULL, output, request * width );
      CHECK( !memcmp( output, input, delivered * width ) );
      for( i = delivered * width; i < request * width; i++ )
        CHECK( output[i] == 0 );
      CHECK( output[request * width] == 0xa5 );
      /* Drain leftovers before the next case, retaining advancing ring indices. */
      callback( NULL, output, capacity * width );
    }
  }
  CHECK( pcm_fifo_write( &sound_fifo, width, input, capacity + 1 ) == capacity );
  CHECK( pcm_fifo_producer_space( &sound_fifo, width ) == 0 );
  CHECK( !sound_lowlevel_reserve( 0 ) );
  CHECK( sound_lowlevel_reserve( capacity + 1 ) == -EINVAL );
  release_bytes = 2 * width;
  waits = 0;
  CHECK( !sound_lowlevel_reserve( 2 ) && waits == 1 );
  CHECK( pcm_fifo_write( &sound_fifo, width, input, 2 ) == 2 );
  callback( NULL, output, capacity * width );
  CHECK( pcm_fifo_write( &sound_fifo, width, input, capacity ) == capacity );
  memset( output, 0xa5, sizeof( output ) );
  callback( NULL, output, width + 1 );
  for( i = 0; i < width + 1; i++ ) CHECK( output[i] == 0 );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, width ) == capacity );
  callback( NULL, output, ( capacity + 1 ) * width );
  CHECK( !memcmp( output, input, capacity * width ) );
  sfifo_close( &sound_fifo );
  audio_progress_close( &progress );
}

static void
pacing_tests( void )
{
  int freq = 48000, stereo = 0;
  unsigned int i;
  libspectrum_signed_word samples[2048] = { 0 }, output[512];
  channels = 1; format = AUDIO_S16SYS; demand_override = 512;
  CHECK( !sound_lowlevel_init( NULL, &freq, &stereo ) );
  CHECK( pacing.controller.target == 2047 && pacing.demand == 512 );
  CHECK( !sound_lowlevel_reserve( 960 ) );
  sound_lowlevel_frame( samples, 960 );
  for( i = 0; i < 20000; i++ ) {
    CHECK( !audio_pacing_pending( &pacing, &sound_fifo, 2, 240, true, true ) );
    if( !audio_pacing_can_admit( &pacing, &sound_fifo, 2, 240 ) )
      callback( NULL, (Uint8 *)output, sizeof( output ) );
    CHECK( !sound_lowlevel_reserve( 240 ) );
    sound_lowlevel_frame( samples, 240 );
    if( pacing.controller.accepted == 1328 && !pacing.controller.candidate ) break;
  }
  CHECK( i < 20000 && pacing.controller.funded && !pacing.controller.excess );
  sfifo_flush( &sound_fifo );
  CHECK( pcm_fifo_write( &sound_fifo, 2, samples, 1261 ) == 1261 );
  callback( NULL, (Uint8 *)output, sizeof( output ) );
  callback( NULL, (Uint8 *)output, sizeof( output ) );
  CHECK( !sound_lowlevel_reserve( 238 ) );
  sound_lowlevel_frame( samples, 238 );
  CHECK( pacing.controller.target == 1667 && pacing.controller.excess == 339 );
  CHECK( pacing.controller.floor == 915 );
  CHECK( !sound_lowlevel_reserve( 240 ) );
  sound_lowlevel_frame( samples, 240 );
  CHECK( pacing.controller.state == ADAPTIVE_HOLD );
  CHECK( pacing.controller.target == 2047 && pacing.controller.excess == 339 );
  CHECK( !atomic_load( &pacing.missing ) );
  sound_lowlevel_end();
  CHECK( !pacing.ready ); demand_override = 0;
}

static void wake_init( void )
{ channels = 1; format = AUDIO_S16SYS; demand_override = 512; }
static void wake_consume( unsigned int frames )
{
  Uint8 output[1024];
  CHECK( frames <= 512 );
  callback( NULL, output, frames * 2 );
}
#ifdef SDL2_TEST_THREADS
#define AUDIO_WAKE_THREADS
#endif
#include "audio-wakeup-cases.h"

#ifdef main
/* SDL headers redefine main on Windows, but this test needs a normal entry point. */
#undef main
#endif

int main( int argc, char **argv )
{
  int c, freq, stereo, old_starts;
  (void)argv;
  test_machine.timings.tstates_per_frame = 70000;
  if( argc > 1 ) {
    libspectrum_signed_word samples[16] = { 0 };
    dummy = 1; freq = 48000; stereo = 1;
    CHECK( !sound_lowlevel_init( "dummy", &freq, &stereo ) );
    CHECK( paused && sound_fifo.buffer );
    sound_lowlevel_frame( samples, 16 );
    SDL_Delay( 50 );
    sound_lowlevel_end();
    CHECK( !sound_fifo.buffer );
    puts( "SDL dummy audio smoke passed" );
    return 0;
  }
  format = AUDIO_S16SYS;
  for( c = 1; c <= 2; c++ ) {
    channels = c; freq = 48000; stereo = !c;
    CHECK( !sound_lowlevel_init( NULL, &freq, &stereo ) );
    CHECK( paused && !audio_output_started && sound_fifo.buffer );
    CHECK( stereo == ( c == 2 ) && bytes_per_frame == c * 2 );
    CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
    old_starts = starts;
    {
      libspectrum_signed_word samples[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
      Uint8 output[16];
      CHECK( !sound_lowlevel_reserve( 8 / c ) );
      CHECK( paused && starts == old_starts );
      sound_lowlevel_frame( samples, 8 );
      CHECK( starts == old_starts + 1 && !paused );
      callback( NULL, output, sizeof( output ) );
      CHECK( !memcmp( output, samples, sizeof( output ) ) );
      sound_lowlevel_frame( samples, 8 );
      CHECK( starts == old_starts + 1 );
      if( c == 2 ) {
        int used = pcm_fifo_consumer_used( &sound_fifo, bytes_per_frame );
        sound_lowlevel_frame( samples, 3 );
        CHECK( pcm_fifo_consumer_used( &sound_fifo, bytes_per_frame ) == used );
      }
    }
    sound_lowlevel_end();
    CHECK( !live && !sound_fifo.buffer );
    transfers( c * 2 );
  }
  for( c = 0; c <= 4; c++ ) {
    if( c == 1 || c == 2 ) continue;
    channels = c; freq = 48000; stereo = 0;
    CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
    CHECK( !live && !sound_fifo.buffer );
  }
  channels = 2; format = AUDIO_F32SYS; freq = 48000; stereo = 1;
  CHECK( sound_lowlevel_init( NULL, &freq, &stereo ) == 1 );
  CHECK( !live && !sound_fifo.buffer );
  sound_lowlevel_end();
  CHECK( closes == 6 );
  pacing_tests();
  wake_regressions();
#ifdef AUDIO_WAKE_THREADS
  wake_threaded();
#endif
  puts( "SDL2 audio tests passed" );
  return 0;
}
