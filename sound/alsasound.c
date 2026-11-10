/* alsasound.c: ALSA (Linux) sound I/O
   Copyright (c) 2006-2008 Gergely Szasz
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

#include "config.h"

/* This is necessary to prevent warnings from the calls to
   snd_pcm_[hs]w_params_alloca() */
#ifndef NDEBUG
#define NDEBUG
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include <alsa/asoundlib.h>

#include "compat.h"
#include "settings.h"
#include "sound.h"
#include "spectrum.h"
#include "ui/ui.h"
#include "alsa_pacing.h"
#include "alsa_config.h"

static snd_pcm_t *pcm_handle;
static int ch;
static const char *pcm_name = NULL;
static int verb = 0;
static snd_pcm_uframes_t exact_periodsize, exact_bsize;

static snd_output_t *output = NULL;
static struct alsa_pacing pacing;
static snd_pcm_uframes_t prefill_goal, wait_threshold;
static bool waiting_configured, interval_max_supported;
static FILE *trace;
static unsigned int trace_rows, trace_waits, admitted_limit;
static unsigned int xruns, suspends;

static void alsa_fail( const char *operation, int error );

void
sound_lowlevel_end( void )
{
  /* Stop PCM device and drop pending frames */
  if( pcm_handle ) {
    snd_pcm_drop( pcm_handle );
    snd_pcm_close( pcm_handle );
    pcm_handle = NULL;
  }
  memset( &pacing, 0, sizeof( pacing ) );
  waiting_configured = false;
  if( trace ) {
    FILE *closing = trace;
    trace = NULL;
    if( fclose( closing ) )
      ui_error( UI_ERROR_WARNING, "ALSA: closing diagnostic trace: %s",
                strerror( errno ) );
  }
}

static int
alsa_init_pacing( int frequency, const struct alsa_configuration *config )
{
  libspectrum_dword processor_speed, interval;
  uint64_t allowance, startup;

  prefill_goal = exact_bsize > exact_periodsize ?
                 exact_bsize - exact_periodsize : exact_bsize;
  processor_speed = sound_get_effective_processor_speed();
  interval = sound_audio_interval_tstates( processor_speed );
  allowance = processor_speed ?
              (uint64_t)frequency * interval / processor_speed + 1 : 0;
  startup = processor_speed ? (uint64_t)frequency *
            machine_current->timings.tstates_per_frame / processor_speed +
            1 : 0;
  if( !startup || exact_bsize > UINT_MAX || exact_periodsize > UINT_MAX ||
      allowance > UINT_MAX || startup > UINT_MAX ||
      alsa_pacing_init( &pacing, exact_bsize, allowance, exact_periodsize,
                        startup, config->mode, config->target ) ) {
    ui_error( UI_ERROR_ERROR, "ALSA: unusable PCM admission geometry on '%s'",
              pcm_name );
    settings_current.sound = 0;
    return 1;
  }
  return 0;
}

static void
alsa_open_trace( const struct alsa_configuration *config )
{
  const char *trace_path;

  /* ioplug's avail_max is cumulative until reset, unlike the documented
     per-status hardware interval. Do not turn that into perpetual warnings. */
  interval_max_supported = snd_pcm_type( pcm_handle ) == SND_PCM_TYPE_HW;
  if( verb )
    fprintf( stdout, "ALSA: %s, %u Hz, %d channels, capacity=%lu, "
             "period=%lu, batch-device=%d, pacing=%s, target=%u\n",
             pcm_name, config->rate, ch, (unsigned long)exact_bsize,
             (unsigned long)exact_periodsize,
             config->batch,
             config->mode == ALSA_PACING_ADAPTIVE ? "adaptive" :
             config->mode == ALSA_PACING_FIXED ? "fixed" : "capacity",
             config->mode == ALSA_PACING_FIXED ? config->target :
             (unsigned int)exact_bsize );
  trace_path = getenv( "FUSE_ALSA_TRACE" );
  if( trace_path && *trace_path ) {
    trace = fopen( trace_path, "w" );
    if( !trace ) {
      ui_error( UI_ERROR_WARNING, "ALSA: opening trace '%s': %s",
                trace_path, strerror( errno ) );
    } else {
      fprintf( trace, "# PCM=%s rate=%u channels=%d C=%lu period=%lu "
               "batch=%d mode=%d interval-max=%d\n", pcm_name, config->rate, ch,
               (unsigned long)exact_bsize, (unsigned long)exact_periodsize,
               config->batch, config->mode,
               interval_max_supported );
      fprintf( trace, "time_us,B,before,after,effective,progress,ordinary,"
               "complete,running,waits,target,accepted,floor,state,epoch,mode,"
               "limit,xruns,suspends,error\n" );
    }
  }
}

int
sound_lowlevel_init( const char *device, int *freqptr, int *stereoptr )
{
  static int first_init = 1, init_running = 0;
  struct alsa_configuration config;
  int result = 1;

  if( init_running ) return 0;
  if( pcm_handle ) return 1;
  init_running = 1;
  waiting_configured = false;
  if( alsa_configure( device, freqptr, stereoptr, first_init, &config ) ) {
    sound_lowlevel_end();
    goto finished;
  }
  pcm_handle = config.pcm_handle;
  pcm_name = config.pcm_name;
  verb = config.verb;
  ch = config.ch;
  exact_periodsize = config.exact_periodsize;
  exact_bsize = config.exact_bsize;
  if( alsa_init_pacing( *freqptr, &config ) ) {
    sound_lowlevel_end();
    goto finished;
  }
  alsa_open_trace( &config );
  trace_rows = trace_waits = xruns = suspends = 0;
  if( first_init ) snd_output_stdio_attach( &output, stdout, 0 );
  first_init = 0;
  result = 0;

finished:
  init_running = 0;
  return result;
}

/* Device failures are not synthesis assertion failures. Retire output locally:
   reserve's negative return is reserved for programming/geometry errors. */
static void
alsa_fail( const char *operation, int error )
{
  settings_current.sound = 0;
  sound_lowlevel_end();
  ui_error( UI_ERROR_ERROR, "ALSA: %s on '%s': %s", operation,
            pcm_name ? pcm_name : "default", snd_strerror( error ) );
}

static int
alsa_recover( int error )
{
  int result;
  long long deadline;
  if( error == -EINTR || error == -EAGAIN ) return 0;
  if( error != -EPIPE && error != -ESTRPIPE ) return error;
  if( error == -EPIPE ) xruns++;
  else suspends++;
  if( error == -ESTRPIPE ) {
    deadline = compat_monotonic_time_us() + 100000;
    do {
      result = snd_pcm_resume( pcm_handle );
      if( result != -EAGAIN ) break;
      compat_timer_sleep( 1 );
    } while( compat_monotonic_time_us() < deadline );
    if( !result ) {
      /* Resume retains queued frames, but does not retain adaptive evidence. */
      alsa_pacing_restart( &pacing );
      waiting_configured = false;
      return 0;
    }
  }
  result = snd_pcm_prepare( pcm_handle );
  if( result < 0 ) return result;
  alsa_pacing_restart( &pacing );
  waiting_configured = false;
  return 0;
}

/* Status resets avail_max: each successful call ends one labelled observation
   window. It is reported low water, not a continuously measured physical queue. */
static int
alsa_snapshot( unsigned int *q, bool *running, bool observe_minimum )
{
  snd_pcm_status_t *status;
  snd_pcm_uframes_t available, maximum;
  snd_pcm_state_t state;
  int error;
  snd_pcm_status_alloca( &status );
  error = snd_pcm_status( pcm_handle, status );
  if( error < 0 ) return error;
  state = snd_pcm_status_get_state( status );
  if( state == SND_PCM_STATE_XRUN ) return -EPIPE;
  if( state == SND_PCM_STATE_SUSPENDED ) return -ESTRPIPE;
  if( state == SND_PCM_STATE_DISCONNECTED ) return -ENODEV;
  if( state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_PREPARED )
    return -EBADFD;
  available = snd_pcm_status_get_avail( status );
  maximum = snd_pcm_status_get_avail_max( status );
  if( available > exact_bsize ) return -ERANGE;
  *q = exact_bsize - available;
  *running = state == SND_PCM_STATE_RUNNING;
  if( alsa_pacing_snapshot( &pacing, *q ) ) {
    /* A plugin may expose a different accounting domain. Keep playback, but
       do not keep inventing progress or qualifying shallow targets. */
    adaptive_fallback( &pacing.controller );
    pacing.mode = ALSA_PACING_CAPACITY;
    pacing.have_queue = false;
    pacing.accepted = 0;
    pacing.quarantine = true;
  }
  if( observe_minimum && *running && interval_max_supported )
    alsa_pacing_low_water( &pacing,
                           maximum <= exact_bsize ? exact_bsize - maximum : 0,
                           maximum >= available && maximum <= exact_bsize );
  return 0;
}

/* Development-only bounded trace. No callback, normal logging or policy
   side effects; all I/O is on the producer and explicitly opted in. */
static void
alsa_trace( unsigned int batch, unsigned int before, unsigned int after,
            bool complete, bool running, int error )
{
  if( trace && trace_rows < 65536 ) {
    trace_rows++;
    if( fprintf( trace,
                 "%lld,%u,%u,%u,%lld,%u,%d,%d,%d,%u,%u,%u,%llu,%d,%llu,%d,%u,%u,%u,%d\n",
                 compat_monotonic_time_us(), batch, before, after,
                 (long long)after - batch, pacing.progress, pacing.ordinary,
                 complete, running, trace_waits, alsa_pacing_limit( &pacing ),
                 pacing.mode == ALSA_PACING_FIXED && pacing.normal ?
                 pacing.fixed_target : pacing.controller.accepted,
                 (unsigned long long)pacing.controller.floor,
                 pacing.controller.state,
                 (unsigned long long)pacing.controller.epoch, pacing.mode,
                 admitted_limit, xruns, suspends, error ) < 0 ) {
      int saved_errno = errno;
      fclose( trace );
      trace = NULL;
      ui_error( UI_ERROR_WARNING, "ALSA: writing diagnostic trace: %s",
                strerror( saved_errno ) );
    }
  }
}

static int
alsa_wait( unsigned int needed )
{
  snd_pcm_sw_params_t *params;
  int error;
  long long before;
  if( !waiting_configured || wait_threshold != needed ) {
    snd_pcm_sw_params_alloca( &params );
    error = snd_pcm_sw_params_current( pcm_handle, params );
    if( error < 0 ) return error;
    error = snd_pcm_sw_params_set_avail_min( pcm_handle, params, needed );
    if( error < 0 ) return error;
    error = snd_pcm_sw_params( pcm_handle, params );
    if( error < 0 ) return error;
    wait_threshold = needed;
    waiting_configured = true;
  }
  trace_waits++;
  before = compat_monotonic_time_us();
  error = snd_pcm_wait( pcm_handle, 10 );
  /* Some plugins signal readiness below the requested threshold. Rate-limit
     immediate returns; the caller still has to recheck its predicate. */
  if( error >= 0 && compat_monotonic_time_us() - before < 1000 )
    compat_timer_sleep( 1 );
  return error < 0 ? error : 0;
}

/* Prepared streams cannot drain. A nonempty prefill can be started, but an
   empty stream with no room is invalid admission geometry. */
static int
alsa_start_prepared( unsigned int queued )
{
  return queued ? snd_pcm_start( pcm_handle ) : -ENOSPC;
}

static int
alsa_recover_output( const char *operation, int error )
{
  error = alsa_recover( error );
  if( error < 0 ) alsa_fail( operation, error );
  return error;
}

static int
alsa_wait_for_admission( unsigned int frames, unsigned int queued,
                         unsigned int limit, bool running )
{
  int error;

  /* A prepared buffer cannot drain; start its prefill or retire output. */
  if( !running ) {
    error = alsa_start_prepared( queued );
    if( error )
      alsa_fail( queued ? "prefill start" : "prefill capacity", error );
    return error;
  }
  error = alsa_wait( pacing.controller.capacity - limit + frames );
  return error < 0 ? alsa_recover_output( "wait/recovery", error ) : 0;
}

int
sound_lowlevel_reserve( unsigned int frames )
{
  unsigned int q, limit;
  bool running;
  int error;
  if( !pcm_handle || !frames ) return 0;
  if( pacing.reserved || frames > pacing.controller.capacity ) return -EINVAL;
  trace_waits = 0;
  for( ;; ) {
    error = alsa_snapshot( &q, &running, true );
    if( error < 0 ) {
      if( alsa_recover_output( "query/recovery", error ) < 0 ) return 0;
      continue;
    }
    if( alsa_pacing_pending( &pacing, frames,
                             sound_normal_producer_context(), running ) )
      return -EINVAL;
    limit = alsa_pacing_limit( &pacing );
    if( frames <= limit && q <= limit - frames ) {
      admitted_limit = limit;
      pacing.reserved = true;
      return 0;
    }
    if( alsa_wait_for_admission( frames, q, limit, running ) ) return 0;
  }
}

/* These observations are transport-only: they must not end a labelled
   low-water window or qualify a publication. */
static int
alsa_start_after_write( unsigned int *after, bool *running )
{
  int error = alsa_snapshot( after, running, false );
  if( !error && !*running && *after >= prefill_goal )
    error = snd_pcm_start( pcm_handle );
  return error;
}

static int
alsa_wait_writable( unsigned int *after, bool *running )
{
  int error = alsa_snapshot( after, running, false );
  if( !error && !*running ) error = alsa_start_prepared( *after );
  if( !error ) error = alsa_wait( 1 );
  return error;
}

static int
alsa_retry_write( int error, unsigned int *after, bool *running )
{
  if( error == -EINTR ) return 0;
  if( error == -EAGAIN ) return alsa_wait_writable( after, running );
  return error;
}

/* Recovery may keep playback alive without retaining valid controller
   evidence. Never replay an accepted prefix when recovering the remaining tail. */
static int
alsa_write_frames( libspectrum_signed_word *data, unsigned int remaining,
                   unsigned int *after, bool *running, bool *complete )
{
  snd_pcm_sframes_t written;
  int error;

  while( remaining ) {
    written = snd_pcm_writei( pcm_handle, data, remaining );
    if( written > 0 ) {
      if( (snd_pcm_uframes_t)written > remaining ) return -ERANGE;
      alsa_pacing_accept( &pacing, written );
      data += written * ch;
      remaining -= written;
      /* Explicit start also applies to a recovered tail being prefilled. */
      error = alsa_start_after_write( after, running );
    } else {
      error = alsa_retry_write( written < 0 ? written : -EAGAIN,
                                after, running );
    }
    if( !error ) continue;
    *complete = false;
    error = alsa_recover( error );
    if( error < 0 ) return error;
  }
  return 0;
}

void
sound_lowlevel_frame( libspectrum_signed_word *data, int len )
{
  unsigned int before = 0, after = 0, batch;
  bool running = false, complete = true;
  int error;
  if( !pcm_handle ) return;
  if( len <= 0 || len % ch || !pacing.reserved ||
      (unsigned int)( len / ch ) != pacing.batch ) {
    alsa_fail( "publication identity", -EINVAL );
    return;
  }
  batch = len / ch;
  error = alsa_snapshot( &before, &running, true );
  if( error < 0 ) {
    error = alsa_recover( error );
    complete = false;
    if( error < 0 ) goto failed;
  }
  error = alsa_write_frames( data, batch, &after, &running, &complete );
  if( error < 0 ) goto failed;
  error = alsa_snapshot( &after, &running, true );
  if( error < 0 ) {
    error = alsa_recover( error );
    complete = false;
    if( error < 0 ) goto failed;
  }
  if( complete && pacing.reserved ) {
    if( alsa_pacing_publication( &pacing, before, after, running,
                                 sound_normal_producer_context() ) ) {
      adaptive_fallback( &pacing.controller );
      pacing.mode = ALSA_PACING_CAPACITY;
      pacing.quarantine = true;
      complete = false;
    }
  }
  alsa_trace( batch, before, after, complete, running, 0 );
  pacing.reserved = false;
  return;
failed:
  alsa_trace( batch, before, after, false, running, error );
  alsa_fail( "write/recovery", error );
}
