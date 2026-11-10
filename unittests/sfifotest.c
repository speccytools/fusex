/* sfifotest.c: byte FIFO boundary and optimized SPSC stress tests
 * GPL version 2 or later.
 */
#include "config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SFIFO_TEST_THREADS
#include <pthread.h>
#endif

#include "sound/sfifo.h"

#define CHECK(x) do { if( !(x) ) { \
  fprintf( stderr, "sfifotest:%d: %s\n", __LINE__, #x ); exit( 1 ); \
} } while( 0 )

static void
boundaries( void )
{
  sfifo_t f;
  unsigned char input[31], output[31];
  int capacity, round, i;
  CHECK( sfifo_init( &f, -1 ) == -EINVAL );
  sfifo_close( &f );
  CHECK( sfifo_init( &f, 0 ) == -EINVAL );
  CHECK( sfifo_init( &f, INT_MAX ) == -EINVAL );
  CHECK( sfifo_read( &f, output, 1 ) == -ENODEV );
  for( capacity = 1; capacity <= 31; capacity++ ) {
    CHECK( sfifo_init( &f, capacity ) == 0 );
    CHECK( atomic_is_lock_free( &f.readpos ) );
    CHECK( atomic_is_lock_free( &f.writepos ) );
    CHECK( sfifo_consumer_used( &f ) == 0 );
    CHECK( sfifo_producer_space( &f ) == f.size - 1 );
    CHECK( sfifo_write( &f, input, -1 ) == -EINVAL );
    CHECK( sfifo_read( &f, output, -1 ) == -EINVAL );
    CHECK( sfifo_write( &f, NULL, 1 ) == -EINVAL );
    CHECK( sfifo_read( &f, NULL, 1 ) == -EINVAL );
    CHECK( sfifo_write( &f, NULL, 0 ) == 0 );
    CHECK( sfifo_read( &f, NULL, 0 ) == 0 );
    for( round = 0; round < 1000; round++ ) {
      for( i = 0; i < 31; i++ ) input[i] = round + i;
      CHECK( sfifo_write( &f, input, 31 ) == f.size - 1 );
      CHECK( sfifo_producer_space( &f ) == 0 );
      CHECK( sfifo_write( &f, input, 1 ) == 0 );
      CHECK( sfifo_read( &f, output, 1 ) == 1 );
      CHECK( output[0] == input[0] );
      CHECK( sfifo_read( &f, output + 1, 30 ) == f.size - 2 );
      CHECK( memcmp( input, output, f.size - 1 ) == 0 );
      CHECK( sfifo_read( &f, output, 1 ) == 0 );
    }
    CHECK( sfifo_write( &f, input, 1 ) == 1 );
    sfifo_flush( &f );
    CHECK( sfifo_consumer_used( &f ) == 0 );
    sfifo_close( &f );
    sfifo_close( &f );
    CHECK( sfifo_producer_space( &f ) == -ENODEV );
  }
}

#ifdef SFIFO_TEST_THREADS
#define STRESS_BYTES 8000000u
static sfifo_t fifo;

static unsigned int
next_random( unsigned int *state )
{
  *state = *state * 1664525u + 1013904223u;
  return *state;
}

static unsigned char
sequence( unsigned int pos )
{
  return ( pos ^ ( pos >> 8 ) ^ ( pos >> 16 ) ) & 255;
}

static void *
producer( void *unused )
{
  unsigned int pos = 0, random = 1;
  unsigned char buf[257];
  int n, i, written;
  (void)unused;
  while( pos < STRESS_BYTES ) {
    n = next_random( &random ) % sizeof( buf ) + 1;
    if( (unsigned int)n > STRESS_BYTES - pos ) n = STRESS_BYTES - pos;
    for( i = 0; i < n; i++ ) buf[i] = sequence( pos + i );
    CHECK( sfifo_producer_space( &fifo ) >= 0 );
    written = sfifo_write( &fifo, buf, n );
    CHECK( written >= 0 && written <= n );
    pos += written;
  }
  return NULL;
}

static void
stress( int capacity )
{
  pthread_t thread;
  unsigned int pos = 0, random = 7;
  unsigned char buf[263];
  int n, i, received;
  CHECK( sfifo_init( &fifo, capacity ) == 0 );
  CHECK( pthread_create( &thread, NULL, producer, NULL ) == 0 );
  while( pos < STRESS_BYTES ) {
    n = next_random( &random ) % sizeof( buf ) + 1;
    CHECK( sfifo_consumer_used( &fifo ) >= 0 );
    received = sfifo_read( &fifo, buf, n );
    CHECK( received >= 0 && received <= n );
    for( i = 0; i < received; i++ )
      CHECK( buf[i] == sequence( pos + i ) );
    pos += received;
  }
  CHECK( pthread_join( thread, NULL ) == 0 );
  CHECK( sfifo_consumer_used( &fifo ) == 0 );
  sfifo_close( &fifo );
}
#endif

int
main( void )
{
  boundaries();
#ifdef SFIFO_TEST_THREADS
  stress( 7 );
  stress( 1023 );
#else
  puts( "SKIP: pthread SPSC stress (pthread support disabled)" );
#endif
  puts( "sfifotest: passed" );
  return 0;
}
