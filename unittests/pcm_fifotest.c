/* pcm_fifotest.c: whole-frame geometry and transfer tests

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
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#include "sound/pcm_fifo.h"

#define CHECK(x) do { if( !(x) ) { \
  fprintf( stderr, "pcm_fifotest:%d: %s\n", __LINE__, #x ); exit( 1 ); \
} } while( 0 )

static void
geometry( unsigned int width )
{
  sfifo_t fifo;
  unsigned char input[256], output[260];
  int size, round, capacity, used, request, delivered;
  unsigned int i;

  for( i = 0; i < sizeof( input ); i++ ) input[i] = i + 1;
  for( size = 1; size <= 127; size++ ) {
    CHECK( !sfifo_init( &fifo, size ) );
    capacity = ( fifo.size - 1 ) / width;
    CHECK( pcm_fifo_capacity( &fifo, width ) == capacity );
    CHECK( pcm_fifo_producer_space( &fifo, width ) == capacity );
    CHECK( pcm_fifo_producer_can_write( &fifo, width, 0 ) == 1 );
    CHECK( pcm_fifo_producer_can_write( &fifo, width, capacity ) == 1 );
    CHECK( pcm_fifo_producer_can_write( &fifo, width, capacity + 1 ) == -EINVAL );
    /* Repeatedly cross the byte-ring boundary, including widths which do
       not divide the allocation, and the reserved-byte remainder. */
    for( round = 0; round < 40; round++ ) {
      CHECK( pcm_fifo_write( &fifo, width, input, capacity + 1 ) == capacity );
      CHECK( sfifo_consumer_used( &fifo ) == capacity * (int)width );
      CHECK( !pcm_fifo_write( &fifo, width, input, 1 ) );
      CHECK( pcm_fifo_producer_can_write( &fifo, width, 0 ) == 1 );
      CHECK( pcm_fifo_producer_can_write( &fifo, width, 1 ) ==
             ( capacity ? 0 : -EINVAL ) );
      CHECK( pcm_fifo_read( &fifo, width, output, capacity + 1 ) == capacity );
      CHECK( !memcmp( input, output, capacity * width ) );
      CHECK( !sfifo_consumer_used( &fifo ) );
    }
    /* Every attainable occupancy; requests smaller/equal/larger than it.
       The adapter leaves unread output untouched: silence is backend policy. */
    for( used = 0; used <= capacity; used++ ) {
      for( request = 0; request <= capacity + 1; request++ ) {
        CHECK( pcm_fifo_write( &fifo, width, input, used ) == used );
        CHECK( pcm_fifo_consumer_used( &fifo, width ) == used );
        CHECK( pcm_fifo_producer_space( &fifo, width ) == capacity - used );
        CHECK( pcm_fifo_producer_can_write( &fifo, width, request ) ==
               ( request > capacity ? -EINVAL : request <= capacity - used ) );
        memset( output, 0xa5, sizeof( output ) );
        delivered = used < request ? used : request;
        CHECK( pcm_fifo_read( &fifo, width, output, request ) == delivered );
        CHECK( !memcmp( input, output, delivered * width ) );
        for( i = delivered * width; i < sizeof( output ); i++ )
          CHECK( output[i] == 0xa5 );
        CHECK( sfifo_consumer_used( &fifo ) ==
               ( used - delivered ) * (int)width );
        /* Quiescent flush is deliberately not a PCM-specific operation. */
        sfifo_flush( &fifo );
      }
    }
    CHECK( pcm_fifo_write( &fifo, width, input, UINT_MAX ) == -EINVAL );
    CHECK( pcm_fifo_read( &fifo, width, output, UINT_MAX ) == -EINVAL );
    CHECK( !sfifo_consumer_used( &fifo ) );
    CHECK( pcm_fifo_write( &fifo, width, NULL, 1 ) == -EINVAL );
    CHECK( pcm_fifo_read( &fifo, width, NULL, 1 ) == -EINVAL );
    CHECK( !pcm_fifo_write( &fifo, width, NULL, 0 ) );
    CHECK( !pcm_fifo_read( &fifo, width, NULL, 0 ) );
    CHECK( pcm_fifo_producer_can_write( NULL, width, 0 ) == -EINVAL );
    CHECK( pcm_fifo_producer_can_write( &fifo, 0, 0 ) == -EINVAL );
    CHECK( pcm_fifo_producer_can_write( &fifo, UINT_MAX, 1 ) == -EINVAL );
    CHECK( pcm_fifo_producer_can_write( &fifo, width, UINT_MAX ) == -EINVAL );
    CHECK( pcm_fifo_capacity( &fifo, 0 ) == -EINVAL );
    CHECK( pcm_fifo_producer_space( &fifo, UINT_MAX ) == -EINVAL );
    CHECK( pcm_fifo_consumer_used( &fifo, 0 ) == -EINVAL );
    CHECK( pcm_fifo_write( &fifo, 0, input, 1 ) == -EINVAL );
    CHECK( pcm_fifo_read( &fifo, UINT_MAX, output, 1 ) == -EINVAL );
    sfifo_close( &fifo );
    CHECK( pcm_fifo_producer_can_write( &fifo, width, 0 ) == -ENODEV );
    CHECK( pcm_fifo_producer_can_write( &fifo, width, 1 ) == -ENODEV );
    CHECK( pcm_fifo_capacity( &fifo, width ) == -ENODEV );
    CHECK( pcm_fifo_producer_space( &fifo, width ) == -ENODEV );
    CHECK( pcm_fifo_consumer_used( &fifo, width ) == -ENODEV );
    CHECK( pcm_fifo_write( &fifo, width, input, 1 ) == -ENODEV );
    CHECK( pcm_fifo_read( &fifo, width, output, 1 ) == -ENODEV );
  }
}

/* One successful fit check funds split publication even when the consumer
   progresses after the observation. Repeat to cross byte-ring boundaries. */
static void
split_publication( unsigned int width )
{
  sfifo_t fifo;
  unsigned char input[128], output[128];
  int capacity, round, batch;
  memset( input, 0x5a, sizeof( input ) );
  CHECK( !sfifo_init( &fifo, 31 ) );
  capacity = pcm_fifo_capacity( &fifo, width );
  for( round = 0; round < 40; round++ ) {
    CHECK( pcm_fifo_write( &fifo, width, input, 3 ) == 3 );
    batch = capacity - 3;
    CHECK( pcm_fifo_producer_can_write( &fifo, width, batch ) == 1 );
    CHECK( pcm_fifo_read( &fifo, width, output, 2 ) == 2 );
    CHECK( pcm_fifo_write( &fifo, width, input, 1 ) == 1 );
    CHECK( pcm_fifo_write( &fifo, width, input, batch - 1 ) == batch - 1 );
    CHECK( pcm_fifo_consumer_used( &fifo, width ) == capacity - 2 );
    CHECK( pcm_fifo_read( &fifo, width, output, capacity ) == capacity - 2 );
    CHECK( !memcmp( input, output, ( capacity - 2 ) * width ) );
  }
  sfifo_close( &fifo );
}

#ifdef HAVE_PTHREAD
static sfifo_t threaded_fifo;
static unsigned int threaded_width;
static int threaded_fit;
#define TOTAL_FRAMES 200000u

static unsigned char
sequence( unsigned int frame, unsigned int byte )
{
  return ( frame ^ ( frame >> 8 ) ^ byte ) & 255;
}

static void *
produce( void *unused )
{
  unsigned char data[68];
  unsigned int pos = 0, count, i, j;
  int written, fits;
  (void)unused;
  while( pos < TOTAL_FRAMES ) {
    count = pos % 17 + 1;
    if( count > TOTAL_FRAMES - pos ) count = TOTAL_FRAMES - pos;
    if( threaded_fit ) {
      int capacity = pcm_fifo_capacity( &threaded_fifo, threaded_width );
      if( count > (unsigned int)capacity ) count = capacity;
      fits = pcm_fifo_producer_can_write( &threaded_fifo, threaded_width, count );
      CHECK( fits >= 0 );
      if( !fits ) continue;
    }
    for( i = 0; i < count; i++ )
      for( j = 0; j < threaded_width; j++ )
        data[i * threaded_width + j] = sequence( pos + i, j );
    if( threaded_fit ) {
      CHECK( pcm_fifo_write( &threaded_fifo, threaded_width, data, 1 ) == 1 );
      written = pcm_fifo_write( &threaded_fifo, threaded_width,
                                data + threaded_width, count - 1 );
      CHECK( written == (int)count - 1 );
      written++;
    } else {
      written = pcm_fifo_write( &threaded_fifo, threaded_width, data, count );
      CHECK( written >= 0 && written <= (int)count );
    }
    pos += written;
  }
  return NULL;
}

static void
stress( unsigned int width, int fit )
{
  pthread_t producer;
  unsigned char data[76];
  unsigned int pos = 0, i, j;
  int received;
  threaded_width = width;
  threaded_fit = fit;
  CHECK( !sfifo_init( &threaded_fifo, 63 ) );
  CHECK( !pthread_create( &producer, NULL, produce, NULL ) );
  while( pos < TOTAL_FRAMES ) {
    received = pcm_fifo_read( &threaded_fifo, width, data, pos % 19 + 1 );
    CHECK( received >= 0 && received <= 19 );
    for( i = 0; i < (unsigned int)received; i++ )
      for( j = 0; j < width; j++ )
        CHECK( data[i * width + j] == sequence( pos + i, j ) );
    pos += received;
  }
  CHECK( !pthread_join( producer, NULL ) );
  CHECK( !pcm_fifo_consumer_used( &threaded_fifo, width ) );
  sfifo_close( &threaded_fifo );
}
#endif

int
main( void )
{
  geometry( 2 );
  geometry( 4 );
  geometry( 3 );
  split_publication( 2 );
  split_publication( 4 );
  split_publication( 3 );
#ifdef HAVE_PTHREAD
  stress( 2, 0 );
  stress( 4, 0 );
  stress( 2, 1 );
  stress( 4, 1 );
  stress( 3, 1 );
#else
  puts( "SKIP: pthread PCM FIFO stress" );
#endif
  puts( "pcm_fifotest: passed" );
  return 0;
}
