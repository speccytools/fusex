/* unittests.c: unit testing framework for Fuse
   Copyright (c) 2008-2018 Philip Kendall
   Copyright (c) 2015 Stuart Brady
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

   Author contact information:

   E-mail: philip-fuse@shadowmagic.org.uk

*/

#include "config.h"

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "libspectrum.h"

#include "debugger/debugger.h"
#include "display.h"
#include "fuse.h"
#include "keyboard.h"
#include "machine.h"
#include "memory_pages.h"
#include "mempool.h"
#include "periph.h"
#include "peripherals/scld.h"
#include "peripherals/disk/beta.h"
#include "peripherals/disk/disk.h"
#include "peripherals/disk/didaktik.h"
#include "peripherals/disk/disciple.h"
#include "peripherals/disk/opus.h"
#include "peripherals/disk/plusd.h"
#include "peripherals/dck.h"
#include "peripherals/ide/divide.h"
#include "peripherals/ide/divmmc.h"
#include "peripherals/ide/zxatasp.h"
#include "peripherals/ide/zxcf.h"
#include "peripherals/if1.h"
#include "peripherals/if2.h"
#include "peripherals/multiface.h"
#include "peripherals/sound/uspeech.h"
#include "peripherals/speccyboot.h"
#include "peripherals/ttx2000s.h"
#include "peripherals/ula.h"
#include "peripherals/usource.h"
#include "pokefinder/pokefinder.h"
#include "settings.h"
#include "sound.h"
#include "sound/speaker_filter.h"
#include "sound/ula_filter.h"
#include "snapshot.h"
#include "tape.h"
#include "bitmap.h"
#include "rectangle.h"
#include "compat.h"
#include "ui/scaler/scaler.h"
#include "unittests.h"
#include "utils.h"

static int
contention_test( void )
{
  libspectrum_dword i, checksum = 0, target;
  int error = 0;

  for( i = 0; i < ULA_CONTENTION_SIZE; i++ ) {
    /* Naive, but it will do for now */
    checksum += ula_contention[ i ] * ( i + 1 );
  }

  if( settings_current.late_timings ) {
    switch( machine_current->machine ) {
    case LIBSPECTRUM_MACHINE_16:
    case LIBSPECTRUM_MACHINE_48:
    case LIBSPECTRUM_MACHINE_SE:
      target = 2308927488UL;
      break;
    case LIBSPECTRUM_MACHINE_48_NTSC:
      target = 1962110976UL;
      break;
    case LIBSPECTRUM_MACHINE_128:
    case LIBSPECTRUM_MACHINE_PLUS2:
      target = 2335248384UL;
      break;
    case LIBSPECTRUM_MACHINE_PLUS2A:
    case LIBSPECTRUM_MACHINE_PLUS3:
    case LIBSPECTRUM_MACHINE_PLUS3E:
      target = 3113840640UL;
      break;
    case LIBSPECTRUM_MACHINE_TC2048:
    case LIBSPECTRUM_MACHINE_TC2068:
      target = 2307959808UL;
      break;
    case LIBSPECTRUM_MACHINE_TS2068:
      target = 1975593984UL;
      break;
    case LIBSPECTRUM_MACHINE_PENT:
    case LIBSPECTRUM_MACHINE_PENT512:
    case LIBSPECTRUM_MACHINE_PENT1024:
    case LIBSPECTRUM_MACHINE_SCORP:
      target = 0;
      break;
    default:
      target = -1;
      break;
    }
  } else {
    switch( machine_current->machine ) {
    case LIBSPECTRUM_MACHINE_16:
    case LIBSPECTRUM_MACHINE_48:
    case LIBSPECTRUM_MACHINE_SE:
      target = 2308862976UL;
      break;
    case LIBSPECTRUM_MACHINE_48_NTSC:
      target = 1962046464UL;
      break;
    case LIBSPECTRUM_MACHINE_128:
    case LIBSPECTRUM_MACHINE_PLUS2:
      target = 2335183872UL;
      break;
    case LIBSPECTRUM_MACHINE_PLUS2A:
    case LIBSPECTRUM_MACHINE_PLUS3:
    case LIBSPECTRUM_MACHINE_PLUS3E:
      target = 3113754624UL;
      break;
    case LIBSPECTRUM_MACHINE_TC2048:
    case LIBSPECTRUM_MACHINE_TC2068:
      target = 2307895296UL;
      break;
    case LIBSPECTRUM_MACHINE_TS2068:
      target = 1975529472UL;
      break;
    case LIBSPECTRUM_MACHINE_PENT:
    case LIBSPECTRUM_MACHINE_PENT512:
    case LIBSPECTRUM_MACHINE_PENT1024:
    case LIBSPECTRUM_MACHINE_SCORP:
      target = 0;
      break;
    default:
      target = -1;
      break;
    }
  }

  if( checksum != target ) {
    printf( "%s: contention test: checksum = %u, expected = %u\n", fuse_progname, checksum, target );
    error = 1;
  }

  return error;
}

static int
floating_bus_test( void )
{
  libspectrum_dword checksum = 0, target;
  libspectrum_word offset;
  int error = 0;

  for( offset = 0; offset < 8192; offset++ )
    RAM[ memory_current_screen ][ offset ] = offset % 0x100;

  for( tstates = 0; tstates < ULA_CONTENTION_SIZE; tstates++ )
    checksum += machine_current->unattached_port() * ( tstates + 1 );

  if( settings_current.late_timings ) {
    switch( machine_current->machine ) {
    case LIBSPECTRUM_MACHINE_16:
    case LIBSPECTRUM_MACHINE_48:
      target = 3426156480UL;
      break;
    case LIBSPECTRUM_MACHINE_48_NTSC:
      target = 3258908608UL;
      break;
    case LIBSPECTRUM_MACHINE_128:
    case LIBSPECTRUM_MACHINE_PLUS2:
      target = 2852995008UL;
      break;
    case LIBSPECTRUM_MACHINE_PLUS2A:
    case LIBSPECTRUM_MACHINE_PLUS3:
    case LIBSPECTRUM_MACHINE_PLUS3E:
    case LIBSPECTRUM_MACHINE_TC2048:
    case LIBSPECTRUM_MACHINE_TC2068:
    case LIBSPECTRUM_MACHINE_TS2068:
    case LIBSPECTRUM_MACHINE_SE:
    case LIBSPECTRUM_MACHINE_PENT:
    case LIBSPECTRUM_MACHINE_PENT512:
    case LIBSPECTRUM_MACHINE_PENT1024:
    case LIBSPECTRUM_MACHINE_SCORP:
      target = 4261381056UL;
      break;
    default:
      target = -1;
      break;
    }
  } else {
    switch( machine_current->machine ) {
    case LIBSPECTRUM_MACHINE_16:
    case LIBSPECTRUM_MACHINE_48:
      target = 3427723200UL;
      break;
    case LIBSPECTRUM_MACHINE_48_NTSC:
      target = 3260475328UL;
      break;
    case LIBSPECTRUM_MACHINE_128:
    case LIBSPECTRUM_MACHINE_PLUS2:
      target = 2854561728UL;
      break;
    case LIBSPECTRUM_MACHINE_PLUS2A:
    case LIBSPECTRUM_MACHINE_PLUS3:
    case LIBSPECTRUM_MACHINE_PLUS3E:
    case LIBSPECTRUM_MACHINE_TC2048:
    case LIBSPECTRUM_MACHINE_TC2068:
    case LIBSPECTRUM_MACHINE_TS2068:
    case LIBSPECTRUM_MACHINE_SE:
    case LIBSPECTRUM_MACHINE_PENT:
    case LIBSPECTRUM_MACHINE_PENT512:
    case LIBSPECTRUM_MACHINE_PENT1024:
    case LIBSPECTRUM_MACHINE_SCORP:
      target = 4261381056UL;
      break;
    default:
      target = -1;
      break;
    }
  }

  if( checksum != target ) {
    printf( "%s: floating bus test: checksum = %u, expected = %u\n", fuse_progname, checksum, target );
    error = 1;
  }

  return error;
}

#define TEST_ASSERT(x) do { if( !(x) ) { printf("Test assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #x ); return 1; } } while( 0 )

static double
speaker_filter_response( const speaker_filter_t *filter, double frequency,
                         int sample_rate )
{
  double omega = 2.0 * 3.14159265358979323846 * frequency / sample_rate;
  double cosine = cos( omega );
  double sine = sin( omega );
  double numerator_real = filter->b0 + filter->b1 * cosine +
                          filter->b2 * cos( 2.0 * omega );
  double numerator_imaginary = -filter->b1 * sine -
                               filter->b2 * sin( 2.0 * omega );
  double denominator_real = 1.0 + filter->a1 * cosine +
                            filter->a2 * cos( 2.0 * omega );
  double denominator_imaginary = -filter->a1 * sine -
                                 filter->a2 * sin( 2.0 * omega );

  return hypot( numerator_real, numerator_imaginary ) /
         hypot( denominator_real, denominator_imaginary );
}

static int
speaker_filter_test( void )
{
  static const int sample_rates[] = { 44100, 48000, 96000 };
  speaker_filter_t filter;
  double output[ 16 ];
  int i, rate;

  for( rate = 0; rate < ARRAY_SIZE( sample_rates ); rate++ ) {
    TEST_ASSERT( !speaker_filter_configure( &filter, sample_rates[ rate ],
                                            SPEAKER_FILTER_DEFAULT_FREQUENCY,
                                            SPEAKER_FILTER_DEFAULT_Q ) );
    TEST_ASSERT( isfinite( filter.b0 ) && isfinite( filter.b1 ) &&
                 isfinite( filter.b2 ) && isfinite( filter.a1 ) &&
                 isfinite( filter.a2 ) );
    TEST_ASSERT( fabs( filter.b0 ) < 2.0 && fabs( filter.b1 ) < 2.0 &&
                 fabs( filter.b2 ) < 2.0 && fabs( filter.a1 ) < 2.0 &&
                 fabs( filter.a2 ) < 2.0 );

    /* The digital response retains the intended acoustic high-pass shape at
     * each supported host rate without an artificial HF roll-off. */
    TEST_ASSERT( speaker_filter_response( &filter, 100.0,
                                           sample_rates[ rate ] ) < 0.03 );
    TEST_ASSERT( speaker_filter_response( &filter, 200.0,
                                           sample_rates[ rate ] ) < 0.1 );
    TEST_ASSERT( speaker_filter_response( &filter, 750.0,
                                           sample_rates[ rate ] ) > 0.65 );
    TEST_ASSERT( speaker_filter_response( &filter, 750.0,
                                           sample_rates[ rate ] ) < 0.75 );
    TEST_ASSERT( speaker_filter_response( &filter, 1000.0,
                                           sample_rates[ rate ] ) > 0.8 );
    TEST_ASSERT( speaker_filter_response( &filter, 2000.0,
                                           sample_rates[ rate ] ) > 0.95 );
    TEST_ASSERT( speaker_filter_response( &filter, 3200.0,
                                           sample_rates[ rate ] ) > 0.98 );
    TEST_ASSERT( speaker_filter_response( &filter, 6400.0,
                                           sample_rates[ rate ] ) > 0.98 );
  }

  TEST_ASSERT( speaker_filter_configure( &filter, 48000, 0.0,
                                         SPEAKER_FILTER_DEFAULT_Q ) );
  TEST_ASSERT( speaker_filter_configure( &filter, 48000,
                                         SPEAKER_FILTER_DEFAULT_FREQUENCY,
                                         0.0 ) );

  TEST_ASSERT( !speaker_filter_configure( &filter, 48000,
                                          SPEAKER_FILTER_DEFAULT_FREQUENCY,
                                          SPEAKER_FILTER_DEFAULT_Q ) );
  for( i = 0; i < ARRAY_SIZE( output ); i++ )
    output[ i ] = speaker_filter_apply( &filter, i == 0 ? 1.0 : 0.0 );
  speaker_filter_reset( &filter );
  for( i = 0; i < ARRAY_SIZE( output ); i++ )
    TEST_ASSERT( output[ i ] == speaker_filter_apply( &filter,
                                                       i == 0 ? 1.0 : 0.0 ) );

  return 0;
}

static int
ula_filter_test( void )
{
  static const int sample_rates[] = { 44100, 48000, 96000 };
  static const double alpha_rise[] = { 0.4624040261175517,
                                       0.43459915012074307,
                                       0.24806858698465262 };
  static const double alpha_fall[] = { 0.2805717781540314,
                                       0.2610632971504549,
                                       0.14038572438008856 };
  static const double input[] = { 0.0, 12800.0, 12800.0, 0.0, 0.0 };
  static const double expected[] = { 0.0, 3175.2779134035536,
                                     5562.869121545511, 4781.921710285717,
                                     4110.608167058385 };
  ula_filter_t continuous, split, direction;
  double continuous_output[ ARRAY_SIZE( input ) ];
  double direction_state, expected_direction;
  int i, rate;

  TEST_ASSERT( ULA_FILTER_RISE_TAU == 36.53558495933805e-6 );
  TEST_ASSERT( ULA_FILTER_FALL_TAU == 68.86073172982108e-6 );

  for( rate = 0; rate < ARRAY_SIZE( sample_rates ); rate++ ) {
    TEST_ASSERT( !ula_filter_configure( &continuous, sample_rates[ rate ] ) );
    TEST_ASSERT( fabs( continuous.alpha_rise - alpha_rise[ rate ] ) < 1e-15 );
    TEST_ASSERT( fabs( continuous.alpha_fall - alpha_fall[ rate ] ) < 1e-15 );
  }
  TEST_ASSERT( ula_filter_configure( &continuous, 0 ) );

  TEST_ASSERT( !ula_filter_configure( &continuous, 96000 ) );
  for( i = 0; i < ARRAY_SIZE( input ); i++ ) {
    continuous_output[ i ] = ula_filter_apply( &continuous, input[ i ] );
    TEST_ASSERT( fabs( continuous_output[ i ] - expected[ i ] ) < 1e-9 );
  }

  /* The first input initializes the stream exactly, as in the listening-test
   * generator; reset must restore that same convention. */
  ula_filter_reset( &continuous );
  TEST_ASSERT( ula_filter_apply( &continuous, 1234.0 ) == 1234.0 );

  TEST_ASSERT( !ula_filter_configure( &direction, 96000 ) );
  ula_filter_apply( &direction, 0.0 );
  TEST_ASSERT( fabs( ula_filter_apply( &direction, 100.0 ) -
                     100.0 * direction.alpha_rise ) < 1e-14 );
  ula_filter_reset( &direction );
  ula_filter_apply( &direction, 100.0 );
  TEST_ASSERT( fabs( ula_filter_apply( &direction, 0.0 ) -
                     100.0 * ( 1.0 - direction.alpha_fall ) ) < 1e-14 );

  /* This target fell from the prior input (100 to 90), but remains above the
   * current state, so it must use the rise coefficient. */
  ula_filter_reset( &direction );
  ula_filter_apply( &direction, 0.0 );
  direction_state = ula_filter_apply( &direction, 100.0 );
  expected_direction = direction_state + direction.alpha_rise *
                       ( 90.0 - direction_state );
  TEST_ASSERT( fabs( ula_filter_apply( &direction, 90.0 ) -
                     expected_direction ) < 1e-14 );

  /* Persisting one state across arbitrary buffer boundaries must be identical
   * to a single uninterrupted stream. */
  TEST_ASSERT( !ula_filter_configure( &split, 96000 ) );
  for( i = 0; i < 2; i++ )
    TEST_ASSERT( ula_filter_apply( &split, input[ i ] ) == continuous_output[ i ] );
  for( ; i < ARRAY_SIZE( input ); i++ )
    TEST_ASSERT( ula_filter_apply( &split, input[ i ] ) == continuous_output[ i ] );

  return 0;
}

static int
ula_sound_levels_test( void )
{
  int mic_ampl, beeper_ampl;

  sound_ula_levels( 0, 0, &mic_ampl, &beeper_ampl );
  TEST_ASSERT( mic_ampl == 0 );
  TEST_ASSERT( beeper_ampl == 0 );

  sound_ula_levels( 1, 0, &mic_ampl, &beeper_ampl );
  TEST_ASSERT( mic_ampl == SOUND_AMPL_TAPE );
  TEST_ASSERT( beeper_ampl == 0 );

  sound_ula_levels( 0, 1, &mic_ampl, &beeper_ampl );
  TEST_ASSERT( mic_ampl == SOUND_AMPL_BEEPER );
  TEST_ASSERT( beeper_ampl == SOUND_AMPL_BEEPER );

  sound_ula_levels( 1, 1, &mic_ampl, &beeper_ampl );
  TEST_ASSERT( mic_ampl == SOUND_AMPL_BEEPER + SOUND_AMPL_TAPE );
  TEST_ASSERT( beeper_ampl == SOUND_AMPL_BEEPER + SOUND_AMPL_TAPE );
  TEST_ASSERT( SOUND_AMPL_BEEPER / SOUND_AMPL_TAPE == 25 );

  /* D3-only transitions alter the MIC path but not speaker drive. */
  sound_ula_levels( 0, 0, &mic_ampl, &beeper_ampl );
  TEST_ASSERT( mic_ampl == 0 && beeper_ampl == 0 );
  sound_ula_levels( 1, 0, &mic_ampl, &beeper_ampl );
  TEST_ASSERT( mic_ampl == SOUND_AMPL_TAPE && beeper_ampl == 0 );

  /* D4-only and every mixed transition retain their distinct levels. */
  sound_ula_levels( 0, 1, &mic_ampl, &beeper_ampl ); /* 00 -> 10 */
  TEST_ASSERT( mic_ampl == SOUND_AMPL_BEEPER );
  TEST_ASSERT( beeper_ampl == SOUND_AMPL_BEEPER );
  sound_ula_levels( 1, 1, &mic_ampl, &beeper_ampl ); /* 10 -> 11 */
  TEST_ASSERT( mic_ampl - SOUND_AMPL_BEEPER == SOUND_AMPL_TAPE );
  TEST_ASSERT( beeper_ampl - SOUND_AMPL_BEEPER == SOUND_AMPL_TAPE );
  sound_ula_levels( 1, 0, &mic_ampl, &beeper_ampl ); /* 11 -> 01 */
  TEST_ASSERT( mic_ampl == SOUND_AMPL_TAPE && beeper_ampl == 0 );
  sound_ula_levels( 0, 1, &mic_ampl, &beeper_ampl ); /* 01 -> 10 */
  TEST_ASSERT( mic_ampl == SOUND_AMPL_BEEPER );
  TEST_ASSERT( beeper_ampl == SOUND_AMPL_BEEPER );
  sound_ula_levels( 1, 1, &mic_ampl, &beeper_ampl ); /* 10 -> 11 */
  TEST_ASSERT( mic_ampl == SOUND_AMPL_BEEPER + SOUND_AMPL_TAPE );
  TEST_ASSERT( beeper_ampl == SOUND_AMPL_BEEPER + SOUND_AMPL_TAPE );

  return 0;
}

static int
floating_bus_merge_test( void )
{
  /* If peripherals asserted all lines, should see no change */
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0xff, 0x00 ) == 0xaa ); 
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0xff, 0xff ) == 0xaa ); 

  /* If peripherals asserted nothing, should pull all lines down */
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0x00, 0x00 ) == 0x00 ); 
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0x00, 0xff ) == 0xaa ); 

  /* Tests when peripherals asserted some lines */
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0xf0, 0x00 ) == 0xa0 );
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0xf0, 0xff ) == 0xaa );
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0x0f, 0x00 ) == 0x0a );
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0x0f, 0xff ) == 0xaa );

  /* Tests with complementary attached/floating_bus masks */
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0xf0, 0x0f ) == 0xaa );
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0x0f, 0xf0 ) == 0xaa );
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0x55, 0x00 ) == 0x00 );
  TEST_ASSERT( periph_merge_floating_bus( 0xaa, 0x00, 0x55 ) == 0x00 );

  return 0;
}

static int
bitmap_ops_test( void )
{
  libspectrum_byte buf[2];

  /* Group 1: bitmap_test returns zero on a zeroed buffer */
  buf[0] = 0; buf[1] = 0;
  TEST_ASSERT( bitmap_test( buf, 0 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 7 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 8 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 15 ) == 0 );

  /* Group 2: bitmap_set sets only the target bit */
  buf[0] = 0; buf[1] = 0;
  bitmap_set( buf, 0 );
  TEST_ASSERT( bitmap_test( buf, 0 ) != 0 );
  TEST_ASSERT( bitmap_test( buf, 1 ) == 0 );

  buf[0] = 0; buf[1] = 0;
  bitmap_set( buf, 7 );
  TEST_ASSERT( bitmap_test( buf, 7 ) != 0 );
  TEST_ASSERT( bitmap_test( buf, 6 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 8 ) == 0 );

  /* Group 3: byte-boundary crossing (bits 7->8) */
  buf[0] = 0; buf[1] = 0;
  bitmap_set( buf, 8 );
  TEST_ASSERT( buf[0] == 0 );
  TEST_ASSERT( bitmap_test( buf, 8 ) != 0 );
  TEST_ASSERT( bitmap_test( buf, 7 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 9 ) == 0 );

  /* Group 4: bitmap_reset clears only the target bit in an all-ones buffer */
  buf[0] = 0xff; buf[1] = 0xff;
  bitmap_reset( buf, 0 );
  TEST_ASSERT( bitmap_test( buf, 0 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 1 ) != 0 );
  TEST_ASSERT( bitmap_test( buf, 8 ) != 0 );

  buf[0] = 0xff; buf[1] = 0xff;
  bitmap_reset( buf, 15 );
  TEST_ASSERT( bitmap_test( buf, 15 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 14 ) != 0 );

  /* Group 5: set-then-reset round-trips cleanly to zero */
  buf[0] = 0; buf[1] = 0;
  bitmap_set( buf, 3 );
  TEST_ASSERT( bitmap_test( buf, 3 ) != 0 );
  bitmap_reset( buf, 3 );
  TEST_ASSERT( bitmap_test( buf, 3 ) == 0 );

  /* Group 6: two independent bits in the same byte do not interfere */
  buf[0] = 0; buf[1] = 0;
  bitmap_set( buf, 0 );
  bitmap_set( buf, 4 );
  TEST_ASSERT( bitmap_test( buf, 0 ) != 0 );
  TEST_ASSERT( bitmap_test( buf, 4 ) != 0 );
  TEST_ASSERT( bitmap_test( buf, 1 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 3 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 5 ) == 0 );
  bitmap_reset( buf, 0 );
  TEST_ASSERT( bitmap_test( buf, 0 ) == 0 );
  TEST_ASSERT( bitmap_test( buf, 4 ) != 0 );

  return 0;
}

static int
snapshot_copy_from_releases_keyboard_test( void )
{
  libspectrum_snap *snap;
  int i;

  snap = libspectrum_snap_alloc();
  TEST_ASSERT( snap != NULL );
  TEST_ASSERT( snapshot_copy_to( snap ) == 0 );

  keyboard_press( KEYBOARD_a );

  for( i = 0; i < 8; i++ ) {
    if( keyboard_return_values[i] != 0xff ) break;
  }
  TEST_ASSERT( i != 8 );

  TEST_ASSERT( snapshot_copy_from( snap ) == 0 );

  for( i = 0; i < 8; i++ ) {
    TEST_ASSERT( keyboard_return_values[i] == 0xff );
  }

  TEST_ASSERT( libspectrum_snap_free( snap ) == 0 );

  return 0;
}

static int
snapshot_custom_rom_is_replaced_by_soft_reset_test( void )
{
  static const libspectrum_machine machines[] = {
    LIBSPECTRUM_MACHINE_16, LIBSPECTRUM_MACHINE_48,
    LIBSPECTRUM_MACHINE_48_NTSC, LIBSPECTRUM_MACHINE_128,
    LIBSPECTRUM_MACHINE_PLUS2, LIBSPECTRUM_MACHINE_PLUS2A,
    LIBSPECTRUM_MACHINE_PLUS3, LIBSPECTRUM_MACHINE_PLUS3E,
    LIBSPECTRUM_MACHINE_TC2048, LIBSPECTRUM_MACHINE_TC2068,
    LIBSPECTRUM_MACHINE_TS2068, LIBSPECTRUM_MACHINE_SE,
  };
  libspectrum_machine old_machine = machine_current->machine;
  size_t i;
  int r = 0;

  for( i = 0; i < ARRAY_SIZE( machines ); i++ ) {
    libspectrum_snap *snap;
    libspectrum_byte *rom;

    if( machine_select( machines[ i ] ) ) { r++; continue; }

    snap = libspectrum_snap_alloc();
    if( !snap || snapshot_copy_to( snap ) ) { r++; continue; }

    rom = libspectrum_new( libspectrum_byte, 0x4000 );
    memset( rom, 0xa5, 0x4000 );
    libspectrum_snap_set_custom_rom( snap, 1 );
    libspectrum_snap_set_custom_rom_pages( snap, 1 );
    libspectrum_snap_set_roms( snap, 0, rom );
    libspectrum_snap_set_rom_length( snap, 0, 0x4000 );

    if( snapshot_copy_from( snap ) || memory_map_rom[ 0 ].page[ 0 ] != 0xa5 ||
        machine_reset( 0 ) || memory_map_rom[ 0 ].page[ 0 ] != 0xa5 ||
        machine_reset( 1 ) || memory_map_rom[ 0 ].page[ 0 ] == 0xa5 ) r++;

    if( libspectrum_snap_free( snap ) ) r++;
  }

  if( machine_select( old_machine ) ) r++;

  return r;
}

static int
slt_is_cleared_by_reset_test( void )
{
  libspectrum_snap *snap;
  libspectrum_snap *result;
  libspectrum_byte *data;
  int r = 0;

  snap = libspectrum_snap_alloc();
  result = libspectrum_snap_alloc();
  if( !snap || !result ) {
    if( snap ) libspectrum_snap_free( snap );
    if( result ) libspectrum_snap_free( result );
    return 1;
  }

  if( snapshot_copy_to( snap ) ) r++;
  data = libspectrum_new( libspectrum_byte, 1 );
  data[ 0 ] = 0xa5;
  libspectrum_snap_set_slt_length( snap, 0, 1 );
  libspectrum_snap_set_slt( snap, 0, data );

  if( snapshot_copy_from( snap ) || machine_reset( 0 ) ||
      snapshot_copy_to( result ) || libspectrum_snap_slt_length( result, 0 ) )
    r++;

  if( libspectrum_snap_free( snap ) ) r++;
  if( libspectrum_snap_free( result ) ) r++;

  return r;
}

static int
slt_screen_is_cleared_by_reset_test( void )
{
  libspectrum_snap *snap;
  libspectrum_snap *result;
  libspectrum_byte *screen;
  int r = 0;

  snap = libspectrum_snap_alloc();
  result = libspectrum_snap_alloc();
  if( !snap || !result ) {
    if( snap ) libspectrum_snap_free( snap );
    if( result ) libspectrum_snap_free( result );
    return 1;
  }

  if( snapshot_copy_to( snap ) ) r++;
  screen = libspectrum_new( libspectrum_byte, DISPLAY_FILE_SIZE );
  memset( screen, 0xa5, DISPLAY_FILE_SIZE );
  libspectrum_snap_set_slt_screen( snap, screen );
  libspectrum_snap_set_slt_screen_level( snap, 1 );

  if( snapshot_copy_from( snap ) || machine_reset( 0 ) ||
      snapshot_copy_to( result ) ||
      libspectrum_snap_slt_screen( result ) ||
      libspectrum_snap_slt_screen_level( result ) )
    r++;

  if( libspectrum_snap_free( snap ) ) r++;
  if( libspectrum_snap_free( result ) ) r++;

  return r;
}

static int
spec_se_dock_ram_reset_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  int r = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_SE ) ) return 1;

  timex_dock[ 0 ].page[ 0 ] = 0xa5;
  if( machine_reset( 0 ) || timex_dock[ 0 ].page[ 0 ] != 0xa5 ||
      machine_reset( 1 ) || timex_dock[ 0 ].page[ 0 ] != 0 ) r++;

  if( machine_select( old_machine ) ) r++;

  return r;
}

static int
keyboard_read_test( void )
{
  /* No keys pressed: all half-rows are 0xff, keyboard_read returns 0xff
     regardless of which half-rows are selected. */
  keyboard_release_all();
  /* Select all half-rows (porth = 0x00 means every bit is low → select all) */
  TEST_ASSERT( keyboard_read( 0x00 ) == 0xff );
  /* Select no half-rows (porth = 0xff means every bit is high → select none) */
  TEST_ASSERT( keyboard_read( 0xff ) == 0xff );

  /* Press 'a': sits in half-row 1, bit 0x01.
     keyboard_read shifts porth right once per iteration and checks bit 0 each
     time, so half-row N is selected when bit N of porth is 0.
     0xfd = 11111101b has bit 1 low → selects only half-row 1. */
  keyboard_press( KEYBOARD_a );
  TEST_ASSERT( keyboard_read( 0xfd ) == 0xfe ); /* bit 0 cleared */
  /* Selecting a different half-row should not show the pressed key. */
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xff ); /* half-row 0, 'a' not there */
  /* Selecting all half-rows still shows the pressed key. */
  TEST_ASSERT( keyboard_read( 0x00 ) == 0xfe );
  keyboard_release( KEYBOARD_a );

  /* After release the bit is restored. */
  TEST_ASSERT( keyboard_read( 0xfd ) == 0xff );

  return 0;
}

static int
keyboard_simulate_keypress_test( void )
{
  /* 'a' is in half-row 1, bit 0x01.  keyboard_simulate_keypress checks
     whether half-row 1's bit (mask = 1<<1 = 0x02) is low in porth. */

  /* porth = 0xfd (bit 1 low) → half-row 1 selected → bit 0x01 cleared */
  TEST_ASSERT( keyboard_simulate_keypress( 0xfd, KEYBOARD_a ) == 0xfe );

  /* porth = 0xff (bit 1 high) → half-row 1 not selected → 0xff returned */
  TEST_ASSERT( keyboard_simulate_keypress( 0xff, KEYBOARD_a ) == 0xff );

  /* porth = 0x00 (all bits low) → all half-rows selected → bit cleared */
  TEST_ASSERT( keyboard_simulate_keypress( 0x00, KEYBOARD_a ) == 0xfe );

  /* An unknown/unmapped key should return 0xff unchanged. */
  TEST_ASSERT( keyboard_simulate_keypress( 0x00, KEYBOARD_NONE ) == 0xff );

  return 0;
}

static int
utils_safe_strdup_test( void )
{
  char *result;

  /* NULL input should return NULL (safe from crash unlike plain strdup) */
  TEST_ASSERT( utils_safe_strdup( NULL ) == NULL );

  /* Regular string should be copied correctly */
  result = utils_safe_strdup( "hello fuse" );
  TEST_ASSERT( result != NULL );
  TEST_ASSERT( strcmp( result, "hello fuse" ) == 0 );
  libspectrum_free( result );

  /* Empty string should produce an allocated, empty string */
  result = utils_safe_strdup( "" );
  TEST_ASSERT( result != NULL );
  TEST_ASSERT( strcmp( result, "" ) == 0 );
  libspectrum_free( result );

  return 0;
}

static int
mempool_test( void )
{
  int pool1, pool2;
  int initial_pools = mempool_get_pools();

  pool1 = mempool_register_pool();

  TEST_ASSERT( mempool_get_pools() == initial_pools + 1 );
  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );

  mempool_malloc( pool1, 23 );

  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 1 );

  mempool_malloc_n( pool1, 42, 4 );

  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 2 );

  mempool_new( pool1, libspectrum_word, 5 );

  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 3 );

  mempool_free( pool1 );

  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );

  pool2 = mempool_register_pool();

  TEST_ASSERT( mempool_get_pools() == initial_pools + 2 );
  TEST_ASSERT( mempool_get_pool_size( pool2 ) == 0 );

  mempool_malloc( pool1, 23 );

  TEST_ASSERT( mempool_get_pool_size( pool2 ) == 0 );

  mempool_malloc_n( pool1, 6, 7 );

  TEST_ASSERT( mempool_get_pool_size( pool2 ) == 0 );

  mempool_new( pool1, libspectrum_byte, 5 );

  TEST_ASSERT( mempool_get_pool_size( pool2 ) == 0 );

  mempool_malloc( pool2, 42 );
  
  TEST_ASSERT( mempool_get_pool_size( pool2 ) == 1 );

  mempool_free( pool2 );

  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 3 );
  TEST_ASSERT( mempool_get_pool_size( pool2 ) == 0 );
  
  mempool_free( pool1 );

  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );
  TEST_ASSERT( mempool_get_pool_size( pool2 ) == 0 );

  /* Test mempool_strdup: verify string content and pool tracking */
  {
    const char *test_string = "hello fuse";
    char *result = mempool_strdup( pool1, test_string );

    TEST_ASSERT( result != NULL );
    TEST_ASSERT( strcmp( result, test_string ) == 0 );
    TEST_ASSERT( mempool_get_pool_size( pool1 ) == 1 );

    mempool_free( pool1 );
    TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );
  }

  /* Test mempool_strdup with an empty string */
  {
    char *result = mempool_strdup( pool1, "" );

    TEST_ASSERT( result != NULL );
    TEST_ASSERT( strcmp( result, "" ) == 0 );
    TEST_ASSERT( mempool_get_pool_size( pool1 ) == 1 );

    mempool_free( pool1 );
    TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );
  }

  /* Test mempool_strdup with NULL returns NULL safely */
  TEST_ASSERT( mempool_strdup( pool1, NULL ) == NULL );
  TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );

  /* Test that out-of-range pool IDs return NULL */
  TEST_ASSERT( mempool_malloc( mempool_get_pools(), 23 ) == NULL );
  TEST_ASSERT( mempool_malloc( -2, 23 ) == NULL );

  /* Test MEMPOOL_UNTRACKED: allocations succeed but bypass pool tracking */
  {
    void *p = mempool_malloc( MEMPOOL_UNTRACKED, 16 );
    TEST_ASSERT( p != NULL );
    TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );
    libspectrum_free( p );
  }

  {
    void *p = mempool_malloc_n( MEMPOOL_UNTRACKED, 4, 8 );
    TEST_ASSERT( p != NULL );
    TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );
    libspectrum_free( p );
  }

  {
    char *s = mempool_strdup( MEMPOOL_UNTRACKED, "untracked" );
    TEST_ASSERT( s != NULL );
    TEST_ASSERT( strcmp( s, "untracked" ) == 0 );
    TEST_ASSERT( mempool_get_pool_size( pool1 ) == 0 );
    libspectrum_free( s );
  }

  return 0;
}

static int
assert_page( libspectrum_word base, libspectrum_word length, int source, int page )
{
  int base_index = base / MEMORY_PAGE_SIZE;
  int i;

  for( i = 0; i < length / MEMORY_PAGE_SIZE; i++ ) {
    TEST_ASSERT( memory_map_read[ base_index + i ].source == source );
    TEST_ASSERT( memory_map_read[ base_index + i ].page_num == page );
    TEST_ASSERT( memory_map_write[ base_index + i ].source == source );
    TEST_ASSERT( memory_map_write[ base_index + i ].page_num == page );
  }

  return 0;
}

int
unittests_assert_2k_page( libspectrum_word base, int source, int page )
{
  return assert_page( base, 0x0800, source, page );
}

int
unittests_assert_4k_page( libspectrum_word base, int source, int page )
{
  return assert_page( base, 0x1000, source, page );
}

int
unittests_assert_8k_page( libspectrum_word base, int source, int page )
{
  return assert_page( base, 0x2000, source, page );
}

int
unittests_assert_16k_page( libspectrum_word base, int source, int page )
{
  return assert_page( base, 0x4000, source, page );
}

static int
assert_16k_rom_page( libspectrum_word base, int page )
{
  return unittests_assert_16k_page( base, memory_source_rom, page );
}

int
unittests_assert_16k_ram_page( libspectrum_word base, int page )
{
  return unittests_assert_16k_page( base, memory_source_ram, page );
}

static int
assert_16k_pages( int rom, int ram4000, int ram8000, int ramc000 )
{
  int r = 0;

  r += assert_16k_rom_page( 0x0000, rom );
  r += unittests_assert_16k_ram_page( 0x4000, ram4000 );
  r += unittests_assert_16k_ram_page( 0x8000, ram8000 );
  r += unittests_assert_16k_ram_page( 0xc000, ramc000 );

  return r;
}

static int
assert_all_ram( int ram0000, int ram4000, int ram8000, int ramc000 )
{
  int r = 0;

  r += unittests_assert_16k_ram_page( 0x0000, ram0000 );
  r += unittests_assert_16k_ram_page( 0x4000, ram4000 );
  r += unittests_assert_16k_ram_page( 0x8000, ram8000 );
  r += unittests_assert_16k_ram_page( 0xc000, ramc000 );

  return r;
}

static int
paging_test_16( void )
{
  int r = 0;

  r += assert_16k_rom_page( 0x0000, 0 );
  r += unittests_assert_16k_ram_page( 0x4000, 5 );
  r += unittests_assert_16k_page( 0x8000, memory_source_none, 0 );
  r += unittests_assert_16k_page( 0xc000, memory_source_none, 0 );

  return r;
}

int
unittests_paging_test_48( int ram8000 )
{
  int r = 0;

  r += assert_16k_pages( 0, 5, ram8000, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  return r;
}

static int
paging_test_128_unlocked( int ram8000 )
{
  int r = 0;

  TEST_ASSERT( machine_current->ram.locked == 0 );

  r += unittests_paging_test_48( ram8000 );

  writeport_internal( 0x7ffd, 0x07 );
  r += assert_16k_pages( 0, 5, ram8000, 7 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x08 );
  r += assert_16k_pages( 0, 5, ram8000, 0 );
  TEST_ASSERT( memory_current_screen == 7 );

  writeport_internal( 0x7ffd, 0x10 );
  r += assert_16k_pages( 1, 5, ram8000, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x1f );
  r += assert_16k_pages( 1, 5, ram8000, 7 );
  TEST_ASSERT( memory_current_screen == 7 );

  return r;
}

static int
paging_test_128_locked( int ram8000 )
{
  int r = 0;

  writeport_internal( 0x7ffd, 0x20 );
  r += assert_16k_pages( 0, 5, ram8000, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  TEST_ASSERT( machine_current->ram.locked != 0 );

  writeport_internal( 0x7ffd, 0x1f );
  r += assert_16k_pages( 0, 5, ram8000, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  return r;
}

static int
paging_test_128( void )
{
  int r = 0;

  r += paging_test_128_unlocked( 2 );
  r += paging_test_128_locked( 2 );

  return r;
}

static int
paging_test_plus3( void )
{
  int r = 0;
  
  r += paging_test_128_unlocked( 2 );

  writeport_internal( 0x7ffd, 0x00 );
  writeport_internal( 0x1ffd, 0x04 );
  r += assert_16k_pages( 2, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x10 );
  r += assert_16k_pages( 3, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x01 );
  r += assert_all_ram( 0, 1, 2, 3 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x03 );
  r += assert_all_ram( 4, 5, 6, 7 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x05 );
  r += assert_all_ram( 4, 5, 6, 3 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x07 );
  r += assert_all_ram( 4, 7, 6, 3 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x00 );
  r += paging_test_128_locked( 2 );

  writeport_internal( 0x1ffd, 0x10 );
  r += assert_16k_pages( 0, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  return r;
}

static int
paging_test_scorpion( void )
{
  int r = 0;

  r += paging_test_128_unlocked( 2 );

  writeport_internal( 0x7ffd, 0x00 );
  writeport_internal( 0x1ffd, 0x01 );
  r += assert_all_ram( 0, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x02 );
  r += assert_16k_pages( 2, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x10 );
  r += assert_16k_pages( 0, 5, 2, 8 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x07 );
  r += assert_16k_pages( 0, 5, 2, 15 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x1ffd, 0x00 );
  r += paging_test_128_locked( 2 );

  return r;
}

static int
paging_test_pentagon512_unlocked( void )
{
  int r = 0;

  beta_unpage();

  r += paging_test_128_unlocked( 2 );

  writeport_internal( 0x7ffd, 0x40 );
  r += assert_16k_pages( 0, 5, 2, 8 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x47 );
  r += assert_16k_pages( 0, 5, 2, 15 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x80 );
  r += assert_16k_pages( 0, 5, 2, 16 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0xc7 );
  r += assert_16k_pages( 0, 5, 2, 31 );
  TEST_ASSERT( memory_current_screen == 5 );

  return r;
}

static int
paging_test_pentagon512( void )
{
  int r = 0;

  r += paging_test_pentagon512_unlocked();
  r += paging_test_128_locked( 2 );

  return r;
}

static int
paging_test_pentagon1024( void )
{
  int r = 0;

  r += paging_test_pentagon512_unlocked();

  writeport_internal( 0x7ffd, 0x20 );
  r += assert_16k_pages( 0, 5, 2, 32 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x27 );
  r += assert_16k_pages( 0, 5, 2, 39 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x60 );
  r += assert_16k_pages( 0, 5, 2, 40 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0xa0 );
  r += assert_16k_pages( 0, 5, 2, 48 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0xe7 );
  r += assert_16k_pages( 0, 5, 2, 63 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x00 );
  writeport_internal( 0xeff7, 0x08 );
  r += assert_all_ram( 0, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x00 );
  writeport_internal( 0xeff7, 0x04 );
  r += assert_16k_pages( 0, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x40 );
  r += assert_16k_pages( 0, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  writeport_internal( 0x7ffd, 0x80 );
  r += assert_16k_pages( 0, 5, 2, 0 );
  TEST_ASSERT( memory_current_screen == 5 );

  r += paging_test_128_locked( 2 );

  return r;
}

static int
paging_test_timex( int ram8000, int dock_source, int exrom_source )
{
  int r = 0;

  r += unittests_paging_test_48( ram8000 );

  writeport_internal( 0x00f4, 0x01 );
  r += unittests_assert_8k_page( 0x0000, dock_source, 0 );
  r += unittests_assert_8k_page( 0x2000, memory_source_rom, 0 );
  r += unittests_assert_16k_ram_page( 0x4000, 5 );
  r += unittests_assert_16k_ram_page( 0x8000, ram8000 );
  r += unittests_assert_16k_ram_page( 0xc000, 0 );

  writeport_internal( 0x00f4, 0x04 );
  r += assert_16k_rom_page( 0x0000, 0 );
  r += unittests_assert_8k_page( 0x4000, dock_source, 2 );
  r += unittests_assert_8k_page( 0x6000, memory_source_ram, 5 );
  r += unittests_assert_16k_ram_page( 0x8000, ram8000 );
  r += unittests_assert_16k_ram_page( 0xc000, 0 );

  writeport_internal( 0x00f4, 0xff );
  r += unittests_assert_8k_page( 0x0000, dock_source, 0 );
  r += unittests_assert_8k_page( 0x2000, dock_source, 1 );
  r += unittests_assert_8k_page( 0x4000, dock_source, 2 );
  r += unittests_assert_8k_page( 0x6000, dock_source, 3 );
  r += unittests_assert_8k_page( 0x8000, dock_source, 4 );
  r += unittests_assert_8k_page( 0xa000, dock_source, 5 );
  r += unittests_assert_8k_page( 0xc000, dock_source, 6 );
  r += unittests_assert_8k_page( 0xe000, dock_source, 7 );

  writeport_internal( 0x00ff, 0x80 );
  r += unittests_assert_8k_page( 0x0000, exrom_source, 0 );
  r += unittests_assert_8k_page( 0x2000, exrom_source, 1 );
  r += unittests_assert_8k_page( 0x4000, exrom_source, 2 );
  r += unittests_assert_8k_page( 0x6000, exrom_source, 3 );
  r += unittests_assert_8k_page( 0x8000, exrom_source, 4 );
  r += unittests_assert_8k_page( 0xa000, exrom_source, 5 );
  r += unittests_assert_8k_page( 0xc000, exrom_source, 6 );
  r += unittests_assert_8k_page( 0xe000, exrom_source, 7 );
  
  writeport_internal( 0x00f4, 0x00 );
  r += assert_16k_rom_page( 0x0000, 0 );
  r += unittests_assert_16k_ram_page( 0x4000, 5 );
  r += unittests_assert_16k_ram_page( 0x8000, ram8000 );
  r += unittests_assert_16k_ram_page( 0xc000, 0 );

  return r;
}

static int
paging_test_se( void )
{
  int r = 0;

  r += paging_test_128_unlocked( 8 );

  writeport_internal( 0x7ffd, 0x00 );
  r += paging_test_timex( 8, memory_source_dock, memory_source_exrom );

  writeport_internal( 0x7ffd, 0x01 );
  writeport_internal( 0x00f4, 0x0c );
  r += assert_16k_rom_page( 0x0000, 0 );
  r += unittests_assert_8k_page( 0x4000, memory_source_exrom, 2 );
  r += unittests_assert_8k_page( 0x6000, memory_source_exrom, 3 );
  r += unittests_assert_16k_ram_page( 0x8000, 8 );
  r += unittests_assert_8k_page( 0xc000, memory_source_exrom, 6 );
  r += unittests_assert_8k_page( 0xe000, memory_source_exrom, 7 );

  return r;
}

static int
paging_test( void )
{
  int r = 0;

  switch( machine_current->machine ) {
    case LIBSPECTRUM_MACHINE_16:
      r += paging_test_16();
      break;
    case LIBSPECTRUM_MACHINE_48:
    case LIBSPECTRUM_MACHINE_48_NTSC:
      r += unittests_paging_test_48( 2 );
      break;
    case LIBSPECTRUM_MACHINE_128:
    case LIBSPECTRUM_MACHINE_PLUS2:
    case LIBSPECTRUM_MACHINE_PENT:
      r += paging_test_128();
      break;
    case LIBSPECTRUM_MACHINE_PLUS2A:
    case LIBSPECTRUM_MACHINE_PLUS3:
    case LIBSPECTRUM_MACHINE_PLUS3E:
    case LIBSPECTRUM_MACHINE_128E:
      r += paging_test_plus3();
      break;
    case LIBSPECTRUM_MACHINE_SCORP:
      r += paging_test_scorpion();
      break;
    case LIBSPECTRUM_MACHINE_PENT512:
      r += paging_test_pentagon512();
      break;
    case LIBSPECTRUM_MACHINE_PENT1024:
      r += paging_test_pentagon1024();
      break;
    case LIBSPECTRUM_MACHINE_TC2048:
      r += paging_test_timex( 2, memory_source_none, memory_source_none );
      break;
    case LIBSPECTRUM_MACHINE_TC2068:
    case LIBSPECTRUM_MACHINE_TS2068:
      r += paging_test_timex( 2, memory_source_none, memory_source_exrom );
      break;
    case LIBSPECTRUM_MACHINE_SE:
      r += paging_test_se();
      break;
    case LIBSPECTRUM_MACHINE_UNKNOWN:
      printf( "%s:%d: unknown machine?\n", __FILE__, __LINE__ );
      break;
  }

  /* We don't run the peripheral unit tests with the 16K machine or the
     Spectrum SE so as to avoid the problem with them having different RAM
     pages at 0x8000 and/or 0xc000 */
  if( machine_current->machine != LIBSPECTRUM_MACHINE_16 &&
      machine_current->machine != LIBSPECTRUM_MACHINE_SE    )
  {
    r += if1_unittest();
    r += if2_unittest();
    r += multiface_unittest();
    r += speccyboot_unittest();
    r += ttx2000s_unittest();
    r += usource_unittest();
    r += uspeech_unittest();

    r += beta_unittest();
    r += didaktik80_unittest();
    r += disciple_unittest();
    r += opus_unittest();
    r += plusd_unittest();

    r += divide_unittest();
    r += divmmc_unittest();
    r += zxatasp_unittest();
    r += zxcf_unittest();
  }

  return r;
}

static int
rectangle_test( void )
{
  int saved_frame_rate = settings_current.frame_rate;

  /* --- Test 1: rectangle_add creates a new active rectangle --- */
  rectangle_reset();
  rectangle_add( 0, 0, 10 );
  TEST_ASSERT( rectangle_get_active_count() == 1 );
  TEST_ASSERT( rectangle_inactive_count == 0 );

  /* --- Test 2: rectangle_add extends a matching active rectangle --- */
  rectangle_add( 1, 0, 10 );
  TEST_ASSERT( rectangle_get_active_count() == 1 );

  /* --- Test 3: rectangle_add creates a second rect when x,w differ --- */
  rectangle_add( 1, 5, 8 );
  TEST_ASSERT( rectangle_get_active_count() == 2 );

  /* --- Test 4: rectangle_end_line keeps rects updated on this line --- */
  /* Both rects ended at line 1 (y+h = 2 = 1+1), so both should be kept. */
  rectangle_end_line( 1 );
  TEST_ASSERT( rectangle_get_active_count() == 2 );
  TEST_ASSERT( rectangle_inactive_count == 0 );

  /* --- Test 5: rectangle_end_line flushes stale rects to inactive --- */
  /* y=300 is beyond any rect; both move to inactive (frame_rate == 1). */
  settings_current.frame_rate = 1;
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_get_active_count() == 0 );
  TEST_ASSERT( rectangle_inactive_count == 2 );

  /* --- Test 6 (frame skip): exact duplicate is discarded --- */
  rectangle_reset();
  settings_current.frame_rate = 2;

  /* Build inactive: {x=0, y=0, w=10, h=3} */
  rectangle_add( 0, 0, 10 );
  rectangle_add( 1, 0, 10 );
  rectangle_add( 2, 0, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );

  /* Exact same rect again — should be discarded, count stays 1 */
  rectangle_add( 0, 0, 10 );
  rectangle_add( 1, 0, 10 );
  rectangle_add( 2, 0, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );
  TEST_ASSERT( rectangle_inactive[0].h == 3 );

  /* --- Test 7 (frame skip): adjacent rows are merged --- */
  rectangle_reset();
  settings_current.frame_rate = 2;

  /* inactive: {x=0, y=0, w=10, h=3} (rows 0-2) */
  rectangle_add( 0, 0, 10 );
  rectangle_add( 1, 0, 10 );
  rectangle_add( 2, 0, 10 );
  rectangle_end_line( 300 );

  /* source: {x=0, y=3, w=10, h=1} (row 3) — touches row 2, should merge */
  rectangle_add( 3, 0, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );
  TEST_ASSERT( rectangle_inactive[0].y == 0 );
  TEST_ASSERT( rectangle_inactive[0].h == 4 );

  /* --- Test 8 (frame skip): same-y different-h merge (bug fix) --- */
  rectangle_reset();
  settings_current.frame_rate = 2;

  /* inactive: {x=0, y=0, w=10, h=3} */
  rectangle_add( 0, 0, 10 );
  rectangle_add( 1, 0, 10 );
  rectangle_add( 2, 0, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );

  /* source: {x=0, y=0, w=10, h=5} — same x,w,y but taller; must merge */
  rectangle_add( 0, 0, 10 );
  rectangle_add( 1, 0, 10 );
  rectangle_add( 2, 0, 10 );
  rectangle_add( 3, 0, 10 );
  rectangle_add( 4, 0, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );
  TEST_ASSERT( rectangle_inactive[0].y == 0 );
  TEST_ASSERT( rectangle_inactive[0].h == 5 );

  /* --- Test 9 (frame skip): same-y same-h different-x merge (bug fix) --- */
  rectangle_reset();
  settings_current.frame_rate = 2;

  /* inactive: {x=5, y=0, w=10, h=3} */
  rectangle_add( 0, 5, 10 );
  rectangle_add( 1, 5, 10 );
  rectangle_add( 2, 5, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );

  /* source: {x=5, y=0, w=15, h=3} — same x,y,h but wider; must merge */
  rectangle_add( 0, 5, 15 );
  rectangle_add( 1, 5, 15 );
  rectangle_add( 2, 5, 15 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );
  TEST_ASSERT( rectangle_inactive[0].x == 5 );
  TEST_ASSERT( rectangle_inactive[0].w == 15 );

  /* --- Test 10 (frame skip): y-merge where source is above inactive --- */
  rectangle_reset();
  settings_current.frame_rate = 2;

  /* inactive: {x=0, y=3, w=10, h=2} (rows 3-4) */
  rectangle_add( 3, 0, 10 );
  rectangle_add( 4, 0, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );

  /* source: {x=0, y=2, w=10, h=1} (row 2) — touches row 3 from above */
  rectangle_add( 2, 0, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );
  TEST_ASSERT( rectangle_inactive[0].y == 2 );
  TEST_ASSERT( rectangle_inactive[0].h == 3 );

  /* --- Test 11 (frame skip): x-merge where source is to the left of inactive --- */
  rectangle_reset();
  settings_current.frame_rate = 2;

  /* inactive: {x=5, y=0, w=10, h=3} (columns 5-14) */
  rectangle_add( 0, 5, 10 );
  rectangle_add( 1, 5, 10 );
  rectangle_add( 2, 5, 10 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );

  /* source: {x=0, y=0, w=6, h=3} (columns 0-5) — touches column 5 from left */
  rectangle_add( 0, 0, 6 );
  rectangle_add( 1, 0, 6 );
  rectangle_add( 2, 0, 6 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );
  TEST_ASSERT( rectangle_inactive[0].x == 0 );
  TEST_ASSERT( rectangle_inactive[0].w == 15 );

  /* --- Test 12 (frame skip): non-overlapping rects stay as separate entries --- */
  rectangle_reset();
  settings_current.frame_rate = 2;

  /* inactive: {x=0, y=0, w=5, h=3} */
  rectangle_add( 0, 0, 5 );
  rectangle_add( 1, 0, 5 );
  rectangle_add( 2, 0, 5 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 1 );

  /* source: {x=20, y=10, w=5, h=3} — no overlap in either dimension */
  rectangle_add( 10, 20, 5 );
  rectangle_add( 11, 20, 5 );
  rectangle_add( 12, 20, 5 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 2 );

  settings_current.frame_rate = saved_frame_rate;
  return 0;
}

static int
rectangle_realloc_test( void )
{
  int i;
  int saved_frame_rate = settings_current.frame_rate;

  /* --- Test 1: force active-list reallocation by adding > 8 distinct rects --- */
  /* Initial active allocation is 8; the 9th unique (x,w) pair triggers doubling. */
  rectangle_reset();
  settings_current.frame_rate = 1;
  for( i = 0; i < 9; i++ )
    rectangle_add( 0, i * 10, 5 );
  TEST_ASSERT( rectangle_get_active_count() == 9 );

  /* --- Test 2: continue past 16 to trigger a second doubling (8->16->32) --- */
  for( i = 9; i < 17; i++ )
    rectangle_add( 0, i * 10, 5 );
  TEST_ASSERT( rectangle_get_active_count() == 17 );

  /* --- Test 3: flushing > 8 rects forces inactive-list reallocation --- */
  /* All 17 active rects are stale (line 300 > line 0); they move to inactive. */
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_get_active_count() == 0 );
  TEST_ASSERT( rectangle_inactive_count == 17 );

  /* --- Test 4: a second flush of > 8 non-overlapping rects grows inactive further --- */
  for( i = 0; i < 9; i++ )
    rectangle_add( 1, i * 10 + 5, 3 );
  rectangle_end_line( 300 );
  TEST_ASSERT( rectangle_inactive_count == 26 );

  settings_current.frame_rate = saved_frame_rate;
  return 0;
}

/* Unit tests for scaler_for_size().
   The function looks up which family a scaler belongs to and returns the
   scaler in that family matching the requested 1x–4x size.  Tests cover:
     - all four size positions in the Normal family
     - lookup starting from a non-1x family member
     - size clamping (< 1 → 1, > 4 → 4)
     - scaler not in any fully-registered family returns unchanged
     - family with fewer than all four members registered returns unchanged
     - the TV family's duplicate 1x/2x slot (TV2X appears twice)
     - the Timex family's remapping (SCALER_NORMAL maps to position 2) */
static int
scaler_for_size_test( void )
{
  int r = 0;

  /* --- Normal family: NORMAL(1x), DOUBLESIZE(2x), TRIPLESIZE(3x), QUADSIZE(4x) --- */
  scaler_register_clear();
  scaler_register( SCALER_NORMAL );
  scaler_register( SCALER_DOUBLESIZE );
  scaler_register( SCALER_TRIPLESIZE );
  scaler_register( SCALER_QUADSIZE );

  if( scaler_for_size( SCALER_NORMAL, 1 ) != SCALER_NORMAL ) {
    printf( "scaler_for_size: normal-at-1x: expected SCALER_NORMAL\n" );
    r++;
  }
  if( scaler_for_size( SCALER_NORMAL, 2 ) != SCALER_DOUBLESIZE ) {
    printf( "scaler_for_size: normal-at-2x: expected SCALER_DOUBLESIZE\n" );
    r++;
  }
  if( scaler_for_size( SCALER_NORMAL, 3 ) != SCALER_TRIPLESIZE ) {
    printf( "scaler_for_size: normal-at-3x: expected SCALER_TRIPLESIZE\n" );
    r++;
  }
  if( scaler_for_size( SCALER_NORMAL, 4 ) != SCALER_QUADSIZE ) {
    printf( "scaler_for_size: normal-at-4x: expected SCALER_QUADSIZE\n" );
    r++;
  }

  /* Lookup from a non-1x family member still resolves the correct size */
  if( scaler_for_size( SCALER_DOUBLESIZE, 1 ) != SCALER_NORMAL ) {
    printf( "scaler_for_size: doublesize-at-1x: expected SCALER_NORMAL\n" );
    r++;
  }
  if( scaler_for_size( SCALER_TRIPLESIZE, 4 ) != SCALER_QUADSIZE ) {
    printf( "scaler_for_size: triplesize-at-4x: expected SCALER_QUADSIZE\n" );
    r++;
  }

  /* Size clamping: values < 1 and > 4 are clamped */
  if( scaler_for_size( SCALER_NORMAL, 0 ) != SCALER_NORMAL ) {
    printf( "scaler_for_size: normal-at-0: expected SCALER_NORMAL (clamp to 1)\n" );
    r++;
  }
  if( scaler_for_size( SCALER_NORMAL, 5 ) != SCALER_QUADSIZE ) {
    printf( "scaler_for_size: normal-at-5: expected SCALER_QUADSIZE (clamp to 4)\n" );
    r++;
  }

  /* Scaler not in any family with all members registered: returned unchanged.
     SCALER_DOTMATRIX is a single-size scaler and not listed in any family row. */
  scaler_register( SCALER_DOTMATRIX );
  if( scaler_for_size( SCALER_DOTMATRIX, 2 ) != SCALER_DOTMATRIX ) {
    printf( "scaler_for_size: dotmatrix-at-2x: expected SCALER_DOTMATRIX (no family)\n" );
    r++;
  }

  /* --- Incomplete family: only PALTV2X registered, not PALTV3X or PALTV4X --- */
  scaler_register_clear();
  scaler_register( SCALER_PALTV2X );
  if( scaler_for_size( SCALER_PALTV2X, 3 ) != SCALER_PALTV2X ) {
    printf( "scaler_for_size: paltv2x-incomplete: expected SCALER_PALTV2X\n" );
    r++;
  }

  /* --- TV family: TV2X appears in both the 1x and 2x slot --- */
  scaler_register_clear();
  scaler_register( SCALER_TV2X );
  scaler_register( SCALER_TV3X );
  scaler_register( SCALER_TV4X );

  if( scaler_for_size( SCALER_TV2X, 1 ) != SCALER_TV2X ) {
    printf( "scaler_for_size: tv2x-at-1x: expected SCALER_TV2X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_TV2X, 2 ) != SCALER_TV2X ) {
    printf( "scaler_for_size: tv2x-at-2x: expected SCALER_TV2X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_TV2X, 3 ) != SCALER_TV3X ) {
    printf( "scaler_for_size: tv2x-at-3x: expected SCALER_TV3X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_TV2X, 4 ) != SCALER_TV4X ) {
    printf( "scaler_for_size: tv2x-at-4x: expected SCALER_TV4X\n" );
    r++;
  }

  /* --- Timex family: HALF(1x), NORMAL(2x), TIMEX1_5X(3x), TIMEX2X(4x).
     With this family registered, SCALER_NORMAL is at position 2 (size 2), so
     requesting size 1 from SCALER_NORMAL returns SCALER_HALF. --- */
  scaler_register_clear();
  scaler_register( SCALER_HALF );
  scaler_register( SCALER_NORMAL );
  scaler_register( SCALER_TIMEX1_5X );
  scaler_register( SCALER_TIMEX2X );

  if( scaler_for_size( SCALER_NORMAL, 1 ) != SCALER_HALF ) {
    printf( "scaler_for_size: timex-normal-at-1x: expected SCALER_HALF\n" );
    r++;
  }
  if( scaler_for_size( SCALER_NORMAL, 2 ) != SCALER_NORMAL ) {
    printf( "scaler_for_size: timex-normal-at-2x: expected SCALER_NORMAL\n" );
    r++;
  }
  if( scaler_for_size( SCALER_HALF, 3 ) != SCALER_TIMEX1_5X ) {
    printf( "scaler_for_size: timex-half-at-3x: expected SCALER_TIMEX1_5X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_TIMEX2X, 1 ) != SCALER_HALF ) {
    printf( "scaler_for_size: timex-timex2x-at-1x: expected SCALER_HALF\n" );
    r++;
  }

  /* --- AdvMAME family: ADVMAME2X fills 1x and 2x; ADVMAME3X fills 3x and 4x --- */
  scaler_register_clear();
  scaler_register( SCALER_ADVMAME2X );
  scaler_register( SCALER_ADVMAME3X );

  if( scaler_for_size( SCALER_ADVMAME2X, 1 ) != SCALER_ADVMAME2X ) {
    printf( "scaler_for_size: advmame2x-at-1x: expected SCALER_ADVMAME2X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_ADVMAME2X, 2 ) != SCALER_ADVMAME2X ) {
    printf( "scaler_for_size: advmame2x-at-2x: expected SCALER_ADVMAME2X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_ADVMAME2X, 3 ) != SCALER_ADVMAME3X ) {
    printf( "scaler_for_size: advmame2x-at-3x: expected SCALER_ADVMAME3X\n" );
    r++;
  }
  /* 4x maps to ADVMAME3X — the family has no distinct 4x scaler */
  if( scaler_for_size( SCALER_ADVMAME2X, 4 ) != SCALER_ADVMAME3X ) {
    printf( "scaler_for_size: advmame2x-at-4x: expected SCALER_ADVMAME3X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_ADVMAME3X, 2 ) != SCALER_ADVMAME2X ) {
    printf( "scaler_for_size: advmame3x-at-2x: expected SCALER_ADVMAME2X\n" );
    r++;
  }

  /* --- HQ family: HQ2X fills 1x and 2x; HQ3X at 3x; HQ4X at 4x --- */
  scaler_register_clear();
  scaler_register( SCALER_HQ2X );
  scaler_register( SCALER_HQ3X );
  scaler_register( SCALER_HQ4X );

  if( scaler_for_size( SCALER_HQ2X, 1 ) != SCALER_HQ2X ) {
    printf( "scaler_for_size: hq2x-at-1x: expected SCALER_HQ2X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_HQ2X, 2 ) != SCALER_HQ2X ) {
    printf( "scaler_for_size: hq2x-at-2x: expected SCALER_HQ2X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_HQ2X, 3 ) != SCALER_HQ3X ) {
    printf( "scaler_for_size: hq2x-at-3x: expected SCALER_HQ3X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_HQ2X, 4 ) != SCALER_HQ4X ) {
    printf( "scaler_for_size: hq2x-at-4x: expected SCALER_HQ4X\n" );
    r++;
  }
  if( scaler_for_size( SCALER_HQ4X, 1 ) != SCALER_HQ2X ) {
    printf( "scaler_for_size: hq4x-at-1x: expected SCALER_HQ2X\n" );
    r++;
  }

  return r;
}

static compat_file_vtable_t utils_file_previous_vtable;
static const char *utils_file_test_path;
static int utils_file_open_count;
static int utils_file_read_count;
static int utils_file_total_open_count;
static int utils_file_total_read_count;

static compat_fd
utils_file_test_open( const char *path, int write )
{
  utils_file_total_open_count++;
  if( !strcmp( path, utils_file_test_path ) ) utils_file_open_count++;
  return utils_file_previous_vtable.open( path, write );
}

static int
utils_file_test_read( compat_fd fd, utils_file *file )
{
  utils_file_total_read_count++;
  if( file->filename && !strcmp( file->filename, utils_file_test_path ) )
    utils_file_read_count++;
  return utils_file_previous_vtable.read( fd, file );
}

static compat_fd
utils_file_test_open_failure( const char *path GCC_UNUSED, int write GCC_UNUSED )
{
  return COMPAT_FILE_OPEN_FAILED;
}

static int
utils_file_read_failure_test( void )
{
  compat_file_vtable_t vtable;
  utils_file file;
  int r = 0;

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open_failure;
  compat_file_set_vtable( &vtable );
  utils_file_init( &file, "missing" );
  if( !utils_file_read( &file ) || file.buffer || file.length ) r++;
  utils_file_free( &file );
  compat_file_set_vtable( &utils_file_previous_vtable );
  if( r ) printf( "utils_file_read_failure_test failed\n" );
  return r;
}

static int
utils_file_lifecycle_test( void )
{
  char filename[] = "/tmp/fuse-utils-file-XXXXXX";
  compat_file_vtable_t vtable;
  utils_file file;
  unsigned char data[] = { 0x01, 0x00, 0x00 };
  int fd, r = 0;

  fd = mkstemp( filename );
  if( fd < 0 || write( fd, data, sizeof( data ) ) != sizeof( data ) ) {
    if( fd >= 0 ) close( fd );
    unlink( filename );
    printf( "utils_file_lifecycle_test: failed to create fixture\n" );
    return 1;
  }
  close( fd );

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename;
  utils_file_open_count = utils_file_read_count = 0;
  utils_file_total_open_count = utils_file_total_read_count = 0;
  compat_file_set_vtable( &vtable );

  utils_file_init( &file, filename );
  if( utils_file_identify( &file ) || utils_file_identify( &file ) ||
      utils_file_open_count != 1 || utils_file_read_count != 1 ) r++;

  utils_file_free( &file );
  if( file.filename || file.buffer || file.length ||
      file.type != LIBSPECTRUM_ID_UNKNOWN ||
      file.class != LIBSPECTRUM_CLASS_UNKNOWN ) r++;

  compat_file_set_vtable( &utils_file_previous_vtable );
  unlink( filename );
  if( r ) printf( "utils_file_lifecycle_test failed\n" );
  return r;
}

static int
utils_open_loaded_file_test( void )
{
  compat_file_vtable_t vtable;
  utils_file file;
  static const unsigned char tap[] = { 0x01, 0x00, 0x00 };
  const char *filename = "already-loaded.tap";
  int r = 0;

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename;
  utils_file_open_count = utils_file_read_count = 0;
  compat_file_set_vtable( &vtable );

  utils_file_init( &file, filename );
  file.buffer = libspectrum_new( unsigned char, sizeof( tap ) );
  memcpy( file.buffer, tap, sizeof( tap ) );
  file.length = sizeof( tap );
  file.type = LIBSPECTRUM_ID_TAPE_TAP;
  file.class = LIBSPECTRUM_CLASS_TAPE;

  if( utils_open_loaded_file( &file, 0, NULL ) ||
      utils_file_open_count || utils_file_read_count ) r++;

  utils_file_free( &file );
  tape_close();
  compat_file_set_vtable( &utils_file_previous_vtable );
  if( r ) printf( "utils_open_loaded_file_test failed\n" );
  return r;
}

static int
utils_open_loaded_if2_test( void )
{
  compat_file_vtable_t vtable;
  utils_file file;
  libspectrum_machine old_machine = machine_current->machine;
  int old_interface2 = settings_current.interface2;
  const char *filename = "already-loaded.rom";
  int r = 0;

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename;
  utils_file_open_count = utils_file_read_count = 0;
  compat_file_set_vtable( &vtable );

  utils_file_init( &file, filename );
  file.length = 0x4000;
  file.buffer = libspectrum_new0( unsigned char, file.length );
  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) r++;
  settings_current.interface2 = 1;
  periph_update();
  if( if2_insert_loaded( &file ) || !if2_active ||
      utils_file_open_count || utils_file_read_count ) r++;

  if2_eject();
  settings_current.interface2 = old_interface2;
  if( machine_select( old_machine ) ) r++;
  periph_update();
  compat_file_set_vtable( &utils_file_previous_vtable );
  utils_file_free( &file );
  if( r ) printf( "utils_open_loaded_if2_test failed\n" );
  return r;
}

static int
utils_open_loaded_dck_test( void )
{
  compat_file_vtable_t vtable;
  utils_file file;
  libspectrum_machine old_machine = machine_current->machine;
  const char *filename = "already-loaded.dck";
  int r = 0;

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename;
  utils_file_open_count = utils_file_read_count = 0;
  compat_file_set_vtable( &vtable );

  utils_file_init( &file, filename );
  file.length = 9; /* A valid DCK block: Dock bank with eight empty pages. */
  file.buffer = libspectrum_new0( unsigned char, file.length );
  file.buffer[ 0 ] = LIBSPECTRUM_DCK_BANK_DOCK;
  if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ||
      dck_insert_loaded( &file ) || !dck_active ||
      utils_file_open_count || utils_file_read_count ) r++;

  dck_eject();
  if( machine_select( old_machine ) ) r++;
  compat_file_set_vtable( &utils_file_previous_vtable );
  utils_file_free( &file );
  if( r ) printf( "utils_open_loaded_dck_test failed\n" );
  return r;
}

static int
utils_open_loaded_microdrive_test( void )
{
  compat_file_vtable_t vtable;
  utils_file file;
  const char *filename = "lib/tests/success.mdr";
  int r = 0;

  if( utils_read_file( filename, &file ) ) {
    printf( "utils_open_loaded_microdrive_test: failed to read fixture\n" );
    return 1;
  }

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename;
  utils_file_open_count = utils_file_read_count = 0;
  compat_file_set_vtable( &vtable );

  if( if1_mdr_insert_loaded( -1, &file ) ||
      utils_file_open_count || utils_file_read_count ) r++;

  if1_mdr_eject( 0 );
  compat_file_set_vtable( &utils_file_previous_vtable );
  utils_file_free( &file );
  if( r ) printf( "utils_open_loaded_microdrive_test failed\n" );
  return r;
}

static int
utils_open_loaded_disk_test( void )
{
  compat_file_vtable_t vtable;
  utils_file file;
  disk_t disk;
  const char *filename = "already-loaded.img";
  int r = 0;

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename;
  utils_file_open_count = utils_file_read_count = 0;
  compat_file_set_vtable( &vtable );

  memset( &disk, 0, sizeof( disk ) );
  utils_file_init( &file, filename );
  file.length = 40 * 10 * 512;
  file.buffer = libspectrum_new0( unsigned char, file.length );
  if( disk_open_loaded( &disk, &file, 0, 0 ) != DISK_OK ||
      utils_file_open_count || utils_file_read_count ) r++;

  disk_close( &disk );
  utils_file_free( &file );
  compat_file_set_vtable( &utils_file_previous_vtable );
  if( r ) printf( "utils_open_loaded_disk_test failed\n" );
  return r;
}

static int
utils_open_loaded_disk_merge_test( void )
{
  char temporary_path[] = "/tmp/fuse-disk-merge-XXXXXX";
  char filename_a[ PATH_MAX ], filename_b[ PATH_MAX ];
  compat_file_vtable_t vtable;
  utils_file file;
  disk_t disk;
  unsigned char *data;
  int fd, saved_ask_merge, r = 0;

  /* mkdtemp is not provided by MinGW; use mkstemp to obtain a unique
     filename prefix instead. */
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );
  snprintf( filename_a, sizeof( filename_a ), "%s Side A.img", temporary_path );
  snprintf( filename_b, sizeof( filename_b ), "%s Side B.img", temporary_path );
  data = libspectrum_new0( unsigned char, 40 * 10 * 512 );
  fd = creat( filename_b, 0600 );
  if( !data || fd < 0 || write( fd, data, 40 * 10 * 512 ) != 40 * 10 * 512 ) {
    if( fd >= 0 ) close( fd );
    libspectrum_free( data );
    unlink( filename_b );
    return 1;
  }
  close( fd );

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename_a;
  utils_file_open_count = utils_file_read_count = 0;
  utils_file_total_open_count = utils_file_total_read_count = 0;
  compat_file_set_vtable( &vtable );

  memset( &disk, 0, sizeof( disk ) );
  utils_file_init( &file, filename_a );
  file.buffer = data;
  file.length = 40 * 10 * 512;
  saved_ask_merge = settings_current.disk_ask_merge;
  settings_current.disk_ask_merge = 0;
  if( disk_open_loaded( &disk, &file, 0, 1 ) != DISK_OK ||
      utils_file_open_count || utils_file_read_count ||
      utils_file_total_open_count != 1 || utils_file_total_read_count != 1 ) r++;
  settings_current.disk_ask_merge = saved_ask_merge;

  disk_close( &disk );
  utils_file_free( &file );
  compat_file_set_vtable( &utils_file_previous_vtable );
  unlink( filename_b );
  if( r ) printf( "utils_open_loaded_disk_merge_test failed\n" );
  return r;
}

static FILE compat_file_test_file;
static int compat_file_test_calls[ 6 ];

static compat_fd compat_file_test_open( const char *path GCC_UNUSED,
                                        int write GCC_UNUSED )
{ compat_file_test_calls[ 0 ]++; return &compat_file_test_file; }
static off_t compat_file_test_get_length( compat_fd fd GCC_UNUSED )
{ compat_file_test_calls[ 1 ]++; return 42; }
static int compat_file_test_read( compat_fd fd GCC_UNUSED,
                                  utils_file *file GCC_UNUSED )
{ compat_file_test_calls[ 2 ]++; return 0; }
static int compat_file_test_write( compat_fd fd GCC_UNUSED,
                                   const unsigned char *buffer GCC_UNUSED,
                                   size_t length GCC_UNUSED )
{ compat_file_test_calls[ 3 ]++; return 0; }
static int compat_file_test_close( compat_fd fd GCC_UNUSED )
{ compat_file_test_calls[ 4 ]++; return 0; }
static int compat_file_test_exists( const char *path GCC_UNUSED )
{ compat_file_test_calls[ 5 ]++; return 1; }

static int
compat_file_vtable_test( void )
{
  compat_file_vtable_t vtable = {
    compat_file_test_open, compat_file_test_get_length, compat_file_test_read,
    compat_file_test_write, compat_file_test_close, compat_file_test_exists
  };
  compat_file_vtable_t previous_vtable;
  utils_file file;
  unsigned char buffer = 0;
  compat_fd fd;
  int i, r = 0;

  memset( compat_file_test_calls, 0, sizeof( compat_file_test_calls ) );
  compat_file_get_vtable( &previous_vtable );
  compat_file_set_vtable( &vtable );
  vtable.open = NULL; /* The setter copies operations, like libspectrum's. */

  fd = compat_file_open( "test", 0 );
  if( fd != &compat_file_test_file || compat_file_get_length( fd ) != 42 ||
      compat_file_read( fd, &file ) ||
      compat_file_write( fd, &buffer, 1 ) || compat_file_close( fd ) ||
      !compat_file_exists( "test" ) ) r++;

  compat_file_set_vtable( &previous_vtable );
  for( i = 0; i < 6; i++ ) if( compat_file_test_calls[ i ] != 1 ) r++;
  if( r ) printf( "compat_file_vtable_test failed\n" );
  return r;
}

int
unittests_run( void )
{
  int r = 0;

  r += contention_test();
  r += floating_bus_test();
  r += speaker_filter_test();
  r += ula_filter_test();
  r += ula_sound_levels_test();
  r += floating_bus_merge_test();
  r += snapshot_copy_from_releases_keyboard_test();
  r += snapshot_custom_rom_is_replaced_by_soft_reset_test();
  r += slt_is_cleared_by_reset_test();
  r += slt_screen_is_cleared_by_reset_test();
  r += spec_se_dock_ram_reset_test();
  r += keyboard_read_test();
  r += keyboard_simulate_keypress_test();
  r += utils_safe_strdup_test();
  r += bitmap_ops_test();
  r += mempool_test();
  r += paging_test();
  r += pokefinder_unittest();
  r += debugger_disassemble_unittest();
  r += debugger_expression_unittest();
  r += rectangle_test();
  r += rectangle_realloc_test();
  r += scaler_for_size_test();
  r += compat_file_vtable_test();
  r += utils_file_lifecycle_test();
  r += utils_file_read_failure_test();
  r += utils_open_loaded_file_test();
  r += utils_open_loaded_if2_test();
  r += utils_open_loaded_dck_test();
  r += utils_open_loaded_microdrive_test();
  r += utils_open_loaded_disk_test();
  r += utils_open_loaded_disk_merge_test();

  printf("Final return value: %d (should be 0)\n", r);

  return r;
}
