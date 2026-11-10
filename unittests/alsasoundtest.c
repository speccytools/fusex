/* alsasoundtest.c: actual ALSA backend with deterministic transport faults
   Copyright (c) 2026 Fredrick Meunier

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
#include <alsa/asoundlib.h>
#include "machine.h"
#include "settings.h"
#include "ui/ui.h"

#define CHECK( x ) do { if( !( x ) ) { \
                          fprintf( stderr, "alsasoundtest:%d: %s\n", __LINE__, \
                                   #x ); exit( 1 ); \
                        } } while( 0 )

static unsigned int queued, starts, prepares, waits, errors, captured;
static long long clock_us;
static snd_pcm_state_t transport_state;
static int script[16], script_count, script_index, context = 1, resume_error;
static libspectrum_signed_word capture[8192];
/* Fail each configuration stage after acquiring a real null PCM. */
enum config_fault { CONFIG_OK, CONFIG_OPEN, CONFIG_FORMAT, CONFIG_HARDWARE,
                    CONFIG_SOFTWARE, CONFIG_QUERY };
static enum config_fault config_fault;
static unsigned int closes;
static int fallback;

static int
mock_open( snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream,
           int mode )
{
  if( config_fault == CONFIG_OPEN ) return -ENODEV;
  if( fallback && !strcmp( name, "default" ) ) return -ENODEV;
  if( fallback && !strcmp( name, "plughw:0,0" ) ) name = "null";
  return snd_pcm_open( pcm, name, stream, mode );
}

static int
mock_close( snd_pcm_t *pcm )
{
  closes++;
  return snd_pcm_close( pcm );
}

static int
mock_hw_any( snd_pcm_t *pcm, snd_pcm_hw_params_t *params )
{
  return config_fault == CONFIG_FORMAT ? -EINVAL :
         snd_pcm_hw_params_any( pcm, params );
}

static int
mock_hw_install( snd_pcm_t *pcm, snd_pcm_hw_params_t *params )
{
  return config_fault == CONFIG_HARDWARE ? -EINVAL :
         snd_pcm_hw_params( pcm, params );
}

static int
mock_sw_current( snd_pcm_t *pcm, snd_pcm_sw_params_t *params )
{
  return config_fault == CONFIG_SOFTWARE ? -EINVAL :
         snd_pcm_sw_params_current( pcm, params );
}

static int
mock_hw_current( snd_pcm_t *pcm, snd_pcm_hw_params_t *params )
{
  return config_fault == CONFIG_QUERY ? -EINVAL :
         snd_pcm_hw_params_current( pcm, params );
}
static int mock_status( snd_pcm_t *, snd_pcm_status_t * );
static snd_pcm_state_t mock_state( const snd_pcm_status_t * );
static snd_pcm_uframes_t mock_avail( const snd_pcm_status_t * );
static snd_pcm_sframes_t mock_write( snd_pcm_t *, const void *,
                                     snd_pcm_uframes_t );
static int mock_wait( snd_pcm_t *, int );
static int
mock_start( snd_pcm_t *p )
{
  (void)p; transport_state = SND_PCM_STATE_RUNNING; starts++; return 0;
}

static int
mock_prepare( snd_pcm_t *p )
{
  queued = 0; transport_state = SND_PCM_STATE_PREPARED; prepares++;
  return snd_pcm_prepare( p );
}

static int
mock_resume( snd_pcm_t *p )
{
  (void)p;
  if( !resume_error ) transport_state = SND_PCM_STATE_RUNNING;
  return resume_error;
}

#define snd_pcm_open mock_open
#define snd_pcm_close mock_close
#define snd_pcm_hw_params_any mock_hw_any
#define snd_pcm_hw_params mock_hw_install
#define snd_pcm_sw_params_current mock_sw_current
#define snd_pcm_hw_params_current mock_hw_current
#define snd_pcm_status mock_status
#define snd_pcm_status_get_state mock_state
#define snd_pcm_status_get_avail mock_avail
#define snd_pcm_status_get_avail_max mock_avail
#define snd_pcm_writei mock_write
#define snd_pcm_wait mock_wait
#define snd_pcm_start mock_start
#define snd_pcm_prepare mock_prepare
#define snd_pcm_resume mock_resume
#include "sound/alsa_config.c"
#include "sound/alsasound.c"

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
  return context;
}

int
ui_error( ui_error_level level, const char *message, ... )
{
  (void)level; (void)message; errors++; return 0;
}

long long
compat_monotonic_time_us( void )
{
  return clock_us;
}

void
compat_timer_sleep( int ms )
{
  clock_us += ms * 1000;
}

static int
mock_status( snd_pcm_t *p, snd_pcm_status_t *s )
{
  (void)p; (void)s; return 0;
}

static snd_pcm_state_t
mock_state( const snd_pcm_status_t *s )
{
  (void)s; return transport_state;
}

static snd_pcm_uframes_t
mock_avail( const snd_pcm_status_t *s )
{
  (void)s; return exact_bsize - queued;
}

static snd_pcm_sframes_t
mock_write( snd_pcm_t *p, const void *data, snd_pcm_uframes_t frames )
{
  int result;
  (void)p;
  if( script_index < script_count ) {
    result = script[script_index++];
    if( result <= 0 ) {
      if( result == -EPIPE ) transport_state = SND_PCM_STATE_XRUN;
      if( result == -ESTRPIPE ) transport_state = SND_PCM_STATE_SUSPENDED;
      return result;
    }
    if( (snd_pcm_uframes_t)result < frames ) frames = result;
  }
  CHECK( queued + frames <= exact_bsize );
  CHECK( captured + frames * ch <= 8192 );
  memcpy( capture + captured, data, frames * ch * sizeof( *capture ) );
  captured += frames * ch;
  queued += frames;
  return frames;
}

static int
mock_wait( snd_pcm_t *p, int timeout )
{
  (void)p;
  CHECK( timeout == 10 );
  CHECK( transport_state == SND_PCM_STATE_RUNNING );
  CHECK( waiting_configured );
  queued = queued > exact_periodsize ? queued - exact_periodsize : 0;
  waits++;
  clock_us += (long long)exact_periodsize * 1000000 / 44100;
  return 1;
}

static void
init( const char *device )
{
  int rate = 44100, stereo = 1;
  sound_lowlevel_end();
  queued = starts = prepares = waits = captured = errors = 0;
  script_count = script_index = 0;
  context = 1;
  transport_state = SND_PCM_STATE_PREPARED;
  settings_current.sound = 1;
  CHECK( !sound_lowlevel_init( device, &rate, &stereo ) );
  CHECK( rate == 44100 && stereo == 1 );
  CHECK( pacing.controller.capacity == exact_bsize );
}

static void
test_configuration( void )
{
  int rate, stereo;
  unsigned int closed;

  init( "buffer=0,frames=0,avail=0,target=0,null" );
  CHECK( exact_periodsize <= pacing.controller.allowance );
  CHECK( pacing.mode == ALSA_PACING_CAPACITY );
  init( "frames=2147483648,target=1400,target=0,avail=1,null" );
  CHECK( exact_periodsize <= pacing.controller.allowance );
  CHECK( pacing.mode == ALSA_PACING_CAPACITY );
  init( "frames=12,buffer=2640,avail=1,target=1400, null" );
  CHECK( exact_periodsize >= 219 && exact_periodsize <= 220 );
  CHECK( pacing.mode == ALSA_PACING_FIXED && pacing.fixed_target == 1400 );
  fallback = 1;
  init( NULL );
  CHECK( !strcmp( pcm_name, "plughw:0,0" ) );
  fallback = 0;

  for( config_fault = CONFIG_OPEN; config_fault <= CONFIG_QUERY;
       config_fault++ ) {
    sound_lowlevel_end();
    rate = 44100; stereo = 1;
    settings_current.sound = 1;
    closed = closes;
    CHECK( sound_lowlevel_init( "null", &rate, &stereo ) == 1 );
    CHECK( !pcm_handle );
    CHECK( closes == closed + ( config_fault != CONFIG_OPEN ) );
    /* Software setup historically leaves the sound setting to its caller. */
    CHECK( settings_current.sound == ( config_fault == CONFIG_SOFTWARE ) );
    /* A failure must release the reentrancy guard as well as the PCM. */
    {
      enum config_fault fault = config_fault;
      config_fault = CONFIG_OK;
      init( "null" );
      config_fault = fault;
    }
  }
  config_fault = CONFIG_OK;
  sound_lowlevel_end();
  puts( "ALSA options, fallback and configuration cleanup passed" );
}

int
main( void )
{
  libspectrum_signed_word samples[1920];
  unsigned int i, epoch;
  test_machine.timings.tstates_per_frame = 69888;
  for( i = 0; i < 1920; i++ ) samples[i] = i;
  test_configuration();
  init( "null" );
  CHECK( exact_periodsize <= pacing.controller.allowance );
  CHECK( exact_bsize >= 2 * pacing.startup_bound );
  CHECK( exact_bsize >= 10 * exact_periodsize );
  for( i = 0; i < 6000; i++ ) {
    captured = 0;
    CHECK( !sound_lowlevel_reserve( 220 ) );
    sound_lowlevel_frame( samples, 440 );
    CHECK( settings_current.sound && captured == 440 );
    CHECK( !memcmp( capture, samples, sizeof( *samples ) * 440 ) );
  }
  CHECK( starts == 1 && waits && pacing.normal );
  CHECK( pacing.controller.target < pacing.controller.capacity );
  CHECK( pacing.controller.state != ADAPTIVE_INVALID );
  CHECK( !sound_lowlevel_reserve( 0 ) );
  CHECK( sound_lowlevel_reserve( exact_bsize + 1 ) == -EINVAL );
  /* Native waits drain a full exceptional batch; it cannot resize the target. */
  context = 0;
  captured = 0;
  CHECK( !sound_lowlevel_reserve( 880 ) );
  CHECK( !pacing.ordinary );
  sound_lowlevel_frame( samples, 1760 );
  CHECK( captured == 1760 );
  CHECK( !memcmp( capture, samples, sizeof( *samples ) * 1760 ) );
  init( "frames=3,null" );
  CHECK( exact_periodsize > 3 * pacing.controller.allowance );
  CHECK( exact_bsize >= 2 * pacing.startup_bound );
  init( "buffer=2640,frames=12,null" );
  CHECK( exact_periodsize >= 219 && exact_periodsize <= 220 );
  CHECK( exact_bsize >= 2640 - exact_periodsize &&
         exact_bsize <= 2640 + exact_periodsize );
  init( "target=1400,frames=2,null" );
  CHECK( pacing.mode == ALSA_PACING_FIXED && pacing.fixed_target == 1400 );
  for( i = 0; i < 50; i++ ) {
    captured = 0;
    CHECK( !sound_lowlevel_reserve( 220 ) );
    sound_lowlevel_frame( samples, 440 );
    CHECK( settings_current.sound );
    if( pacing.ordinary ) CHECK( queued <= 1400 );
  }
  CHECK( pacing.normal && pacing.ordinary );
  init( "target=0,null" );
  CHECK( pacing.mode == ALSA_PACING_CAPACITY );
  /* Partial writes, interruptions and temporary unavailability: exact identity. */
  transport_state = SND_PCM_STATE_RUNNING;
  script[0] = 30; script[1] = -EINTR; script[2] = -EAGAIN;
  script[3] = 0; script[4] = 40; script_count = 5;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  sound_lowlevel_frame( samples, 440 );
  CHECK( captured == 440 && waits == 2 && !errors );
  CHECK( !memcmp( capture, samples, sizeof( *samples ) * 440 ) );
  /* An xrun between accepted prefix and tail must never replay the prefix. */
  init( "null" );
  epoch = pacing.controller.epoch;
  script[0] = 30; script[1] = -EPIPE; script_count = 2;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  sound_lowlevel_frame( samples, 440 );
  CHECK( captured == 440 && prepares == 1 && !pacing.reserved );
  CHECK( pacing.controller.epoch > epoch && !pacing.controller.funded );
  CHECK( !memcmp( capture, samples, sizeof( *samples ) * 440 ) );
  /* A prepared partial write needing space starts instead of deadlocking. */
  init( "null" );
  script[0] = 30; script[1] = -EAGAIN; script_count = 2;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  sound_lowlevel_frame( samples, 440 );
  CHECK( starts == 1 && captured == 440 && !errors );
  CHECK( !memcmp( capture, samples, sizeof( *samples ) * 440 ) );
  /* Successful resume retains the queue, but not qualification. */
  init( "null" );
  script[0] = -ESTRPIPE; script_count = 1; resume_error = 0;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  sound_lowlevel_frame( samples, 440 );
  CHECK( !prepares && captured == 440 && !pacing.controller.funded );
  /* Suspended resume retries are bounded before preparing again. */
  init( "null" );
  script[0] = -ESTRPIPE; script_count = 1; resume_error = -EAGAIN;
  clock_us = 0;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  sound_lowlevel_frame( samples, 440 );
  CHECK( prepares == 1 && clock_us == 100000 && captured == 440 );
  /* GTK-style drained pause: recover at reserve, not an assertion abort. */
  transport_state = SND_PCM_STATE_XRUN;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  CHECK( prepares == 2 );
  sound_lowlevel_frame( samples, 440 );
  init( "null" );
  script[0] = -ENODEV; script_count = 1;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  sound_lowlevel_frame( samples, 440 );
  CHECK( !pcm_handle && !settings_current.sound && errors == 1 );
  CHECK( !sound_lowlevel_reserve( 220 ) );
  sound_lowlevel_end();
  init( "'null" );
  CHECK( !strcmp( pcm_name, "null" ) );
  transport_state = SND_PCM_STATE_DISCONNECTED;
  CHECK( !sound_lowlevel_reserve( 220 ) );
  CHECK( !pcm_handle && !settings_current.sound && errors == 1 );
  init( "null" );
  sound_lowlevel_end();
  puts(
    "ALSA backend admission, PCM identity, partial writes and recovery passed" );
  return 0;
}
