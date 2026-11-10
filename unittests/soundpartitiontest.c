/* soundpartitiontest.c: Audio scheduler, discontinuity and admission regressions
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
/* Exercise physical admission even in a build using a non-FIFO backend. */
#ifndef SOUND_PCM_ADMISSION
#define SOUND_PCM_ADMISSION 1
#endif
#define FUSE_SOUND_TEST_INTERVAL 0
#include "sound.c"
#include "event.c"
int rzx_playback;
enum debugger_mode_t debugger_mode = DEBUGGER_MODE_INACTIVE;
/* Include the implementations to inspect private synthesis/filter state.
   No test entry points or state-observation hooks enter production. */
#include "sound/ay_engine.c"
#include "sound/source_synths.c"
#include "sound/output_mixer.c"
#include "peripherals/sound/sp0256.c"
#include "sound/pcm_fifo.h"

#define CHECK(x) do { if( !(x) ) { \
  fprintf( stderr, "soundpartitiontest:%d: %s (case %d/%d/%d frame %d)\n", \
           __LINE__, #x, test_stereo, test_route, test_sources, test_frame ); \
  exit( 1 ); } } while( 0 )
#define CAPACITY 4096
#define FRAMES 12

static fuse_machine_info test_machine;
fuse_machine_info *machine_current = &test_machine;
settings_info settings_current;
int movie_recording = 1, tape_microphone;
libspectrum_dword tstates;
static int test_stereo, test_route, test_sources, test_frame;
static sfifo_t fifo;
static int speaker, selected_stereo, deny_admission;
static int pacing_queued, pacing_wait;
static uint64_t pacing_wait_state;
static uint64_t state( void );
static unsigned int width, reserved, reserve_calls, output_calls, movie_calls;
static libspectrum_dword preview_elapsed;
static int scheduled_test;
static unsigned long scheduled_advances, maximum_batch;
static int characterize;
static unsigned long all_advances, all_maximum_batch;
static libspectrum_dword last_boundary, minimum_interval, maximum_interval;
static blip_sample_t device_pcm[CAPACITY], movie_pcm[CAPACITY];
static int device_count, movie_count;
static blip_sample_t expected_pcm[FRAMES][CAPACITY];
static int expected_count[FRAMES];
static uint64_t expected_state[FRAMES];
static unsigned long total_intervals;

int periph_is_active( periph_type type )
{
  return type == PERIPH_TYPE_SPECDRUM || type == PERIPH_TYPE_COVOX_FB;
}
void startup_manager_register( startup_manager_module module,
  startup_manager_module *dependencies, size_t count,
  startup_manager_init_fn init, void *context, startup_manager_end_fn end )
{
  (void)module; (void)dependencies; (void)count;
  (void)init; (void)context; (void)end;
}
int option_enumerate_sound_stereo_ay( void ) { return selected_stereo; }
int option_enumerate_sound_speaker_type( void ) { return speaker; }
int timer_fastloading_active( void ) { return 0; }
int tape_is_playing( void ) { return 0; }
int ui_error( ui_error_level level, const char *message, ... )
{ (void)level; (void)message; return 0; }
void fuse_abort( void ) { abort(); }
void movie_init_sound( int frequency, int stereo )
{ (void)frequency; (void)stereo; }
void movie_add_sound( libspectrum_signed_word *data, int count )
{
  CHECK( ++movie_calls == 1 && count <= CAPACITY );
  movie_count = count;
  memcpy( movie_pcm, data, count * sizeof( *data ) );
}
int sound_lowlevel_init( const char *device, int *frequency, int *stereo )
{
  (void)device; (void)frequency;
  width = *stereo ? 4 : 2;
  return sfifo_init( &fifo, 32767 );
}
void sound_lowlevel_end( void ) { sfifo_close( &fifo ); }
int sound_lowlevel_reserve( unsigned int frames )
{
  CHECK( frames > 0 );
  if( scheduled_test ) {
    libspectrum_dword interval = tstates - last_boundary;
    preview_elapsed = tstates - audio_position;
    if( interval < minimum_interval ) minimum_interval = interval;
    if( interval > maximum_interval ) maximum_interval = interval;
    last_boundary = tstates;
    scheduled_advances++;
    if( frames > maximum_batch ) maximum_batch = frames;
  }
  if( characterize ) {
    all_advances++;
    if( frames > all_maximum_batch ) all_maximum_batch = frames;
  }
  {
    long preview = blip_buffer_samples_after( left_buf, preview_elapsed );
    long remaining = sound_framesiz - frame_sample_count / sound_channels;
    if( preview > remaining ) preview = remaining;
    CHECK( frames == preview );
  }
  if( pacing_wait ) {
    blip_sample_t consumed[2];
    CHECK( pcm_fifo_producer_can_write( &fifo, width, frames ) == 0 );
    CHECK( state() == pacing_wait_state );
    /* Deterministic consumer progress while the producer is in admission. */
    CHECK( pcm_fifo_read( &fifo, width, consumed, 1 ) == 1 );
    pacing_queued--;
    CHECK( state() == pacing_wait_state );
    pacing_wait = 0;
  }
  if( deny_admission ) {
    CHECK( pcm_fifo_producer_can_write( &fifo, width, frames ) == 0 );
    return -EAGAIN;
  }
  CHECK( pcm_fifo_producer_can_write( &fifo, width, frames ) == 1 );
  /* Test transport: discard the older queued fixture only AFTER admission. */
  if( pacing_queued ) {
    blip_sample_t queued[16384];
    CHECK( pcm_fifo_read( &fifo, width, queued, pacing_queued ) == pacing_queued );
    pacing_queued = 0;
  }
  reserved = frames;
  reserve_calls++;
  return 0;
}
void sound_lowlevel_frame( libspectrum_signed_word *data, int count )
{
  blip_sample_t readback[CAPACITY];
  int frames = count / sound_channels;
  CHECK( frames == (int)reserved && frames > 0 );
  CHECK( pcm_fifo_write( &fifo, width, data, frames ) == frames );
  CHECK( pcm_fifo_read( &fifo, width, readback, frames ) == frames );
  CHECK( !memcmp( readback, data, count * sizeof( *data ) ) );
  CHECK( device_count + count <= CAPACITY );
  memcpy( device_pcm + device_count, data, count * sizeof( *data ) );
  device_count += count;
  reserved = 0;
  output_calls++;
}

static uint64_t
hash_bytes( uint64_t hash, const void *data, size_t count )
{
  const unsigned char *p = data;
  while( count-- ) { hash ^= *p++; hash *= UINT64_C(1099511628211); }
  return hash;
}
#define HASH(x) hash = hash_bytes( hash, &(x), sizeof( x ) )
static uint64_t
buffer_state( uint64_t hash, Blip_Buffer *buffer )
{
  if( !buffer ) return hash;
  hash = hash_bytes( hash, &buffer->offset_, sizeof( buffer->offset_ ) );
  hash = hash_bytes( hash, &buffer->reader_accum, sizeof( buffer->reader_accum ) );
  /* At safe cuts all delivered events are at or before the endpoint. After
     extraction only fractional residue and this impulse tail remain live. */
  return hash_bytes( hash, buffer->buffer_,
                     ( blip_buffer_samples_avail( buffer ) +
                       BLIP_WIDEST_IMPULSE_ + 2 ) *
                     sizeof( *buffer->buffer_ ) );
}
static uint64_t
state( void )
{
  uint64_t hash = UINT64_C(14695981039346656037);
  HASH( registers ); HASH( tone_tick ); HASH( tone_high ); HASH( tone_period );
  HASH( noise_period ); HASH( env_period ); HASH( noise_tick );
  HASH( tone_cycles ); HASH( env_cycles ); HASH( env_internal_tick );
  HASH( env_tick ); HASH( rng ); HASH( noise_toggle ); HASH( env_first );
  HASH( env_reverse ); HASH( env_counter ); HASH( change_count );
  HASH( change_position ); HASH( next_tick ); HASH( previous );
  HASH( unfiltered_dc_filter ); HASH( beeper_filter ); HASH( tv_filters );
  HASH( ula_filter ); HASH( audio_position ); HASH( frame_sample_count );
  HASH( ula_frame_count ); HASH( ula_published_count );
  /* Scratch ring cursors describe batching, not speech state: each run
     discards the old scratch contents. Compare the LPC state and sequencer. */
  HASH( sp0256.filt ); HASH( sp0256.sound_current );
  HASH( sp0256.pc ); HASH( sp0256.ald ); HASH( sp0256.halted );
  HASH( sp0256.lrq ); HASH( sp0256.mode ); HASH( sp0256.page );
  HASH( sp0256.silent ); HASH( sp0256.fifo_head ); HASH( sp0256.fifo_tail );
  HASH( sp0256.fifo_bitp );
  {
    uint64_t left = buffer_state( 0, left_buf );
    uint64_t right = buffer_state( 0, right_buf );
    uint64_t ula = buffer_state( 0, ula_buf );
    uint64_t tv_left = buffer_state( 0, tv_left_buf );
    uint64_t tv_right = buffer_state( 0, tv_right_buf );
    HASH( left ); HASH( right ); HASH( ula ); HASH( tv_left ); HASH( tv_right );
  }
  for( int channel = 0; channel < AY_CHANNELS; channel++ ) {
    HASH( synths[channel]->impl.last_amp );
    if( right_synths[channel] ) HASH( right_synths[channel]->impl.last_amp );
  }
  HASH( ula_synth->impl.last_amp );
  HASH( left_sp0256->impl.last_amp );
  HASH( left_covox->impl.last_amp ); HASH( left_specdrum->impl.last_amp );
  return hash;
}

static const blip_sample_t *
raw_output( int *count )
{
  if( speaker == SOUND_SPEAKER_TYPE_BEEPER ) {
    *count = sound_ula_beeper_output_count();
    CHECK( !sound_ula_mic_output_count() && !sound_ula_mic_output() );
    return sound_ula_beeper_output();
  }
  *count = sound_ula_mic_output_count();
  CHECK( !sound_ula_beeper_output_count() && !sound_ula_beeper_output() );
  return sound_ula_mic_output();
}

static void
advance( libspectrum_dword endpoint )
{
  const int old_calls = reserve_calls;
  int raw_count;
  const blip_sample_t *raw = raw_output( &raw_count );
  CHECK( raw_count <= CAPACITY );
  const int old_count = raw_count;
  blip_sample_t saved[CAPACITY];
  memcpy( saved, raw, raw_count * sizeof( *raw ) );
  preview_elapsed = endpoint - audio_position;
  long upcoming = blip_buffer_samples_after( left_buf, preview_elapsed );
  if( upcoming > sound_framesiz - frame_sample_count / sound_channels )
    upcoming = sound_framesiz - frame_sample_count / sound_channels;
  const uint64_t before = state();
  CHECK( sound_advance_to( audio_position ) == 0 && state() == before );
  CHECK( sound_advance_to( test_machine.timings.tstates_per_frame + 1 ) < 0 );
  CHECK( state() == before );
  if( audio_position ) {
    CHECK( sound_advance_to( audio_position - 1 ) < 0 );
    CHECK( state() == before );
  }
  if( upcoming && ( total_intervals % 127 == 0 ||
                    endpoint == 34944 || endpoint == 69888 ) ) {
    blip_sample_t queued[16384] = { 0 };
    int capacity = pcm_fifo_capacity( &fifo, width );
    CHECK( pcm_fifo_write( &fifo, width, queued, capacity ) == capacity );
    deny_admission = 1;
    CHECK( sound_advance_to( endpoint ) < 0 );
    deny_admission = 0;
    CHECK( state() == before && !movie_calls );
    CHECK( pcm_fifo_consumer_used( &fifo, width ) == capacity );
    CHECK( pcm_fifo_read( &fifo, width, queued, capacity ) == capacity );
  }
  CHECK( sound_advance_to( endpoint ) == 0 );
  CHECK( reserve_calls == old_calls + !!upcoming );
  CHECK( !movie_calls && frame_sample_count == device_count );
  CHECK( raw == raw_output( &raw_count ) && raw_count == old_count );
  CHECK( !memcmp( saved, raw, raw_count * sizeof( *raw ) ) );
  CHECK( audio_position == endpoint );
  CHECK( blip_buffer_samples_avail( left_buf ) >= 0 );
  total_intervals++;
}

static void
sources_at( libspectrum_dword time )
{
  int f = test_frame;
  tstates = time;
  if( test_sources & 1 ) sound_ula( time, ( time / 97 + f ) & 1,
                                   ( time / 131 + f ) & 1 );
  if( test_sources & 2 ) {
    sound_ay_write( 0, 17 + ( time + f ) % 29, time );
    sound_ay_write( 1, 0, time );
    sound_ay_write( 6, 7, time );
    sound_ay_write( 7, 0x30, time );
    sound_ay_write( 8, 12, time );
    sound_ay_write( 9, 16, time );
    sound_ay_write( 11, 3, time );
    sound_ay_write( 13, ( f + time / 500 ) & 15, time );
  }
  if( test_sources & 8 ) {
    sound_specdrum_write( 0, ( time + f ) & 255 );
    sound_covox_write( 0, ( time / 13 + f ) & 255 );
  }
}

static void
initialise( void )
{
  static libspectrum_byte rom[4096];
  settings_current.sound = 1;
  sound_init( NULL );
  CHECK( sound_enabled && audio_position == 0 && !frame_sample_count );
  /* Existing AY lifecycle intentionally retains some oscillator state.
     Restore the same initial machine state for each independent comparison. */
  rng = 1; noise_toggle = 0; env_first = 1; env_reverse = 0;
  env_counter = AY_ENV_STEPS - 1;
  sound_ula( 0, 0, 0 );
  sound_ay_reset();
  if( test_sources & 4 ) {
    CHECK( sp0256_init( rom ) == 0 );
    /* Real LPC synthesis, both periodic and noise excitation, with enough
       repeats to span the fixture. No ROM file or real device is needed. */
    sp0256.silent = 0;
    sp0256.filt.rpt = 100000;
    sp0256.filt.per = test_route == 2 ? 0 : 17;
    sp0256.filt.amp = 128;
    sp0256.filt.b_coef[0] = -80;
    sp0256.filt.f_coef[0] = 100;
  }
}

/* Actual sound_advance_to with physical space smaller than sound_framesiz.
   Consumer progress is deterministic; production cannot touch protected state
   while the exact request is unfunded. The backend polling loops are covered
   independently by the real Core Audio and SDL2 backend tests. */
static void
test_physical_pacing( void )
{
  static const unsigned int endpoints[] = { 35000, 65000, 69888 };
  blip_sample_t queued[16384] = { 0 };
  test_sources = 7;
  test_route = 2;
  speaker = SOUND_SPEAKER_TYPE_TV;
  selected_stereo = SOUND_STEREO_AY_ABC;
  test_machine.capabilities |= LIBSPECTRUM_MACHINE_CAPABILITY_AY;
  test_frame = 0;
  for( unsigned int i = 0; i < ARRAY_SIZE( endpoints ); i++ ) {
    for( int blocked = 0; blocked < 2; blocked++ ) {
      initialise();
      device_count = movie_calls = movie_count = 0;
      reserve_calls = output_calls = reserved = 0;
      sources_at( 0 );
      preview_elapsed = endpoints[i];
      unsigned int batch = blip_buffer_samples_after( left_buf, preview_elapsed );
      if( batch > (unsigned int)sound_framesiz ) batch = sound_framesiz;
      int capacity = pcm_fifo_capacity( &fifo, width );
      pacing_queued = capacity - batch + blocked;
      CHECK( pcm_fifo_write( &fifo, width, queued, pacing_queued ) == pacing_queued );
      uint64_t before = state();
      if( blocked ) {
        deny_admission = 1;
        CHECK( sound_advance_to( endpoints[i] ) < 0 );
        deny_admission = 0;
        CHECK( state() == before && !output_calls && !reserve_calls );
        CHECK( pcm_fifo_consumer_used( &fifo, width ) == pacing_queued );
        CHECK( pcm_fifo_read( &fifo, width, queued, 1 ) == 1 );
        pacing_queued--;
      }
      CHECK( pcm_fifo_producer_space( &fifo, width ) == (int)batch );
      if( !i ) CHECK( batch < (unsigned int)sound_framesiz );
      CHECK( sound_advance_to( endpoints[i] ) == 0 );
      CHECK( audio_position == endpoints[i] && device_count == (int)batch * sound_channels );
      CHECK( reserve_calls == 1 && output_calls == 1 && !pacing_queued );
      sp0256_end();
      sound_end();
    }
  }
  puts( "physical pacing: exact fit, blocked/retry, overshoot and frame remainder passed" );
}

/* Discontinuities are not compared with an unreset frame. Both runs reset
   at the same committed endpoint, then vary only the partitioning around it. */
static void
test_discontinuities( void )
{
  uint64_t saved_state = 0;
  blip_sample_t saved_pcm[CAPACITY], saved_raw[CAPACITY];
  int saved_count = 0, saved_raw_count = 0;
  test_sources = 7;
  test_route = 2;
  speaker = SOUND_SPEAKER_TYPE_TV;
  selected_stereo = SOUND_STEREO_AY_ABC;
  test_machine.capabilities |= LIBSPECTRUM_MACHINE_CAPABILITY_AY;
  test_frame = 0;
  for( int pass = 0; pass < 2; pass++ ) {
    initialise();
    device_count = movie_calls = movie_count = 0;
    reserve_calls = output_calls = reserved = 0;
    sources_at( 0 );
    if( pass ) { advance( 1 ); advance( 31 ); advance( 999 ); }
    advance( 1000 );
    sound_ay_reset();
    CHECK( audio_position == 1000 && !movie_calls );
    int raw_count;
    const blip_sample_t *raw = raw_output( &raw_count );
    CHECK( !raw_count );
    sources_at( 1001 );
    if( pass ) { advance( 1001 ); advance( 1002 ); advance( 2001 ); }
    preview_elapsed = 69888 - audio_position;
    sound_frame();
    raw = raw_output( &raw_count );
    CHECK( movie_calls == 1 && device_count == movie_count );
    CHECK( !memcmp( movie_pcm, device_pcm, device_count * sizeof( *device_pcm ) ) );
    if( !pass ) {
      saved_count = device_count;
      saved_raw_count = raw_count;
      memcpy( saved_pcm, device_pcm, device_count * sizeof( *device_pcm ) );
      memcpy( saved_raw, raw, raw_count * sizeof( *raw ) );
      saved_state = state();
    } else {
      CHECK( saved_count == device_count && saved_raw_count == raw_count );
      CHECK( !memcmp( saved_pcm, device_pcm, device_count * sizeof( *device_pcm ) ) );
      CHECK( !memcmp( saved_raw, raw, raw_count * sizeof( *raw ) ) );
      CHECK( saved_state == state() );
    }
    movie_calls = device_count = movie_count = 0;
    sources_at( 0 );
    advance( 1000 );
    sound_pause();
    CHECK( !sound_enabled && !audio_position && !frame_sample_count );
    CHECK( !movie_calls );
    sp0256_end();
    sound_unpause();
    CHECK( sound_enabled && !audio_position && !frame_sample_count );
    raw_output( &raw_count );
    CHECK( !raw_count );
    preview_elapsed = 69888;
    sound_frame();
    CHECK( movie_calls == 1 );
    sound_end();
  }
}

/* Adapted from sound_latency's actual event.c scheduler exercise. */
static int scheduler_source, scheduler_frame, edge_count;
static unsigned long overtaken_cuts, coalesced_cuts;

static void
observe_pending_cut( gpointer data, gpointer unused GCC_UNUSED )
{
  event_t *event = data;
  if( event->type == audio_event && event->tstates < tstates &&
      audio_cut_interval ) {
    overtaken_cuts++;
    coalesced_cuts += tstates / audio_cut_interval -
                      event->tstates / audio_cut_interval;
  }
}

static void
scheduler_edge( libspectrum_dword at, int type GCC_UNUSED,
                void *data GCC_UNUSED )
{
  CHECK( audio_position <= at );
  sound_ula( at, edge_count & 1, !( edge_count & 1 ) );
  sound_ay_write( 8, edge_count & 15, at );
  edge_count++;
  /* Tape callbacks may schedule another edge already behind CPU progress. */
  if( at == 110 ) event_add( 120, scheduler_source );
}

static void
scheduler_frame_end( libspectrum_dword at, int type GCC_UNUSED,
                     void *data GCC_UNUSED )
{
  CHECK( audio_position <= at && tstates >= at );
  event_frame( at );
  tstates -= at;
  scheduled_test = 0;
  preview_elapsed = at - audio_position;
  sound_frame();
  scheduled_test = 1;
  last_boundary = 0;
}

static void
test_scheduler_speed_limit( void )
{
  static const unsigned int speeds[] = { 400, 500 };
  unsigned int i, pass;
  libspectrum_dword maximum_interval;

  event_reset();
  settings_current.emulation_speed = MAX_SPEED_PERCENTAGE;
  initialise();
  maximum_interval = sound_audio_interval_tstates(
                       sound_get_effective_processor_speed() );
  CHECK( maximum_interval == 175 * MAX_SPEED_PERCENTAGE );
  sound_end();

  /* The cutoff is inclusive: the next percentage must not start a backend,
     generate PCM, or arm audio events even with a retained nonzero interval. */
  settings_current.emulation_speed = MAX_SPEED_PERCENTAGE + 1;
  audio_cut_interval = maximum_interval;
  device_count = 0;
  sound_unpause();
  CHECK( !sound_enabled && !fifo.buffer && settings_current.sound );
  CHECK( !sound_can_produce_subframe() );
  sound_schedule_audio();
  sound_frame();
  CHECK( !device_count && event_next_event == event_no_events );

  /* On non-Win32 these are real, sound-enabled configurations. Win32's lower
     cutoff makes frame-sized cadence unreachable; do not synthesize it there. */
  for( i = 0; i < sizeof( speeds ) / sizeof( speeds[0] ); i++ ) {
    if( speeds[i] > MAX_SPEED_PERCENTAGE ) continue;
    settings_current.emulation_speed = speeds[i];
    for( pass = 0; pass < 2; pass++ ) {
      event_reset();
      edge_count = 0;
      tstates = 0;
      audio_cut_interval = 0;
      initialise();
      if( pass ) {
        audio_cut_interval = sound_audio_interval_tstates(
                               sound_get_effective_processor_speed() );
        CHECK( audio_cut_interval == 175 * speeds[i] );
        CHECK( audio_cut_interval >= test_machine.timings.tstates_per_frame );
      }
      audio_frame_suspended = 0;
      scheduled_advances = 0;
      for( test_frame = 0; test_frame < FRAMES; test_frame++ ) {
        unsigned int outputs_before = output_calls;
        unsigned int reserves_before = reserve_calls;
        device_count = 0;
        sound_schedule_audio();
        CHECK( event_next_event == event_no_events );
        event_add( 110, scheduler_source );
        event_add( 60001, scheduler_source );
        event_add( 69888, scheduler_frame );
        scheduled_test = 1;
        tstates = 69920;
        event_do_events();
        CHECK( tstates == 32 && audio_position == 0 && !scheduled_advances );
        CHECK( output_calls == outputs_before + 1 &&
               reserve_calls == reserves_before + 1 && device_count > 0 );
        CHECK( event_next_event == event_no_events );
        if( !pass ) {
          expected_count[test_frame] = device_count;
          memcpy( expected_pcm[test_frame], device_pcm,
                  device_count * sizeof( *device_pcm ) );
          expected_state[test_frame] = state();
        } else {
          CHECK( device_count == expected_count[test_frame] );
          CHECK( !memcmp( device_pcm, expected_pcm[test_frame],
                           device_count * sizeof( *device_pcm ) ) );
          CHECK( state() == expected_state[test_frame] );
        }
      }
      scheduled_test = 0;
      sound_end();
    }
  }
  settings_current.emulation_speed = 100;
  audio_cut_interval = 0;
  printf( "scheduler speed cutoff: %u%% inclusive, maximum cadence %u; "
          "disabled gating and reachable frame-only PCM/state passed\n",
          MAX_SPEED_PERCENTAGE, maximum_interval );
}

static void
test_scheduler( void )
{
  CHECK( utils_safe_strdup( NULL ) == NULL );
  char *empty = utils_safe_strdup( "" );
  CHECK( empty && !*empty );
  libspectrum_free( empty );
  char source[] = "Audio cut";
  char *copy = utils_safe_strdup( source );
  CHECK( copy && copy != source && !strcmp( copy, source ) );
  source[0] = 'X';
  CHECK( copy[0] == 'A' );
  libspectrum_free( copy );

  event_init( NULL );
  scheduler_source = event_register( scheduler_edge, "Test tape" );
  scheduler_frame = event_register( scheduler_frame_end, "Test frame" );
  test_sources = 3;
  speaker = SOUND_SPEAKER_TYPE_TV;
  selected_stereo = SOUND_STEREO_AY_ABC;
  test_machine.capabilities = LIBSPECTRUM_MACHINE_CAPABILITY_AY;
  movie_recording = 0;
  audio_cut_interval = 100;
  initialise();
  tstates = 0;
  audio_frame_suspended = 0;
  sound_schedule_audio();
  event_add( 110, scheduler_source );
  scheduled_test = 1;
  minimum_interval = UINT_MAX;
  last_boundary = 0;
  device_count = movie_count = movie_calls = 0;
  /* Regression: neither nominal cut nor actual 140 may consume tape 110. */
  tstates = 140;
  event_do_events();
  CHECK( edge_count == 2 && audio_position == 140 && event_next_event == 200 );
  tstates = 200;
  event_do_events();
  CHECK( audio_position == 200 );
  /* Several nominal cuts overtaken: only one actual advancement. */
  unsigned long before = scheduled_advances;
  tstates = 540;
  event_do_events();
  CHECK( audio_position == 540 && event_next_event == 600 );
  CHECK( scheduled_advances == before + 1 ); /* Coalesced 300/400/500. */
  /* Equal-time sources on either side of the audio type are half-open. */
  int late_source = event_register( scheduler_edge, "Late source" );
  event_add( 600, scheduler_source );
  event_add( 600, late_source );
  tstates = 600;
  event_do_events();
  CHECK( audio_position == 600 && edge_count == 4 );
  /* Delayed event service creates a batch nearly a full frame long. Admit
     at exact B, not a cadence-derived bound, with protected state unchanged
     until one consumer frame makes the whole batch physically retainable. */
  unsigned int delayed_batch = blip_buffer_samples_after( left_buf, 69200 );
  CHECK( delayed_batch > (unsigned int)sound_framesiz / 2 );
  blip_sample_t queued[16384] = { 0 };
  pacing_queued = pcm_fifo_capacity( &fifo, width ) - delayed_batch + 1;
  CHECK( pcm_fifo_write( &fifo, width, queued, pacing_queued ) == pacing_queued );
  pacing_wait_state = state();
  pacing_wait = 1;
  tstates = 69800;
  event_do_events();
  CHECK( !pacing_wait && !pacing_queued );
  CHECK( audio_position == 69800 && event_next_event == event_no_events );
  /* Overtaken near-frame cut must not advance beyond F. */
  event_add( 69850, audio_event );
  event_add( 69870, scheduler_source );
  event_add( 69887, scheduler_source );
  event_add( 69888, scheduler_frame );
  tstates = 69920;
  event_do_events();
  CHECK( tstates == 32 && audio_position == 0 && event_next_event == 100 );
  CHECK( edge_count == 6 );
  device_count = 0;
  tstates = 140;
  event_do_events();
  CHECK( audio_position == 140 );
  sound_ay_reset();
  CHECK( audio_position == 0 && audio_frame_suspended );
  /* RZX forces nominal events without permitting interior advancement. */
  rzx_playback = 1;
  event_add( 200, audio_event );
  event_add( 210, scheduler_source );
  event_force_events();
  CHECK( audio_position == 0 && edge_count == 7 );
  rzx_playback = 0;
  scheduled_test = 0;
  sound_end();
  event_reset();
  initialise();
  CHECK( audio_frame_suspended && event_next_event == event_no_events );
  sound_end();
  audio_cut_interval = 0;
  /* Compare real event dispatch with frame-only production over consecutive
   * frames, preserving all fractional/filter/source state between frames. */
  static const unsigned int intervals[] = { 0, 35000, 17500, 8750, 997, 1, 100 };
  for( unsigned int pass = 0; pass < sizeof( intervals ) / sizeof( *intervals );
       pass++ ) {
    event_reset();
    edge_count = 0;
    tstates = 0;
    audio_cut_interval = 100;
    initialise();
    audio_cut_interval = intervals[pass];
    scheduled_advances = maximum_batch = 0;
    all_advances = all_maximum_batch = 0;
    characterize = 1;
    overtaken_cuts = coalesced_cuts = 0;
    minimum_interval = UINT_MAX;
    maximum_interval = last_boundary = 0;
    for( test_frame = 0; test_frame < FRAMES; test_frame++ ) {
      device_count = 0;
      event_add( 31, scheduler_source );
      event_add( 110, scheduler_source );
      event_add( 997, scheduler_source );
      event_add( 60001, scheduler_source );
      event_add( 69888, scheduler_frame );
      scheduled_test = 1;
      while( event_next_event <= 69888 ) {
        /* Instruction endpoint overshoot, not nominal event progress. */
        tstates = ( event_next_event / 23 + 1 ) * 23;
        event_foreach( observe_pending_cut, NULL );
        event_do_events();
        if( tstates < 23 ) break;
      }
      CHECK( audio_position == 0 );
      if( !pass ) {
        expected_count[test_frame] = device_count;
        memcpy( expected_pcm[test_frame], device_pcm,
                device_count * sizeof( *device_pcm ) );
        expected_state[test_frame] = state();
      } else {
        CHECK( expected_count[test_frame] == device_count );
        CHECK( !memcmp( expected_pcm[test_frame], device_pcm,
                         device_count * sizeof( *device_pcm ) ) );
        CHECK( expected_state[test_frame] == state() );
      }
    }
    scheduled_test = 0;
    characterize = 0;
    sound_end();
    if( pass ) printf( "scheduled %u: %lu interior nonzero batches, "
                       "intervals %u..%u, max exact B %lu, "
                       "overtaken %lu, coalesced %lu, "
                       "all batches %lu/max B %lu; PCM/state identical\n",
                       intervals[pass], scheduled_advances, minimum_interval,
                       maximum_interval, maximum_batch,
                       overtaken_cuts, coalesced_cuts,
                       all_advances, all_maximum_batch );
  }
  audio_cut_interval = 0;
  test_scheduler_speed_limit();
  movie_recording = 1;
  event_end();
  printf( "scheduler: tape/overshoot/ties/frame/rearm/RZX/reinit passed\n" );
}

int
main( int argc, char **argv )
{
  test_machine.timings.processor_speed = 3500000;
  test_machine.timings.tstates_per_frame = 69888;
  settings_current.emulation_speed = 100;
  settings_current.sound_freq = argc > 1 ? atoi( argv[1] ) : 48000;
  CHECK( settings_current.sound_freq >= 8000 && settings_current.sound_freq <= 96000 );
  settings_current.volume_beeper = settings_current.volume_ay = 70;
  settings_current.volume_specdrum = settings_current.volume_covox = 50;
  settings_current.volume_uspeech = 50;
  test_scheduler();
  if( argc > 2 && !strcmp( argv[2], "--scheduler-only" ) ) return 0;
  test_discontinuities();
  test_physical_pacing();
  return 0;
}
