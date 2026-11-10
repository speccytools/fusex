/* sound.c: Sound support
   Copyright (c) 2000-2026 Russell Marks, Matan Ziv-Av, Philip Kendall,
                           Fredrick Meunier, Patrik Rak

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

   Author contact information:

   E-mail: philip-fuse@shadowmagic.org.uk

*/

/* The AY white noise RNG algorithm is based on info from MAME's ay8910.c -
 * MAME's licence explicitly permits free use of info (even encourages it).
 */

#include "config.h"

#include <string.h>

#include "fuse.h"
#include "event.h"
#include "debugger/debugger.h"
#include "rzx.h"
#include "infrastructure/startup_manager.h"
#include "machine.h"
#include "movie.h"
#include "options.h"
#include "settings.h"
#include "sound.h"
#include "tape.h"
#include "timer/timer.h"
#include "ui/ui.h"
#include "peripherals/sound/sp0256.h"
#include "sound/ay_engine.h"
#include "sound/audio_timeline.h"
#include "sound/blipbuffer.h"
#include "sound/output_mixer.h"
#include "sound/source_synths.h"
#include "sound/silence_noise.h"

/* Do we have any of our sound devices available? */

/* configuration */
int sound_enabled = 0;		/* Are we currently using the sound card */

static int sound_enabled_ever = 0; /* whether sound has *ever* been in use; see
				      sound_ay_write() and sound_ay_reset() */
int sound_stereo_ay = SOUND_STEREO_AY_NONE; /* local copy of settings_current.stereo_ay */

int sound_framesiz;

/* Whether the output has a right channel, as distinct from sound_stereo_ay,
   which is the AY panning choice. */
static int sound_stereo = 0;

/* The layout sound_init asked the device for, which is not always the one
   it got. */
static int sound_stereo_wanted = 0;

/* Channels in the output stream: General Sound needs two whether or not
   the AY channels are being separated. */
int sound_channels;
static sound_silence_noise silence_noise;

/* The main buffers contain sources which bypass speaker processing. */
Blip_Buffer *left_buf = NULL;
Blip_Buffer *right_buf = NULL;
blip_sample_t *samples = NULL;
static int sound_tv_route = -1;
static libspectrum_dword audio_position;
/* Advance audio at the ordinary time cadence, independently of FIFO latency
   policy. Test builds may override the interval in CPU T-states. */
#ifdef FUSE_SOUND_TEST_INTERVAL
static libspectrum_dword audio_cut_interval = FUSE_SOUND_TEST_INTERVAL;
#else
static libspectrum_dword audio_cut_interval;
#endif
static int audio_event = -1;
static int audio_frame_suspended;
static void sound_schedule_audio( void );
static void sound_audio_event( libspectrum_dword last_tstates, int type,
                               void *user_data );
static blip_sample_t *frame_samples;
static long frame_sample_count;

static void sound_update_source_routes( void );

/* Returns the emulation speed adjusted processor speed */
libspectrum_dword
sound_get_effective_processor_speed( void )
{
  return machine_current->timings.processor_speed / 100 * 
           settings_current.emulation_speed;
}

static int
sound_init_buffer( Blip_Buffer **buf )
{
  *buf = new_Blip_Buffer();
  blip_buffer_set_clock_rate( *buf, sound_get_effective_processor_speed() );
  /* Allow up to 1s of playback buffer - this allows us to cope with slowing
     down to 2% of speed where a single Speccy frame generates just under 1s
     of sound */
  if ( blip_buffer_set_sample_rate( *buf, settings_current.sound_freq, 1000 ) ) {
    sound_end();
    ui_error( UI_ERROR_ERROR, "out of memory at %s:%d", __FILE__, __LINE__ );
    return 0;
  }

  blip_buffer_set_bass_freq( *buf, 0 );
  return 1;
}

#ifndef UI_WIN32
#define MIN_SPEED_PERCENTAGE 2
#define MAX_SPEED_PERCENTAGE 500
#else                        /* #ifndef UI_WIN32 */
/* We are limiting speed until bugs in the DirectSound driver are resolved, see
   [bugs:#364] for more details */
#define MIN_SPEED_PERCENTAGE 50
#define MAX_SPEED_PERCENTAGE 300
#endif                       /* #ifndef UI_WIN32 */

static int
is_in_sound_enabled_range( void )
{
  return settings_current.emulation_speed >= MIN_SPEED_PERCENTAGE &&
    settings_current.emulation_speed <= MAX_SPEED_PERCENTAGE;
}

/* Separating the AY channels needs a right channel, and so does General
   Sound, whose four channels are split two per side. */
static int
stereo_needed( void )
{
  return option_enumerate_sound_stereo_ay() != SOUND_STEREO_AY_NONE ||
         settings_current.general_sound;
}

/* Whether the running output layout still matches the settings. */
int
sound_layout_stale( void )
{
  if( !sound_enabled ) return 0;

  return sound_stereo_wanted != stereo_needed() ||
         sound_stereo_ay != option_enumerate_sound_stereo_ay();
}

void
sound_init( const char *device )
{
  float hz;

  /* Allow sound as long as emulation speed is greater than 2%
     (less than that and a single Speccy frame generates more
     than a seconds worth of sound which is bigger than the
     maximum Blip_Buffer of 1 second) */
  if( !( !sound_enabled && settings_current.sound &&
         is_in_sound_enabled_range() ) )
    return;

  /* only try for stereo if we need it */
  sound_stereo_ay = option_enumerate_sound_stereo_ay();
  sound_stereo = sound_stereo_wanted = stereo_needed();
  if( settings_current.sound &&
      sound_lowlevel_init( device, &settings_current.sound_freq,
                           &sound_stereo ) )
    return;

  /* The device may not have given us a right channel, in which case the
     AY channels cannot be separated either. */
  if( !sound_stereo ) sound_stereo_ay = SOUND_STEREO_AY_NONE;

  if( sound_stereo_ay != SOUND_STEREO_AY_NONE &&
      sound_stereo_ay != SOUND_STEREO_AY_ACB &&
      sound_stereo_ay != SOUND_STEREO_AY_ABC ) {
    ui_error( UI_ERROR_ERROR, "unknown AY stereo separation type: %d",
              sound_stereo_ay );
    fuse_abort();
  }

  sound_channels = sound_stereo ? 2 : 1;
  sound_silence_noise_reset( &silence_noise );
  hz = ( float )sound_get_effective_processor_speed() /
                machine_current->timings.tstates_per_frame;
  sound_framesiz = ( float )settings_current.sound_freq / hz + 1;

  if( !sound_init_buffer( &left_buf ) ) return;
  if( sound_channels == 2 && !sound_init_buffer( &right_buf ) ) return;
  if( output_mixer_init( sound_get_effective_processor_speed(),
                         settings_current.sound_freq, sound_framesiz,
                         sound_channels, settings_current.volume_beeper ) ) {
    ui_error( UI_ERROR_ERROR, "could not configure Spectrum output mixer" );
    sound_end();
    return;
  }

  audio_position = 0;
  frame_sample_count = 0;
  audio_frame_suspended = 1;
#ifndef FUSE_SOUND_TEST_INTERVAL
  audio_cut_interval = sound_audio_interval_tstates(
                         sound_get_effective_processor_speed() );
#endif
  if( audio_cut_interval && audio_event < 0 )
    audio_event = event_register( sound_audio_event, "Audio cut" );
  ay_engine_init( settings_current.volume_ay, sound_stereo_ay,
                  sound_channels == 2 );
  source_synths_init( left_buf, right_buf, sound_channels == 2,
                      settings_current.volume_specdrum,
                      settings_current.volume_covox,
                      settings_current.volume_uspeech );
  samples = libspectrum_new0( blip_sample_t, sound_framesiz * sound_channels );
  frame_samples = libspectrum_new0( blip_sample_t,
                                    sound_framesiz * sound_channels );

  sound_enabled = sound_enabled_ever = 1;
  sound_tv_route = -1;
  sound_update_source_routes();
  movie_init_sound( settings_current.sound_freq, sound_stereo );
}

void
sound_pause( void )
{
  if( sound_enabled )
    sound_end();
}

void
sound_unpause( void )
{
  /* No sound if fastloading in progress */
  if( settings_current.fastload && timer_fastloading_active() )
    return;

  sound_init( settings_current.sound_device );
}

void
sound_end( void )
{
  if( audio_event >= 0 ) event_remove_type( audio_event );
  if( sound_enabled ) {
    ay_engine_end();
    source_synths_end();
    output_mixer_end();
    delete_Blip_Buffer( &left_buf );
    delete_Blip_Buffer( &right_buf );

    if( settings_current.sound ) sound_lowlevel_end();
    libspectrum_free( samples );
    libspectrum_free( frame_samples );
    audio_position = 0;
    frame_sample_count = 0;
    sound_enabled = 0;
  }
}

void
sound_register_startup( void )
{
  startup_manager_module dependencies[] = {
    STARTUP_MANAGER_MODULE_EVENT, STARTUP_MANAGER_MODULE_SETUID
  };
  startup_manager_register( STARTUP_MANAGER_MODULE_SOUND, dependencies,
                            ARRAY_SIZE( dependencies ), NULL, NULL, sound_end );
}

void
sound_ay_write( int reg, int val, libspectrum_dword now )
{
  sound_interval_time( now );
  ay_engine_write( reg, val, now );
}

void
sound_ay_reset( void )
{
  /* A source reset is not a timestamped write in the current audio timeline. */
  sound_suspend_subframe();
  ay_engine_reset();
  output_mixer_reset( audio_position ? left_buf->offset_ : 0 );
  sound_tv_route = -1;
}

void
sound_sp0256_write( libspectrum_dword at_tstates, libspectrum_signed_word val )
{
  if( !sound_enabled ) return;
  sound_update_source_routes();
  source_synths_sp0256_write( at_tstates, val );
}

int
sound_resolve_speaker_type( int selected_type, int machine_capabilities,
                            int uspeech_enabled )
{
  if( selected_type != SOUND_SPEAKER_TYPE_AUTOMATIC ) return selected_type;

  /* A connected Currah routes its speech and the Spectrum MIC lead through
   * the television, including on machines which normally use a beeper. */
  if( uspeech_enabled ) return SOUND_SPEAKER_TYPE_TV;

  return machine_capabilities & LIBSPECTRUM_MACHINE_CAPABILITY_BEEPER ?
           SOUND_SPEAKER_TYPE_BEEPER : SOUND_SPEAKER_TYPE_TV;
}

unsigned int
sound_tv_source_routes( int speaker_type, int machine_capabilities )
{
  if( speaker_type != SOUND_SPEAKER_TYPE_TV ) return 0;

  return SOUND_ROUTE_ULA_MIC | SOUND_ROUTE_USPEECH |
         ( ( machine_capabilities & LIBSPECTRUM_MACHINE_CAPABILITY_AY ) ?
             SOUND_ROUTE_BUILTIN_AY : 0 );
}

static void
sound_update_source_routes( void )
{
  int speaker_type = output_mixer_speaker_type();
  unsigned int routes = sound_tv_source_routes(
                          speaker_type, machine_current->capabilities );
  int tv_route = speaker_type == SOUND_SPEAKER_TYPE_TV;
  int ay_to_tv = routes & SOUND_ROUTE_BUILTIN_AY;
  Blip_Buffer *ay_left = ay_to_tv ? output_mixer_tv_left() : left_buf;
  Blip_Buffer *ay_right = ay_to_tv ? output_mixer_tv_right() : right_buf;
  Blip_Buffer *speech_left = tv_route ? output_mixer_tv_left() : left_buf;
  Blip_Buffer *speech_right = tv_route ? output_mixer_tv_right() : right_buf;

  if( sound_tv_route == tv_route ) return;

  ay_engine_set_outputs( ay_left, ay_right, sound_stereo_ay );

  source_synths_set_speech_output( speech_left, speech_right );

  output_mixer_route_changed();
  sound_tv_route = tv_route;
}

libspectrum_dword
sound_audio_position( void )
{
  return audio_position;
}

libspectrum_dword
sound_interval_time( libspectrum_dword frame_time )
{
  /* Future events may overshoot the machine frame, but consumed time cannot
     be written again. Never hide a caller error by clamping or wrapping. */
  if( frame_time < audio_position ) {
    ui_error( UI_ERROR_ERROR, "Audio event precedes extracted timeline" );
    fuse_abort();
  }
  return frame_time - audio_position;
}

/* Endpoints are machine tstates in this frame, ordered in [0, F]. Equal
   endpoints are no-ops; backwards or beyond-F endpoints fail before mutation.
   Call only at a safe machine-time boundary: ULA/DAC events must not already
   have been delivered beyond the endpoint. Route/settings changes and source
   resets are discontinuities, not partitions of an otherwise identical frame.
   The subframe scheduler retries at actual CPU progress only after
   strictly older queued source events have drained.
   Blip retains its fractional offset and impulse tail across every interval. */
static int
sound_advance_to( libspectrum_dword endpoint )
{
  if( endpoint < audio_position ||
      endpoint > machine_current->timings.tstates_per_frame ) return -1;
  if( !sound_enabled || endpoint == audio_position ) return 0;

  const libspectrum_dword elapsed = endpoint - audio_position;
  long frames = blip_buffer_samples_after( left_buf, elapsed );
  if( frames < 0 ) return -1;
  /* Preserve the legacy extraction limit for the whole machine frame, not
     separately for each interval. Rounded Blip rates can leave unread samples
     even at F; those samples and fractional residue belong to the next read. */
  const long remaining = sound_framesiz - frame_sample_count / sound_channels;
  if( frames > remaining ) frames = remaining;

#ifdef SOUND_PCM_ADMISSION
  /* Source events alter amplitudes, not sample geometry. Physical admission
     precedes route/source/filter/Blip mutation, including zero-sample cuts. */
  if( settings_current.sound && frames && sound_lowlevel_reserve( frames ) < 0 )
    return -1;
#endif

  sound_update_source_routes();
  sp0256_advance_to( endpoint );
  ay_engine_render( endpoint );

  blip_buffer_end_frame( left_buf, elapsed );

  long count;
  if( sound_stereo ) {
    blip_buffer_end_frame( right_buf, elapsed );

    /* Read left channel into even samples, right channel into odd samples:
       LRLRLRLRLR... */
    count = blip_buffer_read_samples( left_buf, samples, frames, 1 );
    blip_buffer_read_samples( right_buf, samples + 1, count, 1 );
    count <<= 1;
  } else {
    count = blip_buffer_read_samples( left_buf, samples, frames,
                                      BLIP_BUFFER_DEF_STEREO );
  }

  if( count != frames * sound_channels ) fuse_abort();
  output_mixer_advance( elapsed, samples, count );
  /* Keep short digital silences from triggering aggressive DAC auto-muting.
     Apply to the final PCM, including recordings, not the speaker model. */
  sound_silence_noise_apply( &silence_noise, samples, count, sound_channels );
  memcpy( frame_samples + frame_sample_count, samples,
          count * sizeof( *samples ) );
  frame_sample_count += count;
  audio_position = endpoint;

  if( settings_current.sound && count )
    sound_lowlevel_frame( samples, count );
  return 0;
}

static int
sound_can_produce_subframe( void )
{
  return audio_cut_interval && sound_enabled && !audio_frame_suspended &&
         !rzx_playback && !movie_recording &&
         !settings_current.general_sound &&
         debugger_mode == DEBUGGER_MODE_INACTIVE;
}

int
sound_normal_producer_context( void )
{
  return sound_can_produce_subframe();
}

static void
sound_schedule_audio( void )
{
  libspectrum_dword next;
  libspectrum_dword frame_end = machine_current->timings.tstates_per_frame;

  if( !sound_can_produce_subframe() || audio_cut_interval >= frame_end ||
      tstates >= frame_end ) return;

  /* Skip overtaken lattice cuts rather than producing catch-up batches. */
  next = ( tstates / audio_cut_interval + 1 ) * audio_cut_interval;
  if( next < frame_end ) event_add( next, audio_event );
}

static void
sound_audio_event( libspectrum_dword last_tstates, int type GCC_UNUSED,
                   void *user_data GCC_UNUSED )
{
  libspectrum_dword frame_end = machine_current->timings.tstates_per_frame;

  if( !sound_can_produce_subframe() || tstates >= frame_end ) return;
  if( tstates < audio_position ) {
    /* A discontinuous clock change invalidates the committed audio cursor. */
    sound_suspend_subframe();
    return;
  }
  /* Port writes may already extend past the nominal cut, while older tape
   * callbacks are still queued. Retry at actual instruction progress after
   * strictly older events (including newly scheduled overdue tape edges).
   * Equal-time impulses belong to the following half-open interval; do not
   * depend on event-type tie ordering. Frame crossing belongs to sound_frame.
   */
  if( last_tstates != tstates || event_next_event < tstates ) {
    event_add( tstates, audio_event );
    return;
  }
  if( sound_advance_to( tstates ) ) {
    ui_error( UI_ERROR_ERROR, "Cannot admit audio production batch" );
    fuse_abort();
  }
  sound_schedule_audio();
}

void
sound_suspend_subframe( void )
{
  if( !audio_cut_interval || !sound_enabled ) return;

  if( audio_event >= 0 ) event_remove_type( audio_event );
  audio_frame_suspended = 1;
  /* Clock/source discontinuities cannot retract PCM already published. Drop
   * the partial frame's synthesis history through the existing teardown path;
   * finish this frame without interior cuts, then resume from a frame boundary.
   */
  if( audio_position ) {
    sound_end();
    sound_unpause();
  }
}

void
sound_frame( void )
{
  if( !sound_enabled ) return;
  if( sound_advance_to( machine_current->timings.tstates_per_frame ) ) {
    ui_error( UI_ERROR_ERROR, "Cannot admit audio production batch" );
    fuse_abort();
  }

  output_mixer_end_frame();
  if( movie_recording ) movie_add_sound( frame_samples, frame_sample_count );
  ay_engine_end_frame();
  sp0256_end_frame();
  audio_position = 0;
  frame_sample_count = 0;
  audio_frame_suspended = 0;
  sound_schedule_audio();
}

