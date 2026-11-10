/* blippreviewtest.c: non-mutating Blip timeline preview tests

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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sound/blipbuffer.h"

#define CHECK(x) do { if( !(x) ) { \
  fprintf( stderr, "blippreviewtest:%d: %s\n", __LINE__, #x ); exit( 1 ); \
} } while( 0 )

static void
preview( int stereo, long rate, long clock, long duration, long limit )
{
  Blip_Buffer *buffer = new_Blip_Buffer();
  Blip_Synth *synth = new_Blip_Synth();
  blip_sample_t output[4096];
  int i;
  CHECK( buffer && synth );
  blip_buffer_set_clock_rate( buffer, clock );
  CHECK( !blip_buffer_set_sample_rate( buffer, rate, 1000 ) );
  blip_synth_set_volume( synth, 0.5 );
  blip_synth_set_output( synth, buffer );
  for( i = 0; i < 2000; i++ ) {
    Blip_Buffer snapshot = *buffer;
    long time = i % 11 ? duration : 0;
    long predicted = blip_buffer_samples_after( buffer, time );
    CHECK( !memcmp( &snapshot, buffer, sizeof( snapshot ) ) );
    CHECK( predicted >= 0 );
    blip_synth_update( synth, 0, i & 1 ? 1000 : -1000 );
    CHECK( blip_buffer_samples_after( buffer, time ) == predicted );
    blip_buffer_end_frame( buffer, time );
    CHECK( blip_buffer_samples_avail( buffer ) == predicted );
    if( predicted > limit ) predicted = limit;
    CHECK( blip_buffer_read_samples( buffer, output, limit, stereo ) == predicted );
    /* Drain any capped unread samples; the next preview retains only fraction. */
    while( blip_buffer_samples_avail( buffer ) )
      blip_buffer_read_samples( buffer, output, limit, stereo );
  }
  blip_buffer_clear( buffer, BLIP_BUFFER_DEF_ENTIRE_BUFF );
  CHECK( blip_buffer_samples_after( buffer, 0 ) == 0 );
  CHECK( blip_buffer_samples_after( buffer, -1 ) == -1 );
  buffer->offset_ = ULONG_MAX;
  CHECK( blip_buffer_samples_after( buffer, 1 ) == -1 );
  delete_Blip_Synth( &synth );
  delete_Blip_Buffer( &buffer );
}

int main( void )
{
  int stereo;
  Blip_Buffer buffer = { 0 };
  long i, previous = -1;
  CHECK( blip_buffer_samples_after( NULL, 0 ) == -1 );
  CHECK( blip_buffer_samples_after( &buffer, 0 ) == -1 );
  /* Explicit residue boundaries and alternating 958/959-frame batches. */
  buffer.factor_ = 1;
  buffer.offset_ = ( 1UL << BLIP_BUFFER_ACCURACY ) - 1;
  CHECK( blip_buffer_samples_after( &buffer, 0 ) == 0 );
  CHECK( blip_buffer_samples_after( &buffer, 1 ) == 1 );
  buffer.factor_ = 1UL << ( BLIP_BUFFER_ACCURACY - 1 );
  buffer.offset_ = 0;
  for( i = 0; i < 100; i++ ) {
    long count = blip_buffer_samples_after( &buffer, 1917 );
    CHECK( count == ( i & 1 ? 959 : 958 ) && count != previous );
    previous = count;
    blip_buffer_end_frame( &buffer, 1917 );
    blip_buffer_remove_silence( &buffer, count );
  }
  for( stereo = 0; stereo < 2; stereo++ ) {
    preview( stereo, 48000, 3500000, 69888, 960 );
    preview( stereo, 44100, 3546900, 70908, 882 );
    preview( stereo, 48000, 3500000, 69888, 17 );
  }
  puts( "Blip preview tests passed" );
  return 0;
}
