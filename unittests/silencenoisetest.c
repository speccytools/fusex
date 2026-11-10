/* Regression tests for balanced exact-zero PCM filler. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sound/silence_noise.h"

static void
test_channels( int channels )
{
  libspectrum_signed_word input[20000], whole[20000], split[20000];
  sound_silence_noise a, b;
  long i;
  int sums[2] = { 0, 0 };
  int positive[2] = { 0, 0 }, negative[2] = { 0, 0 };
  for( i = 0; i < 20000; i++ ) {
    switch( i % 13 ) {
    case 0: input[i] = -32768; break;
    case 1: input[i] = 32767; break;
    case 2: input[i] = -1; break;
    case 3: input[i] = 1; break;
    default: input[i] = 0; break;
    }
  }
  memcpy( whole, input, sizeof( input ) );
  memcpy( split, input, sizeof( input ) );
  sound_silence_noise_reset( &a );
  sound_silence_noise_reset( &b );
  sound_silence_noise_apply( &a, whole, 20000, channels );
  /* One frame at a time must match a whole-buffer call. */
  for( i = 0; i < 20000; i += channels )
    sound_silence_noise_apply( &b, split + i, channels, channels );
  assert( !memcmp( whole, split, sizeof( whole ) ) );
  for( i = 0; i < 20000; i++ ) {
    int channel = i % channels;
    if( input[i] ) {
      assert( whole[i] == input[i] );
    } else {
      assert( whole[i] == 1 || whole[i] == -1 );
      sums[channel] += whole[i];
      assert( sums[channel] >= -1 && sums[channel] <= 1 );
      if( whole[i] > 0 ) positive[channel]++;
      else negative[channel]++;
    }
  }
  for( i = 0; i < channels; i++ )
    assert( positive[i] && negative[i] );
  /* An empty call must not advance either channel. */
  sound_silence_noise_apply( &a, whole, 0, channels );
  for( i = 0; i < channels; i++ ) {
    assert( a.channel[i].rng == b.channel[i].rng );
    assert( a.channel[i].pending == b.channel[i].pending );
  }
}

int
main( void )
{
  test_channels( 1 );
  test_channels( 2 );
  puts( "silence noise tests passed" );
  return 0;
}
