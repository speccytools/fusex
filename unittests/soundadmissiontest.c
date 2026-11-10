/* soundadmissiontest.c: frame-end synthesis equivalence with/without admission

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
#include <setjmp.h>
#define FUSE_SOUND_TEST_INTERVAL 0
#include "sound.c"
#include "event.c"
int rzx_playback;
enum debugger_mode_t debugger_mode = DEBUGGER_MODE_INACTIVE;
#include "sound/pcm_fifo.h"
#include "periph.h"

#define CHECK(x) do { if( !(x) ) { \
  fprintf( stderr, "soundadmissiontest:%d: %s\n", __LINE__, #x ); exit( 1 ); \
} } while( 0 )

static fuse_machine_info test_machine;
fuse_machine_info *machine_current = &test_machine;
settings_info settings_current;
int movie_recording = 1, tape_microphone;
libspectrum_dword tstates;
int periph_is_active( periph_type type ) { (void)type; return 1; }
void startup_manager_register( startup_manager_module module,
  startup_manager_module *dependencies, size_t count,
  startup_manager_init_fn init, void *context, startup_manager_end_fn end )
{
  (void)module; (void)dependencies; (void)count;
  (void)init; (void)context; (void)end;
}
static sfifo_t fifo;
static unsigned int width, reserved, reserve_calls, published;
static int speaker, selected_stereo, deny_admission, speech_frames;
static jmp_buf failed_admission;
static blip_sample_t captured[4096], expected[100][4096];
static int captured_count, expected_count[100];
static blip_resampled_time_t expected_offset[100];

int option_enumerate_sound_stereo_ay( void ) { return selected_stereo; }
int option_enumerate_sound_speaker_type( void ) { return speaker; }
int timer_fastloading_active( void ) { return 0; }
int tape_is_playing( void ) { return 0; }
void sp0256_end_frame( void ) {}
void sp0256_advance_to( libspectrum_dword endpoint )
{
  (void)endpoint;
  speech_frames++;
  if( settings_current.sound ) CHECK( reserved );
}
int ui_error( ui_error_level level, const char *message, ... )
{ (void)level; (void)message; return 0; }
void fuse_abort( void )
{
  if( deny_admission ) longjmp( failed_admission, 1 );
  abort();
}
void movie_init_sound( int frequency, int stereo )
{ (void)frequency; (void)stereo; }
void movie_add_sound( libspectrum_signed_word *data, int count )
{
  CHECK( count <= 4096 );
  captured_count = count;
  memcpy( captured, data, count * sizeof( *data ) );
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
  if( deny_admission ) return -EINVAL;
  if( !pcm_fifo_producer_can_write( &fifo, width, frames ) ) {
    Blip_Buffer before = *left_buf;
    blip_sample_t discarded[16384];
    int used = pcm_fifo_consumer_used( &fifo, width );
    int speech_before = speech_frames;
    CHECK( pcm_fifo_read( &fifo, width, discarded, used ) == used );
    CHECK( !memcmp( &before, left_buf, sizeof( before ) ) );
    CHECK( speech_frames == speech_before );
  }
  CHECK( pcm_fifo_producer_can_write( &fifo, width, frames ) == 1 );
  CHECK( frames == (unsigned int)blip_buffer_samples_after(
                    left_buf, machine_current->timings.tstates_per_frame ) );
  reserved = frames;
  reserve_calls++;
  return 0;
}
void sound_lowlevel_frame( libspectrum_signed_word *data, int count )
{
  blip_sample_t output[4096];
  unsigned int frames = count / sound_channels;
  CHECK( frames == reserved );
  CHECK( pcm_fifo_write( &fifo, width, data, frames ) == frames );
  CHECK( pcm_fifo_read( &fifo, width, output, frames ) == frames );
  CHECK( !memcmp( output, data, count * sizeof( *data ) ) );
  reserved = 0;
  published++;
}

int main( void )
{
  int stereo, route, pass, frame;
  test_machine.timings.processor_speed = 3500000;
  test_machine.timings.tstates_per_frame = 69888;
  test_machine.capabilities = LIBSPECTRUM_MACHINE_CAPABILITY_AY |
                              LIBSPECTRUM_MACHINE_CAPABILITY_BEEPER;
  settings_current.emulation_speed = 100;
  settings_current.sound_freq = 48000;
  settings_current.volume_beeper = settings_current.volume_ay = 70;
  settings_current.volume_specdrum = settings_current.volume_covox = 50;
  settings_current.volume_uspeech = 50;
  for( stereo = 0; stereo < 2; stereo++ ) {
    selected_stereo = stereo ? SOUND_STEREO_AY_ABC : SOUND_STEREO_AY_NONE;
    for( route = 0; route < 3; route++ ) {
      speaker = route == 0 ? SOUND_SPEAKER_TYPE_UNFILTERED :
                route == 1 ? SOUND_SPEAKER_TYPE_BEEPER : SOUND_SPEAKER_TYPE_TV;
      for( pass = 0; pass < 2; pass++ ) {
        settings_current.sound = 1;
        sound_init( NULL );
        CHECK( sound_enabled );
        /* A fixed route avoids retaining unrelated ULA logical levels across
           sound_end. Explicitly reset the source/filter state for both passes. */
        sound_ula( 0, 0, 0 );
        sound_ay_reset();
        reserve_calls = published = reserved = 0;
        if( pass ) {
          Blip_Buffer before = *left_buf;
          int speech_before = speech_frames;
          deny_admission = 1;
          if( !setjmp( failed_admission ) ) { sound_frame(); CHECK( 0 ); }
          deny_admission = 0;
          CHECK( !memcmp( &before, left_buf, sizeof( before ) ) );
          CHECK( speech_frames == speech_before && !published );
        }
        for( frame = 0; frame < 100; frame++ ) {
          sound_ula( 100, frame & 1, !( frame & 1 ) );
          sound_ay_write( 0, 17 + frame % 13, 500 );
          sound_ay_write( 7, 0x3e, 501 );
          sound_ay_write( 8, 12, 502 );
          sound_sp0256_write( 1000, ( frame % 9 - 4 ) * 100 );
          /* Disabling device publication alone is the preceding synthesis
             path: it skips admission but retains identical source operations. */
          settings_current.sound = pass;
          if( pass ) {
            blip_sample_t queued[16384] = { 0 };
            int upcoming = blip_buffer_samples_after(
                             left_buf, test_machine.timings.tstates_per_frame );
            int fill = pcm_fifo_capacity( &fifo, width ) - upcoming + 1;
            CHECK( pcm_fifo_write( &fifo, width, queued, fill ) == fill );
            CHECK( pcm_fifo_producer_can_write( &fifo, width, upcoming ) == 0 );
          }
          sound_frame();
          settings_current.sound = 1;
          if( !pass ) {
            expected_count[frame] = captured_count;
            memcpy( expected[frame], captured, sizeof( captured ) );
            expected_offset[frame] = left_buf->offset_;
          } else {
            CHECK( captured_count == expected_count[frame] );
            CHECK( !memcmp( captured, expected[frame],
                             captured_count * sizeof( *captured ) ) );
            CHECK( left_buf->offset_ == expected_offset[frame] );
          }
        }
        CHECK( reserve_calls == ( pass ? 100 : 0 ) );
        CHECK( published == reserve_calls );
        sound_end();
        CHECK( !sound_enabled && !fifo.buffer );
      }
    }
  }
  puts( "Frame-end admitted/bypass synthesis PCM equivalence passed" );
  return 0;
}
