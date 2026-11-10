/* output_mixer.c: ULA and TV routed sound output
   Copyright (c) 2000-2026 Russell Marks, Matan Ziv-Av, Philip Kendall,
                           Fredrick Meunier, Patrik Rak

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "config.h"

#include <string.h>

#include "fuse.h"
#include "machine.h"
#include "options.h"
#include "settings.h"
#include "sound.h"
#include "tape.h"
#include "sound/dc_filter.h"
#include "sound/output_mixer.h"
#include "sound/audio_timeline.h"
#include "sound/speaker_filter.h"
#include "sound/tv_filter.h"
#include "sound/ula_filter.h"

static Blip_Buffer *tv_left_buf, *tv_right_buf, *ula_buf;
static Blip_Synth *ula_synth;
static blip_sample_t *tv_samples, *ula_samples;
/* Extraction scratch is private. Accessors expose only the last whole frame. */
static blip_sample_t *ula_frame, *ula_published;
static int ula_frame_count, ula_published_count, ula_frame_size;
static int ula_published_speaker_type = -1;
static int channels, tv_output_count, ula_output_count;
/* The ULA MIC output is active-low at the port. These normalized logical
 * states are retained separately from tape input and rendered output state. */
static int ula_mic_on, ula_beeper_on;
static dc_filter_t unfiltered_dc_filter;
static speaker_filter_t beeper_filter;
static tv_filter_t tv_filters[2];
static ula_filter_t ula_filter;
static int beeper_filter_active;
static int filter_speaker_type = -1;
static int synth_speaker_type = -1;

typedef struct ula_levels_tag {
  int mic;
  int beeper;
} ula_levels_t;

static ula_levels_t current_ula_levels( void );

static int
output_buffer_init( Blip_Buffer **buffer, libspectrum_dword clock_rate,
                    int sample_rate )
{
  *buffer = new_Blip_Buffer();
  blip_buffer_set_clock_rate( *buffer, clock_rate );
  if( blip_buffer_set_sample_rate( *buffer, sample_rate, 1000 ) ) return 1;
  blip_buffer_set_bass_freq( *buffer, 0 );
  return 0;
}

int
output_mixer_init( libspectrum_dword clock_rate, int sample_rate,
                   int frame_size, int output_channels, int beeper_volume )
{
  double volume = beeper_volume < 0 ? 0.0 :
                  beeper_volume > 100 ? 1.0 : beeper_volume / 100.0;

  channels = output_channels;
  if( output_buffer_init( &ula_buf, clock_rate, sample_rate ) ||
      output_buffer_init( &tv_left_buf, clock_rate, sample_rate ) ||
      ( channels == 2 &&
        output_buffer_init( &tv_right_buf, clock_rate, sample_rate ) ) )
    return 1;
  ula_synth = new_Blip_Synth();
  blip_synth_set_volume( ula_synth, volume );
  blip_synth_set_output( ula_synth, ula_buf );
  blip_synth_set_treble_eq( ula_synth, 0.0 );

  if( dc_filter_configure( &unfiltered_dc_filter, sample_rate ) ||
      speaker_filter_configure( &beeper_filter, sample_rate ) ||
      tv_filter_configure( &tv_filters[0], sample_rate ) ||
      tv_filter_configure( &tv_filters[1], sample_rate ) ||
      ula_filter_configure( &ula_filter, sample_rate ) )
    return 1;

  tv_samples = libspectrum_new0( blip_sample_t, frame_size * channels );
  ula_samples = libspectrum_new0( blip_sample_t, frame_size );
  ula_frame = libspectrum_new0( blip_sample_t, frame_size );
  ula_published = libspectrum_new0( blip_sample_t, frame_size );
  ula_frame_size = frame_size;
  ula_frame_count = ula_published_count = 0;
  ula_published_speaker_type = -1;
  beeper_filter_active = 0;
  filter_speaker_type = synth_speaker_type = -1;
  return 0;
}

void
output_mixer_end( void )
{
  delete_Blip_Synth( &ula_synth );
  delete_Blip_Buffer( &tv_left_buf );
  delete_Blip_Buffer( &tv_right_buf );
  delete_Blip_Buffer( &ula_buf );
  libspectrum_free( tv_samples );
  libspectrum_free( ula_samples );
  libspectrum_free( ula_frame );
  libspectrum_free( ula_published );
}

void
output_mixer_reset( blip_resampled_time_t offset )
{
  int speaker_type = output_mixer_speaker_type();

  dc_filter_reset( &unfiltered_dc_filter );
  speaker_filter_reset( &beeper_filter );
  tv_filter_reset( &tv_filters[0] );
  tv_filter_reset( &tv_filters[1] );
  ula_filter_reset( &ula_filter );
  beeper_filter_active = speaker_type == SOUND_SPEAKER_TYPE_BEEPER;
  filter_speaker_type = synth_speaker_type = speaker_type;

  if( sound_enabled ) {
    ula_levels_t levels = current_ula_levels();

    blip_buffer_clear( ula_buf, BLIP_BUFFER_DEF_ENTIRE_BUFF );
    /* Preserve alignment after a discontinuity inside an extracted frame. */
    ula_buf->offset_ = offset;
    blip_synth_set_output( ula_synth, ula_buf );
    blip_synth_set_level( ula_synth,
                          speaker_type == SOUND_SPEAKER_TYPE_BEEPER ?
                          levels.beeper : levels.mic );
  }
  ula_output_count = 0;
  if( !sound_audio_position() ) {
    ula_frame_count = ula_published_count = 0;
    ula_published_speaker_type = speaker_type;
  }
}

int
output_mixer_speaker_type( void )
{
  return sound_resolve_speaker_type( option_enumerate_sound_speaker_type(),
                                     machine_current->capabilities,
                                     settings_current.uspeech );
}

Blip_Buffer *
output_mixer_tv_left( void )
{
  return tv_left_buf;
}

Blip_Buffer *
output_mixer_tv_right( void )
{
  return tv_right_buf;
}

void
output_mixer_route_changed( void )
{
  tv_filter_reset( &tv_filters[0] );
  tv_filter_reset( &tv_filters[1] );
}

static void
select_ula_filters( int speaker_type, int *filter_ula, int *filter_speaker )
{
  *filter_ula = speaker_type != SOUND_SPEAKER_TYPE_UNFILTERED;
  *filter_speaker = speaker_type == SOUND_SPEAKER_TYPE_BEEPER;
  if( speaker_type != SOUND_SPEAKER_TYPE_TV &&
      speaker_type != SOUND_SPEAKER_TYPE_BEEPER &&
      speaker_type != SOUND_SPEAKER_TYPE_UNFILTERED )
    fuse_abort();
}

static void
reset_route_filters( int speaker_type, int filter_ula, int filter_speaker )
{
  if( speaker_type != filter_speaker_type ) {
    if( filter_ula ) ula_filter_reset( &ula_filter );
    if( speaker_type == SOUND_SPEAKER_TYPE_UNFILTERED )
      dc_filter_reset( &unfiltered_dc_filter );
    filter_speaker_type = speaker_type;
  }
  if( filter_speaker != beeper_filter_active ) {
    speaker_filter_reset( &beeper_filter );
    beeper_filter_active = filter_speaker;
  }
}

static double
filter_ula_sample( double sample, int filter_ula, int filter_speaker )
{
  /* The acoustic built-in-speaker model follows the electrical ULA response;
   * it never affects the MIC-socket path. */
  if( filter_ula )
    sample = ula_filter_apply( &ula_filter, sample );
  else
    sample = dc_filter_apply( &unfiltered_dc_filter, sample );
  if( filter_speaker ) sample = speaker_filter_apply( &beeper_filter, sample );
  return sample;
}

static long
clip_sample( double sample )
{
  if( sample > 0x7fff ) return 0x7fff;
  if( sample < -0x8000 ) return -0x8000;
  return sample;
}

static void
mix_channel( blip_sample_t *main_samples, long offset, int channel,
             int speaker_type, double ula_sample )
{
  double routed_sample = ula_sample;
  if( speaker_type == SOUND_SPEAKER_TYPE_TV ) {
    if( offset < tv_output_count ) routed_sample += tv_samples[offset];
    routed_sample = tv_filter_apply( &tv_filters[channel], routed_sample );
  }
  main_samples[offset] = clip_sample( main_samples[offset] + routed_sample );
}

static void
mix_output( blip_sample_t *main_samples, long count )
{
  int filter_speaker, filter_ula;
  int speaker_type = output_mixer_speaker_type();
  long frames = channels == 2 ? count / 2 : count;
  long i;

  select_ula_filters( speaker_type, &filter_ula, &filter_speaker );
  reset_route_filters( speaker_type, filter_ula, filter_speaker );
  for( i = 0; i < frames && i < ula_output_count; i++ ) {
    double ula_sample = filter_ula_sample( ula_samples[i], filter_ula,
                                           filter_speaker );
    int channel;
    for( channel = 0; channel < channels; channel++ )
      mix_channel( main_samples, i * channels + channel, channel, speaker_type,
                   ula_sample );
  }
}

void
output_mixer_advance( libspectrum_dword elapsed,
                      blip_sample_t *main_samples, long count )
{
  blip_buffer_end_frame( tv_left_buf, elapsed );
  blip_buffer_end_frame( ula_buf, elapsed );
  if( channels == 2 ) {
    blip_buffer_end_frame( tv_right_buf, elapsed );
    tv_output_count = blip_buffer_read_samples( tv_left_buf, tv_samples,
                                                count / 2, 1 );
    blip_buffer_read_samples( tv_right_buf, tv_samples + 1,
                              tv_output_count, 1 );
    tv_output_count <<= 1;
  } else {
    tv_output_count = blip_buffer_read_samples( tv_left_buf, tv_samples,
                                                count, 0 );
  }
  ula_output_count = blip_buffer_read_samples( ula_buf, ula_samples,
                                               count / channels, 0 );
  mix_output( main_samples, count );
  if( ula_output_count > ula_frame_size - ula_frame_count ) fuse_abort();
  memcpy( ula_frame + ula_frame_count, ula_samples,
          ula_output_count * sizeof( *ula_samples ) );
  ula_frame_count += ula_output_count;
}

void
output_mixer_end_frame( void )
{
  memcpy( ula_published, ula_frame, ula_frame_count * sizeof( *ula_frame ) );
  ula_published_count = ula_frame_count;
  ula_published_speaker_type = synth_speaker_type;
  ula_frame_count = 0;
}

void
sound_ula_levels( int mic_on, int beeper_on, int *mic_ampl, int *beeper_ampl )
{
  *mic_ampl = ( mic_on ? SOUND_AMPL_TAPE : 0 ) +
              ( beeper_on ? SOUND_AMPL_BEEPER : 0 );
  *beeper_ampl = beeper_on ? SOUND_AMPL_BEEPER +
                 ( mic_on ? SOUND_AMPL_TAPE : 0 ) : 0;
}

static ula_levels_t
current_ula_levels( void )
{
  ula_levels_t levels;
  /* Tape input is combined at the ULA node, while the raw MIC latch remains
   * separate for tape saving and subsequent edge handling. */
  int mic_on = ula_mic_on || tape_microphone;

  sound_ula_levels( mic_on, ula_beeper_on, &levels.mic, &levels.beeper );
  /* Preserve the legacy loading-noise policy. While a tape is playing, its
   * input drives the internal-speaker path unless loading sound is disabled
   * or this is a Timex machine. The electrical MIC output remains intact. */
  if( tape_is_playing() ) {
    if( settings_current.sound_load && !machine_current->timex )
      levels.beeper = levels.mic;
    else
      levels.beeper = ula_beeper_on ? SOUND_AMPL_BEEPER : 0;
  }

  return levels;
}

static void
ula_update( libspectrum_dword at_tstates )
{
  ula_levels_t levels = current_ula_levels();
  int speaker_type = output_mixer_speaker_type();

  if( !sound_enabled ) return;

  if( speaker_type != synth_speaker_type ) {
    /* The inactive logical path is not rendered. Discard old Blip history
     * before selecting the new path instead of replaying stale transitions. */
    blip_buffer_clear( ula_buf, BLIP_BUFFER_DEF_ENTIRE_BUFF );
    blip_synth_set_output( ula_synth, ula_buf );
    synth_speaker_type = speaker_type;
  }
  blip_synth_update( ula_synth, sound_interval_time( at_tstates ),
                     speaker_type == SOUND_SPEAKER_TYPE_BEEPER ?
                     levels.beeper : levels.mic );
}

void
sound_ula( libspectrum_dword at_tstates, int mic_on, int beeper_on )
{
  ula_mic_on = !!mic_on;
  ula_beeper_on = !!beeper_on;
  ula_update( at_tstates );
}

void
sound_tape( libspectrum_dword at_tstates )
{
  ula_update( at_tstates );
}

const libspectrum_signed_word *
sound_ula_mic_output( void )
{
  return ula_published_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ?
           NULL : ula_published;
}

const libspectrum_signed_word *
sound_ula_beeper_output( void )
{
  return ula_published_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ?
           ula_published : NULL;
}

int
sound_ula_mic_output_count( void )
{
  return ula_published_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ?
         0 : ula_published_count;
}

int
sound_ula_beeper_output_count( void )
{
  return ula_published_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ?
         ula_published_count : 0;
}
