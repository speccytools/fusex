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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libspectrum.h"

#include "debugger/debugger.h"
#include "display.h"
#include "fuse.h"
#include "input.h"
#include "keyboard.h"
#include "loader.h"
#include "machine.h"
#include "memory_pages.h"
#include "mempool.h"
#include "periph.h"
#include "peripherals/scld.h"
#include "peripherals/ulaplus.h"
#include "peripherals/disk/beta.h"
#include "peripherals/disk/disk.h"
#include "peripherals/disk/crc.h"
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
#include "peripherals/joystick.h"
#include "peripherals/multiface.h"
#include "peripherals/sound/uspeech.h"
#include "peripherals/speccyboot.h"
#include "peripherals/ttx2000s.h"
#include "peripherals/ula.h"
#include "peripherals/usource.h"
#include "psg.h"
#include "pokefinder/pokefinder.h"
#include "settings.h"
#include "sound.h"
#include "sound/ay_engine.h"
#include "sound/blipbuffer.h"
#include "sound/dc_filter.h"
#include "sound/source_synths.h"
#include "sound/speaker_filter.h"
#include "sound/tv_filter.h"
#include "sound/ula_filter.h"
#include "snapshot.h"
#include "tape.h"
#include "bitmap.h"
#include "rectangle.h"
#include "compat.h"
#include "event.h"
#include "rzx.h"
#include "spectrum.h"
#include "screenshot.h"
#include "ui/scaler/scaler.h"
#include "helpers.h"
#include "unittests.h"
#include "utils.h"
#include "z80/z80.h"
#include "z80/z80_macros.h"

#ifdef ENABLE_AUTOMATION
#include "automation/artifacts.h"
#endif /* ENABLE_AUTOMATION */

static int
rzx_automatic_snapshot_count( libspectrum_snap **automatic_snap )
{
  libspectrum_rzx_iterator it;
  int count = 0;

  *automatic_snap = NULL;
  for( it = libspectrum_rzx_iterator_begin( rzx ); it;
       it = libspectrum_rzx_iterator_next( it ) ) {
    if( libspectrum_rzx_iterator_get_type( it ) ==
          LIBSPECTRUM_RZX_SNAPSHOT_BLOCK &&
        libspectrum_rzx_iterator_snap_is_automatic( it ) ) {
      libspectrum_rzx_iterator next;

      count++;
      *automatic_snap = libspectrum_rzx_iterator_get_snap( it );
      next = libspectrum_rzx_iterator_next( it );
      if( !next || libspectrum_rzx_iterator_get_type( next ) !=
                     LIBSPECTRUM_RZX_INPUT_BLOCK )
        return -1;
    }
  }

  return count;
}

static int
rzx_post_interrupt_autosave_test( void )
{
  const char *filename = "/tmp/fuse-rzx-autosave-test.rzx";
  libspectrum_snap *snap;
  libspectrum_dword frame_tstates;
  int old_autosaves, disabled, error = 0, i;

  old_autosaves = settings_current.rzx_autosaves;
  settings_current.rzx_autosaves = 1;
  frame_tstates = machine_current->timings.tstates_per_frame;

  for( disabled = 0; disabled < 2; disabled++ ) {
    if( rzx_start_recording( filename, 0 ) ) {
      error++;
      break;
    }

    for( i = 0; i < 250; i++ ) rzx_frame();
    if( rzx_automatic_snapshot_count( &snap ) != 0 ) error++;

    PC = 0x1234; SP = 0x8000; IM = 1;
    IFF1 = IFF2 = !disabled;
    tstates = frame_tstates;
    spectrum_frame();
    z80_interrupt();

    /* Interrupt acknowledge does not count as an RZX instruction fetch. */
    if( R + rzx_instructions_offset != 0 ) error++;

    rzx_frame_interrupt_complete();
    if( rzx_automatic_snapshot_count( &snap ) != 1 || !snap ) {
      error++;
    } else if( libspectrum_snap_tstates( snap ) != tstates ) {
      error++;
    } else if( disabled ) {
      if( libspectrum_snap_pc( snap ) != 0x1234 ||
          libspectrum_snap_iff1( snap ) || libspectrum_snap_iff2( snap ) )
        error++;
    } else {
      if( libspectrum_snap_pc( snap ) != 0x0038 ||
          libspectrum_snap_iff1( snap ) || libspectrum_snap_iff2( snap ) ||
          tstates >= frame_tstates )
        error++;
    }

    if( rzx_stop_recording() ) error++;
    unlink( filename );
  }

  settings_current.rzx_autosaves = old_autosaves;
  if( error ) printf( "rzx_post_interrupt_autosave_test failed\n" );
  return error;
}

static int
rzx_retrigger_autosave_test( void )
{
  const char *filename = "/tmp/fuse-rzx-retrigger-test.rzx";
  libspectrum_snap *snap;
  int old_autosaves, delayed, error = 0, i;

  old_autosaves = settings_current.rzx_autosaves;
  settings_current.rzx_autosaves = 1;

  for( delayed = 0; delayed < 2; delayed++ ) {
    if( rzx_start_recording( filename, 0 ) ) {
      error++;
      break;
    }

    for( i = 0; i < 250; i++ ) rzx_frame();
    PC = 0x2345; SP = 0x8000; IM = 1; IFF1 = IFF2 = 1;
    tstates = delayed ? 0 : 1;
    z80.interrupts_enabled_at = delayed ? 0 : -1;

    if( delayed ) {
      if( z80_interrupt() || rzx_frame_interrupt_complete() ||
          rzx_automatic_snapshot_count( &snap ) != 0 ) error++;
      tstates++;
    }

    if( !z80_interrupt() ) error++;
    rzx_frame();
    rzx_frame_interrupt_complete();
    if( rzx_automatic_snapshot_count( &snap ) != 1 || !snap ||
        libspectrum_snap_pc( snap ) != 0x0038 ||
        libspectrum_snap_iff1( snap ) || libspectrum_snap_iff2( snap ) )
      error++;

    if( rzx_stop_recording() ) error++;
    unlink( filename );
  }

  settings_current.rzx_autosaves = old_autosaves;
  if( error ) printf( "rzx_retrigger_autosave_test failed\n" );
  return error;
}

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

static int
blip_synth_level_test( void )
{
  Blip_Buffer *buffer = new_Blip_Buffer();
  Blip_Synth *synth = new_Blip_Synth();
  blip_sample_t output[ 128 ];
  long count;
  int i, nonzero;

  TEST_ASSERT( buffer && synth );
  blip_buffer_set_clock_rate( buffer, 1000000 );
  TEST_ASSERT( !blip_buffer_set_sample_rate( buffer, 48000, 100 ) );
  blip_buffer_set_bass_freq( buffer, 0 );
  blip_synth_set_volume( synth, 1.0 );
  blip_synth_set_output( synth, buffer );
  blip_synth_set_treble_eq( synth, 0.0 );

  blip_synth_set_level( synth, 1000 );
  blip_synth_update( synth, 0, 1000 );
  blip_buffer_end_frame( buffer, 2000 );
  count = blip_buffer_read_samples( buffer, output, ARRAY_SIZE( output ), 0 );
  TEST_ASSERT( count > 0 );
  for( i = 0; i < count; i++ ) TEST_ASSERT( output[ i ] == 0 );

  blip_synth_update( synth, 0, 2000 );
  blip_buffer_end_frame( buffer, 2000 );
  count = blip_buffer_read_samples( buffer, output, ARRAY_SIZE( output ), 0 );
  nonzero = 0;
  for( i = 0; i < count; i++ ) if( output[ i ] ) nonzero = 1;
  TEST_ASSERT( nonzero );
  TEST_ASSERT( buffer->reader_accum != 0 );

  blip_buffer_clear( buffer, BLIP_BUFFER_DEF_ENTIRE_BUFF );
  blip_synth_set_output( synth, buffer );
  blip_synth_set_level( synth, 2000 );
  TEST_ASSERT( buffer->reader_accum == 0 );
  TEST_ASSERT( synth->impl.last_amp == 2000 );
  blip_synth_update( synth, 0, 2000 );
  blip_buffer_end_frame( buffer, 2000 );
  count = blip_buffer_read_samples( buffer, output, ARRAY_SIZE( output ), 0 );
  TEST_ASSERT( count > 0 );
  for( i = 0; i < count; i++ ) TEST_ASSERT( output[ i ] == 0 );

  delete_Blip_Synth( &synth );
  delete_Blip_Buffer( &buffer );
  return 0;
}

static double
dc_filter_response( const dc_filter_t *filter, double frequency,
                    int sample_rate )
{
  double omega = 2.0 * 3.14159265358979323846 * frequency / sample_rate;
  double cosine = cos( omega );
  double sine = sin( omega );
  double numerator = filter->decay *
                     hypot( 1.0 - cosine, sine );
  double denominator = hypot( 1.0 - filter->decay * cosine,
                              filter->decay * sine );

  return numerator / denominator;
}

static int
dc_filter_test( void )
{
  static const int sample_rates[] = { 44100, 48000, 96000 };
  dc_filter_t filter;
  double impulse[16];
  int i, rate;

  for( rate = 0; rate < ARRAY_SIZE( sample_rates ); rate++ ) {
    TEST_ASSERT( !dc_filter_configure( &filter, sample_rates[rate] ) );
    TEST_ASSERT( filter.decay > 0.0 && filter.decay < 1.0 );
    TEST_ASSERT( dc_filter_response( &filter, 1.0,
                                     sample_rates[rate] ) < 0.07 );
    TEST_ASSERT( dc_filter_response( &filter, 16.0,
                                     sample_rates[rate] ) > 0.70 );
    TEST_ASSERT( dc_filter_response( &filter, 16.0,
                                     sample_rates[rate] ) < 0.71 );
    TEST_ASSERT( dc_filter_response( &filter, 1000.0,
                                     sample_rates[rate] ) > 0.98 );
  }

  TEST_ASSERT( dc_filter_configure( &filter, 0 ) );
  TEST_ASSERT( !dc_filter_configure( &filter, 48000 ) );
  TEST_ASSERT( dc_filter_apply( &filter, 1234.0 ) == 0.0 );
  TEST_ASSERT( dc_filter_apply( &filter, 1234.0 ) == 0.0 );
  for( i = 0; i < ARRAY_SIZE( impulse ); i++ )
    impulse[i] = dc_filter_apply( &filter, i == 0 ? 2234.0 : 1234.0 );
  TEST_ASSERT( impulse[0] > 0.0 );

  dc_filter_reset( &filter );
  TEST_ASSERT( dc_filter_apply( &filter, 1234.0 ) == 0.0 );
  for( i = 0; i < ARRAY_SIZE( impulse ); i++ )
    TEST_ASSERT( impulse[i] ==
                 dc_filter_apply( &filter, i == 0 ? 2234.0 : 1234.0 ) );

  dc_filter_reset( &filter );
  TEST_ASSERT( dc_filter_apply( &filter, 0.0 ) == 0.0 );
  TEST_ASSERT( fabs( dc_filter_apply( &filter, 1.0e-30 ) ) < 1.0e-20 );
  TEST_ASSERT( filter.state == 0.0 );

  return 0;
}

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
    TEST_ASSERT( !speaker_filter_configure( &filter, sample_rates[ rate ] ) );
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

  TEST_ASSERT( speaker_filter_configure( &filter, 0 ) );

  TEST_ASSERT( !speaker_filter_configure( &filter, 48000 ) );
  TEST_ASSERT( speaker_filter_apply( &filter, 1234.0 ) == 0.0 );
  TEST_ASSERT( fabs( speaker_filter_apply( &filter, 1234.0 ) ) < 1.0e-12 );
  for( i = 0; i < ARRAY_SIZE( output ); i++ )
    output[ i ] = speaker_filter_apply( &filter,
                                         i == 0 ? 2234.0 : 1234.0 );
  TEST_ASSERT( output[ 0 ] != 0.0 );

  speaker_filter_reset( &filter );
  TEST_ASSERT( speaker_filter_apply( &filter, 1234.0 ) == 0.0 );
  TEST_ASSERT( fabs( speaker_filter_apply( &filter, 1234.0 ) ) < 1.0e-12 );
  for( i = 0; i < ARRAY_SIZE( output ); i++ )
    TEST_ASSERT( output[ i ] == speaker_filter_apply(
                                   &filter, i == 0 ? 2234.0 : 1234.0 ) );

  /* Sub-threshold residual state must be flushed, as in the dc and tv
   * filter tests, so processors without denormal flushing cannot slow a
   * long silence down. */
  speaker_filter_reset( &filter );
  TEST_ASSERT( speaker_filter_apply( &filter, 0.0 ) == 0.0 );
  TEST_ASSERT( fabs( speaker_filter_apply( &filter, 1.0e-30 ) ) < 1.0e-20 );
  TEST_ASSERT( filter.z1 == 0.0 );
  TEST_ASSERT( filter.z2 == 0.0 );

  return 0;
}

static double
tv_filter_response( const tv_filter_t *filter, double frequency,
                    int sample_rate )
{
  double omega = 2.0 * 3.14159265358979323846 * frequency / sample_rate;
  double complex_z_real = cos( omega );
  double complex_z_imaginary = -sin( omega );
  double one_minus_z_real = 1.0 - complex_z_real;
  double one_minus_z_imaginary = -complex_z_imaginary;
  double hp_den_real = 1.0 - filter->high_pass_decay * complex_z_real;
  double hp_den_imaginary = -filter->high_pass_decay * complex_z_imaginary;
  double lp_decay = 1.0 - filter->low_pass_alpha;
  double lp_den_real = 1.0 - lp_decay * complex_z_real;
  double lp_den_imaginary = -lp_decay * complex_z_imaginary;
  double hp = filter->high_pass_decay *
              hypot( one_minus_z_real, one_minus_z_imaginary ) /
              hypot( hp_den_real, hp_den_imaginary );
  double lp = filter->low_pass_alpha /
              hypot( lp_den_real, lp_den_imaginary );

  return hp * lp;
}

static int
tv_filter_test( void )
{
  static const int sample_rates[] = { 44100, 48000, 96000 };
  tv_filter_t filter;
  double impulse[16];
  int i, rate;

  for( rate = 0; rate < ARRAY_SIZE( sample_rates ); rate++ ) {
    TEST_ASSERT( !tv_filter_configure( &filter, sample_rates[rate] ) );
    TEST_ASSERT( filter.high_pass_decay > 0.0 &&
                 filter.high_pass_decay < 1.0 );
    TEST_ASSERT( filter.low_pass_alpha > 0.0 &&
                 filter.low_pass_alpha < 1.0 );
    TEST_ASSERT( tv_filter_response( &filter, 10.0,
                                     sample_rates[rate] ) < 0.11 );
    TEST_ASSERT( tv_filter_response( &filter, 1000.0,
                                     sample_rates[rate] ) > 0.9 );
    TEST_ASSERT( tv_filter_response( &filter, 20000.0,
                                     sample_rates[rate] ) < 0.65 );
  }

  TEST_ASSERT( tv_filter_configure( &filter, 0 ) );
  TEST_ASSERT( tv_filter_configure( &filter, 20000 ) );
  TEST_ASSERT( !tv_filter_configure( &filter, 48000 ) );
  TEST_ASSERT( tv_filter_apply( &filter, 1234.0 ) == 0.0 );
  for( i = 0; i < ARRAY_SIZE( impulse ); i++ )
    impulse[i] = tv_filter_apply( &filter, i == 0 ? 2234.0 : 1234.0 );
  tv_filter_reset( &filter );
  TEST_ASSERT( tv_filter_apply( &filter, 1234.0 ) == 0.0 );
  for( i = 0; i < ARRAY_SIZE( impulse ); i++ )
    TEST_ASSERT( impulse[i] ==
                 tv_filter_apply( &filter, i == 0 ? 2234.0 : 1234.0 ) );

  tv_filter_reset( &filter );
  TEST_ASSERT( tv_filter_apply( &filter, 0.0 ) == 0.0 );
  TEST_ASSERT( fabs( tv_filter_apply( &filter, 1.0e-30 ) ) < 1.0e-20 );
  TEST_ASSERT( filter.high_pass_state == 0.0 );
  TEST_ASSERT( filter.low_pass_state == 0.0 );

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
sound_source_routes_test( void )
{
  int beeper = LIBSPECTRUM_MACHINE_CAPABILITY_BEEPER;
  int ay = LIBSPECTRUM_MACHINE_CAPABILITY_AY;
  unsigned int tv_base = SOUND_ROUTE_ULA_MIC | SOUND_ROUTE_USPEECH;

  TEST_ASSERT( sound_resolve_speaker_type( SOUND_SPEAKER_TYPE_AUTOMATIC,
                                           beeper, 0 ) ==
               SOUND_SPEAKER_TYPE_BEEPER );
  TEST_ASSERT( sound_resolve_speaker_type( SOUND_SPEAKER_TYPE_AUTOMATIC,
                                           beeper, 1 ) ==
               SOUND_SPEAKER_TYPE_TV );
  TEST_ASSERT( sound_resolve_speaker_type( SOUND_SPEAKER_TYPE_AUTOMATIC,
                                           0, 0 ) == SOUND_SPEAKER_TYPE_TV );
  TEST_ASSERT( sound_resolve_speaker_type( SOUND_SPEAKER_TYPE_BEEPER,
                                           0, 1 ) ==
               SOUND_SPEAKER_TYPE_BEEPER );

  TEST_ASSERT( sound_tv_source_routes( SOUND_SPEAKER_TYPE_TV, 0 ) ==
               tv_base );
  TEST_ASSERT( sound_tv_source_routes( SOUND_SPEAKER_TYPE_TV, ay ) ==
               ( tv_base | SOUND_ROUTE_BUILTIN_AY ) );
  TEST_ASSERT( sound_tv_source_routes( SOUND_SPEAKER_TYPE_BEEPER,
                                       beeper | ay ) == 0 );
  TEST_ASSERT( sound_tv_source_routes( SOUND_SPEAKER_TYPE_UNFILTERED,
                                       beeper | ay ) == 0 );
  /* Unfiltered processes only the separate ULA stream; AY remains in the
   * unprocessed main mix rather than entering the routed TV bus. */
  TEST_ASSERT( !( sound_tv_source_routes( SOUND_SPEAKER_TYPE_UNFILTERED, ay ) &
                  SOUND_ROUTE_BUILTIN_AY ) );

  /* Covox, SpecDrum and external AY deliberately have no TV route bits. */
  TEST_ASSERT( ( tv_base | SOUND_ROUTE_BUILTIN_AY ) == 0x0d );

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
source_volume_test( void )
{
  double v;

  /* Below range clamps to silence (0.0) */
  v = source_volume( -1 ); TEST_ASSERT( v == 0.0 );

  /* Exact lower boundary is silence (0.0) */
  v = source_volume( 0 ); TEST_ASSERT( v == 0.0 );

  /* In-range values scale linearly (percent / 100) */
  v = source_volume( 50 ); TEST_ASSERT( v == 0.5 );
  v = source_volume( 25 ); TEST_ASSERT( v == 0.25 );

  /* Exact upper boundary is full gain (1.0) */
  v = source_volume( 100 ); TEST_ASSERT( v == 1.0 );

  /* Above range clamps to full gain (1.0) */
  v = source_volume( 101 ); TEST_ASSERT( v == 1.0 );
  v = source_volume( 10000 ); TEST_ASSERT( v == 1.0 );

  return 0;
}

static int
ay_render_grid_continuity_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_dword frame, offset, last_tick, expected, seen;
  int frames, r = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_128 ) ) r++;

  frame = machine_current->timings.tstates_per_frame;

  /* AY rendering runs on a 32-tstate grid; 128K frames are 70908 tstates
     long, so only a carried offset keeps the grid continuous. */
  if( !r && frame != 70908 ) r++;

  for( frames = 0; !r && frames < 9; frames++ ) {
    /* The last tick of this frame is the largest grid position below the
       frame end; the next tick must land exactly one grid step later in
       machine time, whatever frame-relative offset that is. */
    offset = ay_engine_next_tick_offset();
    last_tick = offset + 32 * ( ( frame - 1 - offset ) / 32 );
    ay_engine_render( frame );
    ay_engine_end_frame();
    expected = frames * frame + last_tick + 32;
    seen = ( frames + 1 ) * frame + ay_engine_next_tick_offset();
    if( seen != expected ) r++;
  }

  if( machine_select( old_machine ) ) r++;

  if( r ) printf( "ay_render_grid_continuity_test failed\n" );
  return r;
}

static int
ay_tail_write_survives_frame_boundary_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_dword frame;
  int r = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_128 ) ) r++;

  frame = machine_current->timings.tstates_per_frame;

  if( !r ) {
    /* A write queued before the last tick is applied by this frame's
       first tick at or after its tstate. */
    ay_engine_write( 6, 0x31, 100 );
    ay_engine_render( frame );
    ay_engine_end_frame();
    if( ay_engine_register_value( 6 ) != 0x31 ) r++;

    /* A write queued after the last rendering tick has no eligible tick
       left in this frame; it must wait for the next frame's first tick
       instead of being dropped. */
    ay_engine_write( 9, 0x55, frame - 8 );
    ay_engine_render( frame );
    ay_engine_end_frame();
    if( ay_engine_register_value( 9 ) != 0 ) r++;
    ay_engine_render( frame );
    ay_engine_end_frame();
    if( ay_engine_register_value( 9 ) != 0x55 ) r++;
  }

  if( machine_select( old_machine ) ) r++;

  if( r ) printf( "ay_tail_write_survives_frame_boundary_test failed\n" );
  return r;
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
snapshot_copy_from_releases_joystick_test( void )
{
  libspectrum_snap *snap;
  libspectrum_byte attached;
  int old_output = settings_current.joystick_1_output;

  snap = libspectrum_snap_alloc();
  TEST_ASSERT( snap != NULL );

  settings_current.joystick_1_output = JOYSTICK_TYPE_NONE;
  TEST_ASSERT( snapshot_copy_to( snap ) == 0 );

  settings_current.joystick_1_output = JOYSTICK_TYPE_KEMPSTON;
  joystick_press( 0, JOYSTICK_BUTTON_FIRE, 1 );
  TEST_ASSERT( ( joystick_kempston_read( 0, &attached ) & 0x10 ) != 0 );

  TEST_ASSERT( snapshot_copy_from( snap ) == 0 );
  TEST_ASSERT( joystick_kempston_read( 0, &attached ) == 0 );

  settings_current.joystick_1_output = old_output;
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
snapshot_machine_rom_without_file_test( void )
{
  static char missing_rom[] = "/nonexistent/snapshot-machine.rom";
  libspectrum_machine old_machine = machine_current->machine;
  char *saved_rom = settings_current.rom_48_0;
  char *saved_default_rom = settings_default.rom_48_0;
  libspectrum_snap *snap;
  libspectrum_byte *rom;
  int r = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) return 1;
  snap = libspectrum_snap_alloc();
  if( !snap ) return 1;
  if( snapshot_copy_to( snap ) ) { libspectrum_snap_free( snap ); return 1; }

  rom = libspectrum_new( libspectrum_byte, 0x4000 );
  memset( rom, 0xa5, 0x4000 );
  libspectrum_snap_set_custom_rom( snap, 1 );
  libspectrum_snap_set_custom_rom_pages( snap, 1 );
  libspectrum_snap_set_roms( snap, 0, rom );
  libspectrum_snap_set_rom_length( snap, 0, 0x4000 );

  settings_current.rom_48_0 = missing_rom;
  settings_default.rom_48_0 = missing_rom;
  if( snapshot_copy_from( snap ) || !memory_map_rom[ 0 ].page ||
      memory_map_rom[ 0 ].page[ 0 ] != 0xa5 ) r++;

  settings_current.rom_48_0 = saved_rom;
  settings_default.rom_48_0 = saved_default_rom;
  if( machine_select( LIBSPECTRUM_MACHINE_128 ) ) r++;

  settings_current.rom_48_0 = missing_rom;
  settings_default.rom_48_0 = missing_rom;
  if( snapshot_copy_from( snap ) || !memory_map_rom[ 0 ].page ||
      memory_map_rom[ 0 ].page[ 0 ] != 0xa5 ) r++;

  settings_current.rom_48_0 = saved_rom;
  settings_default.rom_48_0 = saved_default_rom;
  if( machine_select( old_machine ) ) r++;
  if( libspectrum_snap_free( snap ) ) r++;
  return r;
}

static int
snapshot_missing_rom_file_uses_snapshot_rom_test( void )
{
  libspectrum_snap *snap;
  libspectrum_byte *rom;
  char *saved_rom_plusd;
  char *saved_default_rom_plusd;
  static char missing_rom[] = "/nonexistent/plusd.rom";
  int saved_plusd = settings_current.plusd;
  int r = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) return 1;

  snap = libspectrum_snap_alloc();
  if( !snap ) return 1;

  /* A +D snapshot ROM that the +D reset cannot find on disk */
  rom = libspectrum_new( libspectrum_byte, 0x2000 );
  if( !rom ) {
    libspectrum_snap_free( snap );
    return 1;
  }
  memset( rom, 0xa5, 0x2000 );
  libspectrum_snap_set_machine( snap, LIBSPECTRUM_MACHINE_48 );
  libspectrum_snap_set_plusd_active( snap, 1 );
  libspectrum_snap_set_plusd_custom_rom( snap, 1 );
  libspectrum_snap_set_plusd_rom( snap, 0, rom );

  /* Hide both the configured +D ROM and its fallback so the +D reset cannot
     read any ROM from disk and has to use the snapshot's ROM bank */
  saved_rom_plusd = utils_safe_strdup( settings_current.rom_plusd );
  saved_default_rom_plusd = utils_safe_strdup( settings_default.rom_plusd );
  settings_current.rom_plusd = missing_rom;
  settings_default.rom_plusd = missing_rom;

  /* Soft reset: the snapshot's machine is the machine already selected */
  if( snapshot_copy_from( snap ) || !plusd_available ||
      periph_is_active( PERIPH_TYPE_PLUSD ) != 1 ||
      settings_current.plusd != 1 )
    r++;

  plusd_page();
  if( memory_map_read[ 0 ].page[ 0 ] != 0xa5 ) r++;
  plusd_unpage();

  /* Machine change: select a different machine first, then let the snapshot
     select its own machine */
  settings_current.rom_plusd = saved_rom_plusd;
  settings_default.rom_plusd = saved_default_rom_plusd;

  if( machine_select( LIBSPECTRUM_MACHINE_128 ) ) r++;

  settings_current.rom_plusd = missing_rom;
  settings_default.rom_plusd = missing_rom;

  if( snapshot_copy_from( snap ) || !plusd_available ||
      periph_is_active( PERIPH_TYPE_PLUSD ) != 1 ||
      settings_current.plusd != 1 )
    r++;

  plusd_page();
  if( memory_map_read[ 0 ].page[ 0 ] != 0xa5 ) r++;
  plusd_unpage();

  settings_current.rom_plusd = saved_rom_plusd;
  settings_default.rom_plusd = saved_default_rom_plusd;
  settings_current.plusd = saved_plusd;

  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) r++;

  if( libspectrum_snap_free( snap ) ) r++;

  return r;
}

static int
snapshot_write_read_roundtrip_test( void )
{
  char temporary_path[] = "/tmp/fuse-snapshot-roundtrip-XXXXXX";
  char filename[ PATH_MAX ];
  libspectrum_machine old_machine = machine_current->machine;
  int fd, r = 0;

  /* mkdtemp is not provided by MinGW; use mkstemp to obtain a unique
     filename prefix, as in the disk round-trip test */
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );
  snprintf( filename, sizeof( filename ), "%s.szx", temporary_path );

  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) r++;

  /* Write a snapshot of the selected machine to a real file */
  if( !r && snapshot_write( filename ) ) r++;

  /* Select a different machine first, so loading the snapshot has to
     restore the written snapshot's own machine */
  if( !r && machine_select( LIBSPECTRUM_MACHINE_128 ) ) r++;

  if( !r && snapshot_read( filename ) ) r++;

  if( !r && machine_current->machine != LIBSPECTRUM_MACHINE_48 ) r++;

  if( machine_select( old_machine ) ) r++;

  unlink( filename );

  if( r ) printf( "snapshot_write_read_roundtrip_test failed\n" );
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
snapshot_dock_carts_cleared_by_hard_reset_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_snap *snap;
  libspectrum_snap *result;
  libspectrum_byte *cart;
  int r = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) return 1;

  snap = libspectrum_snap_alloc();
  result = libspectrum_snap_alloc();
  if( !snap || !result ) {
    if( snap ) libspectrum_snap_free( snap );
    if( result ) libspectrum_snap_free( result );
    return 1;
  }

  libspectrum_snap_set_machine( snap, machine_current->machine );
  libspectrum_snap_set_dock_active( snap, 1 );
  cart = libspectrum_new( libspectrum_byte, 0x2000 );
  memset( cart, 0xa5, 0x2000 );
  libspectrum_snap_set_dock_cart( snap, 0, cart );
  libspectrum_snap_set_dock_ram( snap, 0, 1 );

  /* Loading a snapshot carrying a DOCK cartridge on a machine without dock
     support still maps the cartridge: scld_from_snapshot() copies it into a
     shared cartridge bank and points the DOCK page mappings at it. */
  if( snapshot_copy_from( snap ) ) r++;
  TEST_ASSERT( timex_dock[ 0 ].page != NULL );
  TEST_ASSERT( timex_dock[ 0 ].page[ 0 ] == 0xa5 );

  /* A hard reset drops the bank data; the mappings must be dropped with
     it rather than left pointing into the freed bank. */
  if( machine_reset( 1 ) ) r++;
  TEST_ASSERT( timex_dock[ 0 ].page == NULL );
  TEST_ASSERT( timex_exrom[ 0 ].page == NULL );
  TEST_ASSERT( !timex_dock[ 0 ].save_to_snapshot );
  TEST_ASSERT( !timex_dock[ 0 ].writable );
  TEST_ASSERT( !timex_exrom[ 0 ].save_to_snapshot );
  TEST_ASSERT( !timex_exrom[ 0 ].writable );

  /* Saving a snapshot afterwards must not resurrect cartridges out of
     the freed bank. */
  if( snapshot_copy_to( result ) ) r++;
  TEST_ASSERT( libspectrum_snap_dock_cart( result, 0 ) == NULL );
  TEST_ASSERT( libspectrum_snap_exrom_cart( result, 0 ) == NULL );
  TEST_ASSERT( !libspectrum_snap_dock_ram( result, 0 ) );
  TEST_ASSERT( !libspectrum_snap_exrom_ram( result, 0 ) );

  if( libspectrum_snap_free( snap ) ) r++;
  if( libspectrum_snap_free( result ) ) r++;

  if( machine_select( old_machine ) ) r++;

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
ulaplus_ports_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_machine timex_machines[] = {
    LIBSPECTRUM_MACHINE_TC2048, LIBSPECTRUM_MACHINE_TC2068,
    LIBSPECTRUM_MACHINE_TS2068, LIBSPECTRUM_MACHINE_SE,
  };
  libspectrum_machine sinclair_machines[] = {
    LIBSPECTRUM_MACHINE_48, LIBSPECTRUM_MACHINE_128,
    LIBSPECTRUM_MACHINE_PENT, LIBSPECTRUM_MACHINE_SCORP,
  };
  size_t i;
  int r = 0;

  settings_current.ulaplus = 1;

  for( i = 0; i < ARRAY_SIZE( sinclair_machines ); i++ ) {
    if( machine_select( sinclair_machines[ i ] ) ) { r++; continue; }
    periph_update();

    TEST_ASSERT( periph_is_active( PERIPH_TYPE_ULAPLUS ) );
    TEST_ASSERT( !ulaplus_is_enabled() );

    /* The standard colours are loaded: bright magenta ink, normal cyan paper */
    TEST_ASSERT( ulaplus_get_colour( 3 ) == 0x12 );
    TEST_ASSERT( ulaplus_get_colour( 16 + 3 ) == 0x1f );
    TEST_ASSERT( ulaplus_get_colour( 32 + 8 + 5 ) == 0x82 );
    TEST_ASSERT( ulaplus_get_colour( 48 + 8 + 5 ) == 0xe3 );

    /* Group 0 selects a palette entry */
    writeport_internal( 0xbf3b, 0x05 );
    writeport_internal( 0xff3b, 0x42 );
    TEST_ASSERT( ulaplus_get_colour( 5 ) == 0x42 );
    TEST_ASSERT( readport_internal( 0xff3b ) == 0x42 );
    writeport_internal( 0xbf3b, 0x3f );
    writeport_internal( 0xff3b, 0xa5 );
    TEST_ASSERT( ulaplus_get_colour( 63 ) == 0xa5 );

    /* Group 1 switches the mode, and only bit 0 matters */
    writeport_internal( 0xbf3b, 0x40 );
    writeport_internal( 0xff3b, 0x01 );
    TEST_ASSERT( ulaplus_is_enabled() );
    TEST_ASSERT( readport_internal( 0xff3b ) == 0x01 );
    writeport_internal( 0xff3b, 0xfe );
    TEST_ASSERT( !ulaplus_is_enabled() );
    TEST_ASSERT( readport_internal( 0xff3b ) == 0x00 );
    writeport_internal( 0xff3b, 0x01 );

    /* Other groups are ignored and read back as 0xff */
    writeport_internal( 0xbf3b, 0x85 );
    writeport_internal( 0xff3b, 0x00 );
    TEST_ASSERT( ulaplus_is_enabled() );
    TEST_ASSERT( ulaplus_get_colour( 5 ) == 0x42 );
    TEST_ASSERT( readport_internal( 0xff3b ) == 0xff );

    /* Other ports do not select the registers */
    writeport_internal( 0xbf3a, 0x05 );
    writeport_internal( 0xff3a, 0x00 );
    TEST_ASSERT( ulaplus_get_colour( 5 ) == 0x42 );

    /* A reset switches the mode off and restores the standard colours */
    if( machine_reset( 0 ) ) r++;
    TEST_ASSERT( !ulaplus_is_enabled() );
    TEST_ASSERT( ulaplus_get_colour( 5 ) == 0x82 );
    TEST_ASSERT( ulaplus_get_colour( 63 ) == 0xff );

    /* With the option off the ports do nothing */
    settings_current.ulaplus = 0;
    periph_update();
    TEST_ASSERT( !periph_is_active( PERIPH_TYPE_ULAPLUS ) );
    writeport_internal( 0xbf3b, 0x40 );
    writeport_internal( 0xff3b, 0x01 );
    TEST_ASSERT( !ulaplus_is_enabled() );
    settings_current.ulaplus = 1;
  }

  for( i = 0; i < ARRAY_SIZE( timex_machines ); i++ ) {
    if( machine_select( timex_machines[ i ] ) ) { r++; continue; }
    periph_update();

    TEST_ASSERT( !periph_is_active( PERIPH_TYPE_ULAPLUS ) );
    writeport_internal( 0xbf3b, 0x40 );
    writeport_internal( 0xff3b, 0x01 );
    TEST_ASSERT( !ulaplus_is_enabled() );
  }

  settings_current.ulaplus = 0;
  if( machine_select( old_machine ) ) r++;
  periph_update();

  if( r ) printf( "ulaplus_ports_test failed\n" );
  return r;
}

/* Write the current state to an SZX buffer, then change the ULA+ state, then
   load the buffer back */
static int
ulaplus_snapshot_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_snap *snap;
  libspectrum_byte *buffer = NULL;
  size_t length = 0;
  int flags = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) return 1;

  settings_current.ulaplus = 1;
  periph_update();
  writeport_internal( 0xbf3b, 0x05 );
  writeport_internal( 0xff3b, 0x42 );
  writeport_internal( 0xbf3b, 0x40 );
  writeport_internal( 0xff3b, 0x01 );

  snap = libspectrum_snap_alloc();
  TEST_ASSERT( snapshot_copy_to( snap ) == 0 );
  TEST_ASSERT( libspectrum_snap_ulaplus_active( snap ) );
  TEST_ASSERT( libspectrum_snap_write( &buffer, &length, &flags, snap,
                                       LIBSPECTRUM_ID_SNAPSHOT_SZX, NULL,
                                       0 ) == 0 );
  TEST_ASSERT( libspectrum_snap_free( snap ) == 0 );

  settings_current.ulaplus = 0;
  periph_update();
  TEST_ASSERT( machine_reset( 0 ) == 0 );
  TEST_ASSERT( !ulaplus_is_enabled() );

  TEST_ASSERT( snapshot_read_buffer( buffer, length,
                                     LIBSPECTRUM_ID_SNAPSHOT_SZX ) == 0 );
  TEST_ASSERT( settings_current.ulaplus );
  TEST_ASSERT( ulaplus_is_enabled() );
  TEST_ASSERT( ulaplus_get_colour( 5 ) == 0x42 );
  TEST_ASSERT( readport_internal( 0xff3b ) == 0x01 );
  libspectrum_free( buffer );

  /* A snapshot taken with the option off turns it off */
  settings_current.ulaplus = 0;
  periph_update();
  snap = libspectrum_snap_alloc();
  TEST_ASSERT( snapshot_copy_to( snap ) == 0 );
  TEST_ASSERT( !libspectrum_snap_ulaplus_active( snap ) );
  TEST_ASSERT( libspectrum_snap_free( snap ) == 0 );

  settings_current.ulaplus = 0;
  if( machine_select( old_machine ) ) return 1;
  periph_update();

  return 0;
}

static int
spec_se_dock_active_machine_switch_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_snap *snap;
  int r = 0;

  if( machine_select( LIBSPECTRUM_MACHINE_SE ) ) return 1;

  /* The dock is always active on the SE */
  if( !dck_active ) r++;

  /* Switching to a machine without Timex-style video support drops the
     dock state, so snapshots saved there claim no dock carts */
  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) r++;
  else {
    if( dck_active ) r++;

    snap = libspectrum_snap_alloc();
    if( !snap ) return 1;
    if( snapshot_copy_to( snap ) || libspectrum_snap_dock_active( snap ) ||
        libspectrum_snap_dock_cart( snap, 0 ) ||
        libspectrum_snap_exrom_cart( snap, 0 ) ) r++;
    if( libspectrum_snap_free( snap ) ) r++;
  }

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
keyboard_synthetic_test( void )
{
  keyboard_release_all();
  keyboard_synthetic_release_all( KEYBOARD_SYNTHETIC_PHANTOM_TYPIST );
  keyboard_synthetic_release_all( KEYBOARD_SYNTHETIC_DISCIPLE );

  keyboard_synthetic_press( KEYBOARD_SYNTHETIC_PHANTOM_TYPIST, KEYBOARD_Caps );
  keyboard_synthetic_press( KEYBOARD_SYNTHETIC_DISCIPLE, KEYBOARD_a );
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xfe );
  TEST_ASSERT( keyboard_read( 0xfd ) == 0xfe );

  keyboard_synthetic_release_all( KEYBOARD_SYNTHETIC_PHANTOM_TYPIST );
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xff );
  TEST_ASSERT( keyboard_read( 0xfd ) == 0xfe );

  keyboard_press( KEYBOARD_a );
  keyboard_synthetic_release_all( KEYBOARD_SYNTHETIC_DISCIPLE );
  TEST_ASSERT( keyboard_read( 0xfd ) == 0xfe );
  keyboard_release( KEYBOARD_a );
  TEST_ASSERT( keyboard_read( 0xfd ) == 0xff );

  keyboard_synthetic_press( KEYBOARD_SYNTHETIC_JOYSTICK_FIRE, KEYBOARD_7 );
  keyboard_release( KEYBOARD_7 );
  TEST_ASSERT( keyboard_read( 0xef ) == 0xf7 );
  keyboard_synthetic_release( KEYBOARD_SYNTHETIC_JOYSTICK_FIRE, KEYBOARD_7 );
  TEST_ASSERT( keyboard_read( 0xef ) == 0xff );

  keyboard_synthetic_press( KEYBOARD_SYNTHETIC_JOYSTICK_FIRE, KEYBOARD_7 );
  keyboard_synthetic_press( KEYBOARD_SYNTHETIC_DISCIPLE, KEYBOARD_a );
  keyboard_release_all();
  TEST_ASSERT( keyboard_read( 0xef ) == 0xff );
  TEST_ASSERT( keyboard_read( 0xfd ) == 0xfe );
  keyboard_synthetic_release_all( KEYBOARD_SYNTHETIC_DISCIPLE );

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
keyboard_shifted_arrows_test( void )
{
  /* Regression test for bug #470: with keyboard_arrows_shifted enabled,
     holding two cursor keys must keep Caps Shift pressed until the last
     cursor key is released.  Previously releasing one of two held cursor
     keys dropped Caps Shift while the other was still held. */
  input_event_t event;
  int old_shifted = settings_current.keyboard_arrows_shifted;

  settings_current.keyboard_arrows_shifted = 1;
  keyboard_release_all();

  /* Caps Shift lives in half-row 0, bit 0x01, so keyboard_read( 0xfe )
     (selecting only half-row 0) reflects whether Caps Shift is pressed. */

  /* Press Up: KEYBOARD_7 plus Caps Shift */
  event.type = INPUT_EVENT_KEYPRESS;
  event.types.key.native_key = INPUT_KEY_Up;
  event.types.key.spectrum_key = INPUT_KEY_Up;
  input_event( &event );
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xfe ); /* Caps Shift pressed */

  /* Press Left while Up is still held: KEYBOARD_5 plus Caps Shift */
  event.type = INPUT_EVENT_KEYPRESS;
  event.types.key.native_key = INPUT_KEY_Left;
  event.types.key.spectrum_key = INPUT_KEY_Left;
  input_event( &event );

  /* Release Up: Caps Shift must stay pressed because Left is still held */
  event.type = INPUT_EVENT_KEYRELEASE;
  event.types.key.native_key = INPUT_KEY_Up;
  event.types.key.spectrum_key = INPUT_KEY_Up;
  input_event( &event );
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xfe );

  /* Release Left: last cursor key released, Caps Shift released */
  event.type = INPUT_EVENT_KEYRELEASE;
  event.types.key.native_key = INPUT_KEY_Left;
  event.types.key.spectrum_key = INPUT_KEY_Left;
  input_event( &event );
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xff );

  settings_current.keyboard_arrows_shifted = old_shifted;
  return 0;
}

static int
keyboard_shifted_arrows_release_all_test( void )
{
  /* Regression test for the edge case of bug #470: releasing all keys
     (e.g. focus loss or emulation pause) must also reset the shifted-arrow
     tracking state.  Otherwise a later cursor-key press would be treated
     as already held and Caps Shift would not be re-pressed. */
  input_event_t event;
  int old_shifted = settings_current.keyboard_arrows_shifted;

  settings_current.keyboard_arrows_shifted = 1;
  keyboard_release_all();

  /* Press Up: Caps Shift pressed */
  event.type = INPUT_EVENT_KEYPRESS;
  event.types.key.native_key = INPUT_KEY_Up;
  event.types.key.spectrum_key = INPUT_KEY_Up;
  input_event( &event );
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xfe );

  /* Release all keys (e.g. focus loss / pause): Caps Shift released */
  keyboard_release_all();
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xff );

  /* Press Up again: Caps Shift must be re-pressed (state was reset) */
  event.type = INPUT_EVENT_KEYPRESS;
  event.types.key.native_key = INPUT_KEY_Up;
  event.types.key.spectrum_key = INPUT_KEY_Up;
  input_event( &event );
  TEST_ASSERT( keyboard_read( 0xfe ) == 0xfe );

  keyboard_release_all();
  settings_current.keyboard_arrows_shifted = old_shifted;
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
psg_unittest( void )
{
  static const unsigned char header[] = "PSG\x1a";
  char temporary_path[] = "/tmp/fuse-psg-test-XXXXXX";
  unsigned char expected[ 26 ];
  unsigned char read_back[ 26 ];
  FILE *file;
  int fd, saved_recording;
  size_t length;
  int i, r = 0;

  /* mkdtemp is not provided by MinGW; use mkstemp to obtain a unique
     filename prefix instead. */
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );

  saved_recording = psg_recording;
  psg_recording = 0;

  /* Calls while not recording are harmless no-ops, and stopping a recording
     that was never started fails */
  if( psg_frame() || psg_write_register( 7, 0x40 ) ||
      psg_stop_recording() != 1 ) r++;

  /* Starting a recording writes the header and refuses a second start */
  if( psg_start_recording( temporary_path ) || !psg_recording ||
      psg_start_recording( temporary_path ) != 1 ) r++;

  /* Six empty frames are accumulated in the pending empty-frame counter; a
     register write marked afterwards is emitted with the register data, and
     registers 14 and 15 (I/O ports) are never part of the frame data */
  for( i = 0; i < 6; i++ ) if( psg_frame() ) r++;
  if( psg_write_register( 7, 0x40 ) || psg_write_register( 14, 0xff ) ) r++;
  if( psg_frame() ) r++;

  /* Two more empty frames */
  if( psg_frame() || psg_frame() ) r++;

  /* Stopping flushes the pending empty frames */
  if( psg_stop_recording() || psg_recording ) r++;

  /* header (4 bytes + 12 padding) + FE 01 FF FF FF 07 40 + FE 01:
     7 pending empty frames are encoded as a 4-frame block plus three
     single-frame markers before the register data */
  memcpy( expected, header, sizeof( header ) );
  memset( expected + 4, 0, 12 );
  expected[ 16 ] = 0xfe; expected[ 17 ] = 0x01;
  expected[ 18 ] = 0xff; expected[ 19 ] = 0xff; expected[ 20 ] = 0xff;
  expected[ 21 ] = 0x07; expected[ 22 ] = 0x40;
  expected[ 23 ] = 0xfe; expected[ 24 ] = 0x01;
  length = 25;

  file = fopen( temporary_path, "rb" );
  if( !file ) {
    r++;
  } else {
    memset( read_back, 0, sizeof( read_back ) );
    if( fread( read_back, 1, length, file ) != length || fgetc( file ) != EOF ||
        memcmp( read_back, expected, length ) ) r++;
    fclose( file );
  }

  /* A long run of empty frames is encoded with the multi-frame marker and a
     count byte capped at 0xff empty frames */
  if( psg_start_recording( temporary_path ) ) r++;
  for( i = 0; i < 1020; i++ ) if( psg_frame() ) r++;
  if( psg_write_register( 1, 0x20 ) ) r++;
  if( psg_stop_recording() ) r++;

  /* header + FE FF (0xff-frame block) FF 01 20 + FF */
  memcpy( expected, header, sizeof( header ) );
  memset( expected + 4, 0, 12 );
  expected[ 16 ] = 0xfe; expected[ 17 ] = 0xff;
  expected[ 18 ] = 0xff; expected[ 19 ] = 0x01; expected[ 20 ] = 0x20;
  expected[ 21 ] = 0xff;
  length = 22;

  file = fopen( temporary_path, "rb" );
  if( !file ) {
    r++;
  } else {
    memset( read_back, 0, sizeof( read_back ) );
    if( fread( read_back, 1, length, file ) != length || fgetc( file ) != EOF ||
        memcmp( read_back, expected, length ) ) r++;
    fclose( file );
  }

  unlink( temporary_path );
  psg_recording = saved_recording;
  if( r ) printf( "psg_unittest failed\n" );
  return r;
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
assert_16k_rom_page( libspectrum_word base, int page )
{
  return unittests_assert_16k_page( base, memory_source_rom, page );
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
create_rom_fixture( char *filename, size_t length, libspectrum_byte value )
{
  libspectrum_byte buffer[0x4000];
  int fd;

  memset( buffer, value, length );
  fd = mkstemp( filename );
  if( fd < 0 ) return 1;
  if( write( fd, buffer, length ) != length ) {
    close( fd );
    unlink( filename );
    return 1;
  }

  return close( fd );
}

static int
machine_load_rom_bank_with_sizes_test( void )
{
  static const size_t allowed_lengths[] = { 0x2000, 0x4000 };
  char rom8[] = "/tmp/fuse-rom-8k-XXXXXX";
  char rom16[] = "/tmp/fuse-rom-16k-XXXXXX";
  char rom_bad[] = "/tmp/fuse-rom-bad-XXXXXX";
  const char *missing_rom = "/tmp/fuse-rom-does-not-exist";
  memory_page map[MEMORY_PAGES_IN_16K];
  libspectrum_byte snapshot_rom[0x4000];
  size_t loaded_length = 0;
  int r = 0;

  memset( map, 0, sizeof( map ) );
  if( create_rom_fixture( rom8, 0x2000, 0x08 ) ||
      create_rom_fixture( rom16, 0x4000, 0x16 ) ||
      create_rom_fixture( rom_bad, 0x3000, 0x30 ) ) {
    printf( "machine_load_rom_bank_with_sizes_test: failed to create fixtures\n" );
    r++;
    goto cleanup;
  }

  if( machine_load_rom_bank_with_sizes(
        map, 0, rom16, rom8, allowed_lengths,
        ARRAY_SIZE( allowed_lengths ), &loaded_length ) ||
      loaded_length != 0x4000 || map[7].page[0] != 0x16 ||
      !map[0].save_to_snapshot )
    r++;

  /* An unsupported custom ROM should fall back to the default 8K image. */
  if( machine_load_rom_bank_with_sizes(
        map, 0, rom_bad, rom8, allowed_lengths,
        ARRAY_SIZE( allowed_lengths ), &loaded_length ) ||
      loaded_length != 0x2000 || map[0].page[0] != 0x08 ||
      map[0].save_to_snapshot )
    r++;

  /* A missing custom ROM should also fall back to the default. */
  if( machine_load_rom_bank_with_sizes(
        map, 0, missing_rom, rom8, allowed_lengths,
        ARRAY_SIZE( allowed_lengths ), &loaded_length ) ||
      loaded_length != 0x2000 || map[0].page[0] != 0x08 ||
      map[0].save_to_snapshot )
    r++;

  if( !machine_load_rom_bank_with_sizes(
         map, 0, rom_bad, NULL, allowed_lengths,
         ARRAY_SIZE( allowed_lengths ), &loaded_length ) )
    r++;

  /* The result length is optional, but the list of accepted sizes is not. */
  if( machine_load_rom_bank_with_sizes(
        map, 0, rom8, NULL, allowed_lengths,
        ARRAY_SIZE( allowed_lengths ), NULL ) ||
      !machine_load_rom_bank_with_sizes(
         map, 0, rom8, NULL, NULL, 0, &loaded_length ) ||
      loaded_length != 0 )
    r++;

  /* Keep the original fixed-size API on the same common loading path. */
  if( machine_load_rom_bank( map, 0, rom8, NULL, 0x2000 ) ||
      map[0].page[0] != 0x08 ||
      !machine_load_rom_bank( map, 0, rom_bad, NULL, 0x2000 ) )
    r++;

  memset( snapshot_rom, 0x5a, sizeof( snapshot_rom ) );
  if( machine_load_rom_bank_from_snapshot( map, 0, snapshot_rom,
                                           sizeof( snapshot_rom ), 1 ) ||
      machine_load_rom_bank_with_sizes(
        map, 0, rom8, NULL, allowed_lengths,
        ARRAY_SIZE( allowed_lengths ), &loaded_length ) ||
      loaded_length != sizeof( snapshot_rom ) || map[7].page[0] != 0x5a )
    r++;
  machine_clear_snapshot_rom_bank( map, 0 );

  /* A cached snapshot ROM must satisfy the same size constraints as a file. */
  if( machine_load_rom_bank_from_snapshot( map, 0, snapshot_rom, 0x3000, 1 ) ||
      !machine_load_rom_bank_with_sizes(
         map, 0, rom8, NULL, allowed_lengths,
         ARRAY_SIZE( allowed_lengths ), &loaded_length ) ||
      loaded_length != 0 )
    r++;
  machine_clear_snapshot_rom_bank( map, 0 );

cleanup:
  unlink( rom8 );
  unlink( rom16 );
  unlink( rom_bad );
  if( r ) printf( "machine_load_rom_bank_with_sizes_test failed\n" );
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
      file.file_class != LIBSPECTRUM_CLASS_UNKNOWN ) r++;

  compat_file_set_vtable( &utils_file_previous_vtable );
  unlink( filename );
  if( r ) printf( "utils_file_lifecycle_test failed\n" );
  return r;
}

static int
utils_file_harddisk_identify_test( void )
{
  char temporary_path[] = "/tmp/fuse-utils-hdf-XXXXXX";
  char filename[ PATH_MAX ];
  compat_file_vtable_t vtable;
  utils_file file;
  unsigned char header[ 128 ] = "RS-IDE\x1a";
  int fd, r = 0;

  fd = mkstemp( temporary_path );
  snprintf( filename, sizeof( filename ), "%s.hdf", temporary_path );
  if( fd < 0 || write( fd, header, sizeof( header ) ) != sizeof( header ) ||
      close( fd ) || rename( temporary_path, filename ) ) {
    if( fd >= 0 ) close( fd );
    unlink( temporary_path );
    unlink( filename );
    printf( "utils_file_harddisk_identify_test: failed to create fixture\n" );
    return 1;
  }

  compat_file_get_vtable( &utils_file_previous_vtable );
  vtable = utils_file_previous_vtable;
  vtable.open = utils_file_test_open;
  vtable.read = utils_file_test_read;
  utils_file_test_path = filename;
  utils_file_open_count = utils_file_read_count = 0;
  compat_file_set_vtable( &vtable );

  utils_file_init( &file, filename );
  if( utils_file_identify( &file ) ||
      file.file_class != LIBSPECTRUM_CLASS_HARDDISK || file.buffer ||
      utils_file_open_count != 1 || utils_file_read_count != 1 ) r++;

  utils_file_free( &file );
  compat_file_set_vtable( &utils_file_previous_vtable );
  unlink( filename );
  if( r ) printf( "utils_file_harddisk_identify_test failed\n" );
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
  file.file_class = LIBSPECTRUM_CLASS_TAPE;

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

/* Exercise the sparse DCK bank IDs, not just an empty DOCK block (#532). */
static int
dck_sparse_banks_test( void )
{
  static const libspectrum_dck_bank banks[] = {
    LIBSPECTRUM_DCK_BANK_DOCK, LIBSPECTRUM_DCK_BANK_EXROM,
    LIBSPECTRUM_DCK_BANK_HOME
  };
  libspectrum_machine old_machine = machine_current->machine;
  utils_file file;
  int bank, page, offset, ram, r = 0;
  size_t block_length = 9 + 8 * 0x2000;
  size_t position;

  utils_file_init( &file, "synthetic-banks.dck" );
  file.length = 3 * block_length;
  file.buffer = libspectrum_new0( unsigned char, file.length );

  if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ) {
    r++;
    goto cleanup;
  }

  for( ram = 0; ram < 2; ram++ ) {
    position = 0;
    for( bank = 0; bank < 3; bank++ ) {
      unsigned char *block = file.buffer + position;
      int pages = !ram && bank == 2 ? 2 : 8;

      memset( block, 0, 9 );
      block[ 0 ] = banks[ bank ];
      for( page = 0; page < pages; page++ ) {
        block[ 1 + page ] = ram ? LIBSPECTRUM_DCK_PAGE_RAM :
                                 LIBSPECTRUM_DCK_PAGE_ROM;
        memset( block + 9 + page * 0x2000,
                0x40 + bank * 8 + page, 0x2000 );
      }
      position += 9 + pages * 0x2000;
    }
    file.length = position;

    if( dck_insert_loaded( &file ) || !dck_active ) {
      printf( "dck_sparse_banks_test: %s cartridge rejected\n",
              ram ? "RAM" : "ROM" );
      r++;
      break;
    }

    for( bank = 0; bank < 3; bank++ ) {
      writeport_internal( 0x00ff, bank == 1 ? 0x80 : 0 );
      writeport_internal( 0x00f4, bank == 2 ? 0 : 0xff );
      /* HOME ROM replaces only the two ROM slots, as in Emulator.dck. */
      for( page = 0; page < ( !ram && bank == 2 ? 2 : 8 ); page++ ) {
        libspectrum_byte expected = 0x40 + bank * 8 + page;
        for( offset = 0; offset < 0x2000; offset++ ) {
          if( readbyte_internal( page * 0x2000 + offset ) != expected ) {
            printf( "dck_sparse_banks_test: bank %d page %d contents\n",
                    banks[ bank ], page );
            r++;
            break;
          }
        }
        writebyte_internal( page * 0x2000, expected ^ 0xff );
        if( readbyte_internal( page * 0x2000 ) !=
            ( ram ? ( expected ^ 0xff ) : expected ) ) r++;
      }
    }
    dck_eject();
  }

cleanup:
  if( dck_active ) dck_eject();
  if( machine_select( old_machine ) ) r++;
  utils_file_free( &file );
  if( r ) printf( "dck_sparse_banks_test failed\n" );
  return r;
}

static void
dck_test_map( libspectrum_dck_bank bank )
{
  writeport_internal( 0x00ff, bank == LIBSPECTRUM_DCK_BANK_EXROM ? 0x80 : 0 );
  writeport_internal( 0x00f4, bank == LIBSPECTRUM_DCK_BANK_HOME ? 0 : 0xff );
}

static int
dck_independent_pages_test( void )
{
  static const libspectrum_dck_bank banks[] = {
    LIBSPECTRUM_DCK_BANK_DOCK, LIBSPECTRUM_DCK_BANK_EXROM,
    LIBSPECTRUM_DCK_BANK_HOME
  };
  static const libspectrum_dck_page_type types[] = {
    LIBSPECTRUM_DCK_PAGE_ROM, LIBSPECTRUM_DCK_PAGE_RAM,
    LIBSPECTRUM_DCK_PAGE_RAM_EMPTY, LIBSPECTRUM_DCK_PAGE_NULL,
    LIBSPECTRUM_DCK_PAGE_RAM, LIBSPECTRUM_DCK_PAGE_RAM_EMPTY,
    LIBSPECTRUM_DCK_PAGE_NULL, LIBSPECTRUM_DCK_PAGE_RAM
  };
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_byte *defaults = libspectrum_new( libspectrum_byte, 0x10000 );
  utils_file file;
  int bank, page, offset, r = 0;
  size_t position;

  utils_file_init( &file, "synthetic-page-types.dck" );
  file.buffer = libspectrum_new0( unsigned char, 9 + 8 * 0x2000 );
  for( bank = 0; bank < 3; bank++ ) {
    if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ) { r++; break; }
    dck_test_map( banks[ bank ] );
    for( offset = 0; offset < 0x10000; offset++ )
      defaults[ offset ] = readbyte_internal( offset );

    memset( file.buffer, 0, 9 );
    file.buffer[ 0 ] = banks[ bank ];
    position = 9;
    for( page = 0; page < 8; page++ ) {
      file.buffer[ 1 + page ] = types[ page ];
      if( types[ page ] == LIBSPECTRUM_DCK_PAGE_ROM ||
          types[ page ] == LIBSPECTRUM_DCK_PAGE_RAM ) {
        memset( file.buffer + position, 0x60 + page, 0x2000 );
        position += 0x2000;
      }
    }
    file.length = position;
    if( dck_insert_loaded( &file ) || !dck_active ) {
      printf( "dck_independent_pages_test: bank %d rejected\n", banks[ bank ] );
      r++;
    } else {
      dck_test_map( banks[ bank ] );
      for( page = 0; page < 8; page++ ) {
        for( offset = 0; offset < 0x2000; offset++ ) {
          int address = page * 0x2000 + offset;
          libspectrum_byte expected = types[ page ] == LIBSPECTRUM_DCK_PAGE_NULL ?
            defaults[ address ] : types[ page ] == LIBSPECTRUM_DCK_PAGE_RAM_EMPTY ?
            0 : 0x60 + page;
          if( readbyte_internal( address ) != expected ) {
            printf( "dck_independent_pages_test: bank %d page %d contents\n",
                    banks[ bank ], page );
            r++;
            break;
          }
        }
        if( types[ page ] != LIBSPECTRUM_DCK_PAGE_NULL ) {
          int writable = types[ page ] != LIBSPECTRUM_DCK_PAGE_ROM;
          libspectrum_byte before = readbyte_internal( page * 0x2000 );
          writebyte_internal( page * 0x2000, 0xa5 );
          if( readbyte_internal( page * 0x2000 ) !=
              ( writable ? 0xa5 : before ) ) r++;
        }
      }
    }
    dck_eject();
  }
  if( machine_select( old_machine ) ) r++;
  utils_file_free( &file );
  libspectrum_free( defaults );
  if( r ) printf( "dck_independent_pages_test failed\n" );
  return r;
}

static int
dck_lifecycle_test( void )
{
  static const libspectrum_dck_bank banks[] = {
    LIBSPECTRUM_DCK_BANK_DOCK, LIBSPECTRUM_DCK_BANK_EXROM,
    LIBSPECTRUM_DCK_BANK_HOME
  };
  libspectrum_machine old_machine = machine_current->machine;
  utils_file file;
  int bank, reset, fd, r = 0;
  char filename[] = "/tmp/fuse-dck-lifecycle-XXXXXX";

  fd = mkstemp( filename );
  if( fd < 0 ) return 1;
  utils_file_init( &file, filename );
  file.length = 9 + 2 * 0x2000;
  file.buffer = libspectrum_new0( unsigned char, file.length );
  file.buffer[ 1 ] = LIBSPECTRUM_DCK_PAGE_RAM;
  file.buffer[ 2 ] = LIBSPECTRUM_DCK_PAGE_ROM;
  memset( file.buffer + 9, 0x35, 0x2000 );
  memset( file.buffer + 9 + 0x2000, 0x79, 0x2000 );

  for( bank = 0; bank < 3; bank++ ) {
    file.buffer[ 0 ] = banks[ bank ];
    if( lseek( fd, 0, SEEK_SET ) < 0 ||
        write( fd, file.buffer, file.length ) != file.length ) { r++; break; }
    if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ||
        dck_insert( filename ) || !dck_active ) { r++; break; }
    dck_test_map( banks[ bank ] );
    if( readbyte_internal( 0 ) != 0x35 ||
        readbyte_internal( 0x2000 ) != 0x79 ) r++;

    for( reset = 0; reset < 2; reset++ ) {
      writebyte_internal( 0, 0xa5 );
      if( machine_reset( reset ) ) r++;
      dck_test_map( banks[ bank ] );
      if( !dck_active || readbyte_internal( 0 ) != ( reset ? 0 : 0xa5 ) ||
          readbyte_internal( 0x2000 ) != 0x79 ) {
        printf( "dck_lifecycle_test: bank %d %s reset\n",
                banks[ bank ], reset ? "hard" : "soft" );
        r++;
      }
    }

    /* Replace it with an empty block; old cartridge pages must disappear. */
    file.length = 9;
    memset( file.buffer + 1, 0, 8 );
    if( dck_insert_loaded( &file ) || !dck_active ) r++;
    dck_test_map( banks[ bank ] );
    if( readbyte_internal( 0 ) == 0x35 ||
        readbyte_internal( 0x2000 ) == 0x79 ) r++;
    dck_eject();
    if( dck_active || settings_current.dck_file ) r++;
    writebyte_internal( 0x4000, 0x96 );
    if( readbyte_internal( 0x4000 ) != 0x96 ) r++;
    file.length = 9 + 2 * 0x2000;
    file.buffer[ 1 ] = LIBSPECTRUM_DCK_PAGE_RAM;
    file.buffer[ 2 ] = LIBSPECTRUM_DCK_PAGE_ROM;
  }
  if( dck_active ) dck_eject();
  if( close( fd ) ) r++;
  unlink( filename );
  utils_file_free( &file );
  if( machine_select( old_machine ) ) r++;
  if( r ) printf( "dck_lifecycle_test failed\n" );
  return r;
}

/* The archives contain cartridge.dck: one DOCK ROM page filled with 0x79.
   Keep fixtures inline so these tests need no external compression tools. */
static int
dck_archive_test( void )
{
#ifdef HAVE_ZLIB_H
  static const unsigned char zip[] = {
    0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0xea, 0x8a,
    0x49, 0x5d, 0x0c, 0x27, 0xb8, 0x2b, 0x1d, 0x00, 0x00, 0x00, 0x09, 0x20,
    0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x63, 0x61, 0x72, 0x74, 0x72, 0x69,
    0x64, 0x67, 0x65, 0x2e, 0x64, 0x63, 0x6b, 0xed, 0xc1, 0xb1, 0x09, 0x00,
    0x00, 0x08, 0x03, 0xb0, 0xe2, 0xd7, 0x7e, 0xdf, 0xc1, 0x2b, 0x84, 0x24,
    0x99, 0x9c, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xde, 0x2b,
    0x50, 0x4b, 0x01, 0x02, 0x14, 0x03, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00,
    0xea, 0x8a, 0x49, 0x5d, 0x0c, 0x27, 0xb8, 0x2b, 0x1d, 0x00, 0x00, 0x00,
    0x09, 0x20, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x63, 0x61,
    0x72, 0x74, 0x72, 0x69, 0x64, 0x67, 0x65, 0x2e, 0x64, 0x63, 0x6b, 0x50,
    0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x3b,
    0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  static const unsigned char gzip[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xed, 0xc1,
    0xb1, 0x09, 0x00, 0x00, 0x08, 0x03, 0xb0, 0xe2, 0xd7, 0x7e, 0xdf, 0xc1,
    0x2b, 0x84, 0x24, 0x99, 0x9c, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xde, 0x2b, 0x0c, 0x27, 0xb8, 0x2b, 0x09, 0x20, 0x00, 0x00,
  };
#endif
  unsigned char plain[ 9 + 0x2000 ];
  const unsigned char *data[] = { plain,
#ifdef HAVE_ZLIB_H
    zip, gzip,
#endif
  };
  const size_t lengths[] = { sizeof( plain ),
#ifdef HAVE_ZLIB_H
    sizeof( zip ), sizeof( gzip ),
#endif
  };
  const char *suffixes[] = { ".dck",
#ifdef HAVE_ZLIB_H
    ".zip", ".dck.gz",
#endif
  };
  libspectrum_machine old_machine = machine_current->machine;
  char temporary_path[ PATH_MAX ];
  char filename[ PATH_MAX ];
  utils_file file;
  size_t format;
  int loaded, reset, page, fd, r = 0;

  memset( plain, 0, 9 );
  plain[ 1 ] = LIBSPECTRUM_DCK_PAGE_ROM;
  memset( plain + 9, 0x79, 0x2000 );
  snprintf( temporary_path, sizeof( temporary_path ),
            "%s%sfuse-dck-archive-XXXXXX", compat_get_temp_path(),
            FUSE_DIR_SEP_STR );
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );
  utils_file_init( &file, NULL );
  if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ) { r++; goto cleanup; }
  for( format = 0; format < ARRAY_SIZE( data ); format++ ) {
    snprintf( filename, sizeof( filename ), "%s%s",
              temporary_path, suffixes[ format ] );
    fd = open( filename, O_WRONLY | O_CREAT | O_TRUNC, 0600 );
    if( fd < 0 ) { r++; break; }
    if( write( fd, data[ format ], lengths[ format ] ) != lengths[ format ] )
      r++;
    if( close( fd ) ) r++;
    for( loaded = 0; loaded < 2; loaded++ ) {
      /* Menu insertion reads the path; generic loading supplies resolved data. */
      if( loaded ) {
        if( utils_read_file( filename, &file ) ) { r++; continue; }
        if( dck_insert_loaded( &file ) ) r++;
      } else if( dck_insert( filename ) ) r++;
      /* Neither reset may need the source archive after insertion. */
      if( loaded ) unlink( filename );
      for( reset = -1; reset < 2; reset++ ) {
        if( reset >= 0 && machine_reset( reset ) ) r++;
        dck_test_map( LIBSPECTRUM_DCK_BANK_DOCK );
        if( !dck_active || !settings_current.dck_file ||
            strcmp( settings_current.dck_file,
                    loaded ? file.filename : filename ) ) r++;
        for( page = 0; page < 0x2000; page++ )
          if( readbyte_internal( page ) != 0x79 ) { r++; break; }
        writebyte_internal( 0, 0xa5 );
        if( readbyte_internal( 0 ) != 0x79 ) r++;
      }
      dck_eject();
      utils_file_free( &file );
    }
    unlink( filename );
  }
cleanup:
  utils_file_free( &file );
  if( dck_active ) dck_eject();
  if( machine_select( old_machine ) ) r++;
  if( r ) printf( "dck_archive_test failed: %d errors\n", r );
  return r;
}

static int
dck_snapshot_reset_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_snap *snap = libspectrum_snap_alloc();
  libspectrum_snap *saved = libspectrum_snap_alloc();
  utils_file file;
  libspectrum_byte *ram, *rom;
  int bank, reset, address, r = 0;

  utils_file_init( &file, "buffer-only.dck" );
  file.length = 9 + 0x2000;
  file.buffer = libspectrum_new0( unsigned char, file.length );
  file.buffer[ 3 ] = LIBSPECTRUM_DCK_PAGE_ROM;
  memset( file.buffer + 9, 0x35, 0x2000 );
  if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ||
      dck_insert_loaded( &file ) ) { r++; goto cleanup; }
  libspectrum_snap_set_machine( snap, LIBSPECTRUM_MACHINE_TC2068 );
  libspectrum_snap_set_dock_active( snap, 1 );
  for( bank = 0; bank < 2; bank++ ) {
    ram = libspectrum_new( libspectrum_byte, 0x2000 );
    rom = libspectrum_new( libspectrum_byte, 0x2000 );
    memset( ram, 0x96, 0x2000 );
    memset( rom, 0x79, 0x2000 );
    if( bank ) {
      libspectrum_snap_set_exrom_cart( snap, 0, ram );
      libspectrum_snap_set_exrom_ram( snap, 0, 1 );
      libspectrum_snap_set_exrom_cart( snap, 1, rom );
      libspectrum_snap_set_exrom_ram( snap, 1, 0 );
    } else {
      libspectrum_snap_set_dock_cart( snap, 0, ram );
      libspectrum_snap_set_dock_ram( snap, 0, 1 );
      libspectrum_snap_set_dock_cart( snap, 1, rom );
      libspectrum_snap_set_dock_ram( snap, 1, 0 );
    }
  }
  if( snapshot_copy_from( snap ) ) { r++; goto cleanup; }
  /* The snapshot replaces the old cartridge and needs no backing path. */
  if( !dck_active || settings_current.dck_file ) r++;
  if( snapshot_copy_to( saved ) ||
      libspectrum_snap_dock_cart( saved, 2 ) ) r++;
  for( reset = -1; reset < 2; reset++ ) {
    if( reset >= 0 && machine_reset( reset ) ) r++;
    for( bank = 0; bank < 2; bank++ ) {
      dck_test_map( bank ? LIBSPECTRUM_DCK_BANK_EXROM :
                          LIBSPECTRUM_DCK_BANK_DOCK );
      for( address = 0; address < 0x2000; address++ ) {
        libspectrum_byte expected = reset == 1 ? 0 :
          ( reset == 0 && address == 0 ? 0xa5 : 0x96 );
        if( readbyte_internal( address ) != expected ||
            readbyte_internal( 0x2000 + address ) != 0x79 ) {
          r++; break;
        }
      }
      writebyte_internal( 0, 0xa5 );
      writebyte_internal( 0x2000, 0xa5 );
      if( readbyte_internal( 0 ) != 0xa5 ||
          readbyte_internal( 0x2000 ) != 0x79 ) r++;
    }
    if( !dck_active ) r++;
  }
  /* A cartridge-free snapshot must also replace the restored cartridge. */
  libspectrum_snap_set_dock_active( snap, 0 );
  if( snapshot_copy_from( snap ) || dck_active ) r++;
cleanup:
  dck_eject();
  utils_file_free( &file );
  libspectrum_snap_free( snap );
  libspectrum_snap_free( saved );
  if( machine_select( old_machine ) ) r++;
  if( r ) printf( "dck_snapshot_reset_test failed: %d errors\n", r );
  return r;
}

static int
dck_replacement_power_cycle_test( void )
{
  static const libspectrum_dck_bank banks[] = {
    LIBSPECTRUM_DCK_BANK_DOCK, LIBSPECTRUM_DCK_BANK_EXROM,
    LIBSPECTRUM_DCK_BANK_HOME
  };
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_snap *snap = libspectrum_snap_alloc();
  utils_file old, replacement;
  int bank, from_snapshot, r = 0;

  utils_file_init( &old, "old.dck" );
  old.length = 9 + 0x2000;
  old.buffer = libspectrum_new0( unsigned char, old.length );
  old.buffer[ 3 ] = LIBSPECTRUM_DCK_PAGE_ROM;
  memset( old.buffer + 9, 0x35, 0x2000 );
  utils_file_init( &replacement, "replacement.dck" );
  replacement.length = 9 + 2 * 0x2000;
  replacement.buffer = libspectrum_new0( unsigned char, replacement.length );
  replacement.buffer[ 1 ] = LIBSPECTRUM_DCK_PAGE_RAM;
  replacement.buffer[ 2 ] = LIBSPECTRUM_DCK_PAGE_ROM;
  memset( replacement.buffer + 9, 0x96, 0x2000 );
  memset( replacement.buffer + 9 + 0x2000, 0x79, 0x2000 );
  if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ) { r++; goto cleanup; }
  for( bank = 0; bank < ARRAY_SIZE( banks ); bank++ ) {
    for( from_snapshot = 0; from_snapshot < ( bank == 2 ? 1 : 2 );
         from_snapshot++ ) {
      old.buffer[ 0 ] = banks[ bank ];
      if( dck_insert_loaded( &old ) || !dck_active ) r++;
      if( from_snapshot ) {
        libspectrum_snap_free( snap );
        snap = libspectrum_snap_alloc();
        if( snapshot_copy_to( snap ) || snapshot_copy_from( snap ) ) r++;
      }
      /* Replace even HOME ROM overlays with a different bank: absent old
         pages must revert to their normal machine mappings. */
      replacement.buffer[ 0 ] = banks[ ( bank + 1 ) % 3 ];
      BC = 0x1234;
      if( dck_insert_loaded( &replacement ) || !dck_active || BC ) r++;
      dck_test_map( banks[ bank ] );
      if( readbyte_internal( 0x4000 ) == 0x35 ) r++;
      dck_test_map( banks[ ( bank + 1 ) % 3 ] );
      if( readbyte_internal( 0 ) != 0x96 ||
          readbyte_internal( 0x2000 ) != 0x79 ) r++;
      BC = 0x5678;
      dck_eject();
      if( BC || dck_active || settings_current.dck_file ) r++;
      dck_test_map( banks[ ( bank + 1 ) % 3 ] );
      if( readbyte_internal( 0 ) == 0x96 ||
          readbyte_internal( 0x2000 ) == 0x79 ) r++;
    }
  }
cleanup:
  if( dck_active ) dck_eject();
  utils_file_free( &old );
  utils_file_free( &replacement );
  libspectrum_snap_free( snap );
  if( machine_select( old_machine ) ) r++;
  if( r ) printf( "dck_replacement_power_cycle_test failed: %d errors\n", r );
  return r;
}

static int
dck_malformed_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  utils_file file;
  int test, r = 0;

  utils_file_init( &file, "malformed.dck" );
  file.buffer = libspectrum_new0( unsigned char, 9 + 0x2000 );
  if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ) { r++; goto cleanup; }
  for( test = 0; test < 4; test++ ) {
    /* A failed replacement must eject the previous valid cartridge. */
    memset( file.buffer, 0, 9 );
    file.length = 9;
    if( dck_insert_loaded( &file ) || !dck_active ) r++;
    memset( file.buffer, 0, 9 + 0x2000 );
    file.length = 9;
    switch( test ) {
    case 0: file.length = 8; break; /* Truncated header. */
    case 1: file.buffer[ 0 ] = 42; break; /* Unsupported bank. */
    case 2: file.buffer[ 1 ] = 42; break; /* Invalid page type. */
    case 3:
      file.buffer[ 1 ] = LIBSPECTRUM_DCK_PAGE_ROM;
      file.length = 9 + 0x2000 - 1; /* Truncated page. */
      break;
    }
    dck_insert_loaded( &file );
    if( dck_active || settings_current.dck_file ) {
      printf( "dck_malformed_test: case %d accepted\n", test );
      r++;
      dck_eject();
    }
    writebyte_internal( 0x4000, 0x96 );
    if( readbyte_internal( 0x4000 ) != 0x96 ) r++;
  }
cleanup:
  utils_file_free( &file );
  if( machine_select( old_machine ) ) r++;
  if( r ) printf( "dck_malformed_test failed\n" );
  return r;
}

static int
dck_home_rom_ram_slots_test( void )
{
  libspectrum_machine old_machine = machine_current->machine;
  memory_page *saved = libspectrum_new( memory_page,
                                       SPECTRUM_RAM_PAGES * MEMORY_PAGES_IN_16K );
  utils_file file;
  int page, r = 0;

  memcpy( saved, memory_map_ram, sizeof( memory_map_ram ) );
  utils_file_init( &file, "home-rom-in-ram-slots.dck" );
  file.length = 9 + 6 * 0x2000;
  file.buffer = libspectrum_new0( unsigned char, file.length );
  file.buffer[ 0 ] = LIBSPECTRUM_DCK_BANK_HOME;
  for( page = 2; page < 8; page++ ) {
    file.buffer[ 1 + page ] = LIBSPECTRUM_DCK_PAGE_ROM;
    memset( file.buffer + 9 + ( page - 2 ) * 0x2000, 0x40 + page, 0x2000 );
  }
  if( machine_select( LIBSPECTRUM_MACHINE_TC2068 ) ||
      dck_insert_loaded( &file ) || !dck_active ) r++;
  for( page = 2; page < 8; page++ ) {
    if( readbyte_internal( page * 0x2000 ) != 0x40 + page ) r++;
    writebyte_internal( page * 0x2000, 0xa5 );
    if( readbyte_internal( page * 0x2000 ) != 0x40 + page ) r++;
  }
  if( machine_reset( 0 ) ) r++;
  for( page = 2; page < 8; page++ ) {
    if( readbyte_internal( page * 0x2000 ) != 0x40 + page ) r++;
    writebyte_internal( page * 0x2000, 0xa5 );
    if( readbyte_internal( page * 0x2000 ) != 0x40 + page ) r++;
  }
  dck_eject();
  for( page = 2; page < 8; page++ ) {
    writebyte_internal( page * 0x2000, 0xa5 );
    if( readbyte_internal( page * 0x2000 ) != 0xa5 ) {
      printf( "dck_home_rom_ram_slots_test: page %d read-only after eject\n",
              page );
      r++;
    }
  }
  if( machine_reset( 1 ) ) r++;
  /* Check descriptors before reading: stale cartridge storage may be freed. */
  for( page = 0; page < SPECTRUM_RAM_PAGES * MEMORY_PAGES_IN_16K; page++ ) {
    if( memory_map_ram[ page ].page != saved[ page ].page ||
        memory_map_ram[ page ].writable != saved[ page ].writable ) {
      printf( "dck_home_rom_ram_slots_test: RAM mapping not restored by hard reset\n" );
      r++;
      break;
    }
  }
  /* Keep test isolation even if cartridge descriptor handling regresses. */
  memcpy( memory_map_ram, saved, sizeof( memory_map_ram ) );
  libspectrum_free( saved );
  utils_file_free( &file );
  if( machine_select( old_machine ) ) r++;
  if( r ) printf( "dck_home_rom_ram_slots_test failed\n" );
  return r;
}

static int
utils_open_loaded_microdrive_test( void )
{
  compat_file_vtable_t vtable;
  utils_file file;
  const char *srcdir;
  char *filename;
  int r = 0;

  srcdir = getenv( "FUSE_TEST_SRCDIR" );
  if( !srcdir ) srcdir = ".";
  filename = libspectrum_new( char,
                              strlen( srcdir ) +
                                sizeof( "/lib/tests/success.mdr" ) );
  sprintf( filename, "%s/lib/tests/success.mdr", srcdir );

  if( utils_read_file( filename, &file ) ) {
    printf( "utils_open_loaded_microdrive_test: failed to read fixture\n" );
    libspectrum_free( filename );
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
  libspectrum_free( filename );
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
  file.type = LIBSPECTRUM_ID_DISK_IMG;
  file.file_class = LIBSPECTRUM_CLASS_DISK_PLUSD;
  if( disk_open_loaded( &disk, &file, 0, 0 ) != DISK_OK ||
      utils_file_open_count || utils_file_read_count ) r++;

  disk_close( &disk );
  utils_file_free( &file );
  compat_file_set_vtable( &utils_file_previous_vtable );
  if( r ) printf( "utils_open_loaded_disk_test failed\n" );
  return r;
}

static int
utils_open_loaded_compressed_disk_test( void )
{
#ifdef HAVE_ZLIB_H
  static const unsigned char compressed[] = {
    31, 139, 8, 0, 0, 0, 0, 0, 2, 255, 237, 193, 1, 13, 0, 0,
    0, 194, 160, 247, 79, 109, 15, 7, 20, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 79,
    6, 63, 79, 55, 213, 0, 32, 3, 0,
  };
  utils_file file;
  disk_t disk;
  int r = 0;

  memset( &disk, 0, sizeof( disk ) );
  utils_file_init( &file, "compressed.img.gz" );
  file.buffer = libspectrum_new( unsigned char, sizeof( compressed ) );
  memcpy( file.buffer, compressed, sizeof( compressed ) );
  file.length = sizeof( compressed );
  if( utils_file_identify( &file ) ||
      file.type != LIBSPECTRUM_ID_DISK_IMG ||
      disk_open_loaded( &disk, &file, 0, 0 ) != DISK_OK ) r++;

  disk_close( &disk );
  utils_file_free( &file );
  if( r ) printf( "utils_open_loaded_compressed_disk_test failed\n" );
  return r;
#else
  return 0;
#endif
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
  file.type = LIBSPECTRUM_ID_DISK_IMG;
  file.file_class = LIBSPECTRUM_CLASS_DISK_PLUSD;
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

/* A brand new blank disk image cannot be saved as a classic CPCEMU .dsk
   image: the format cannot store unformatted tracks, and the image Fuse
   used to write could not be reopened (bug #415) */
static int
disk_write_blank_disk_cpc_save_test( void )
{
  char temporary_path[] = "/tmp/fuse-disk-blank-cpc-XXXXXX";
  char filename[ PATH_MAX ];
  disk_t disk;
  int fd, r = 0;

  /* mkdtemp is not provided by MinGW; use mkstemp to obtain a unique
     filename prefix instead. */
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );
  snprintf( filename, sizeof( filename ), "%s.dsk", temporary_path );

  /* A new blank disk image, as created by the media menu's "Insert New"
     item: every track is unformatted */
  memset( &disk, 0, sizeof( disk ) );
  if( disk_new( &disk, 2, 80, DISK_DENS_AUTO, DISK_UDI ) != DISK_OK ) r++;

  /* Saving as a classic CPCEMU .dsk image must be refused */
  disk.type = DISK_TYPE_NONE;
  if( disk_write( &disk, filename ) != DISK_GEOM ) r++;

  disk_close( &disk );
  unlink( filename );

  if( r ) printf( "disk_write_blank_disk_cpc_save_test failed\n" );
  return r;
}

/* Write a blank UDI disk image and reopen it: the UDI writer and reader
   previously had no direct test coverage */
static int
disk_write_open_udi_roundtrip_test( void )
{
  char temporary_path[] = "/tmp/fuse-disk-roundtrip-XXXXXX";
  char filename[ PATH_MAX ];
  disk_t write_disk, open_disk;
  int fd, i, r = 0;
  const int sides = 2, cylinders = 40;

  /* mkdtemp is not provided by MinGW; use mkstemp to obtain a unique
     filename prefix instead. */
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );
  snprintf( filename, sizeof( filename ), "%s.udi", temporary_path );

  memset( &write_disk, 0, sizeof( write_disk ) );
  if( disk_new( &write_disk, sides, cylinders, DISK_DENS_AUTO,
                DISK_UDI ) != DISK_OK )
    r++;

  if( disk_write( &write_disk, filename ) != DISK_OK ) r++;

  memset( &open_disk, 0, sizeof( open_disk ) );
  if( !r && disk_open( &open_disk, filename, 0, 0 ) != DISK_OK ) r++;

  if( !r && ( open_disk.sides != sides || open_disk.cylinders != cylinders ||
              open_disk.type != DISK_UDI || open_disk.status != DISK_OK ) )
    r++;

  /* Every track must come back with the same type, length and contents */
  if( !r ) {
    disk_t *w = &write_disk, *o = &open_disk;
    for( i = 0; i < sides * cylinders; i++ ) {
      DISK_SET_TRACK_IDX( w, i );
      DISK_SET_TRACK_IDX( o, i );
      if( w->track[-1] != o->track[-1] || w->c_bpt != o->c_bpt ||
          memcmp( w->track, o->track,
                  o->c_bpt + 3 * DISK_CLEN( o->c_bpt ) ) ) {
        r++;
        break;
      }
    }
  }

  disk_close( &write_disk );
  disk_close( &open_disk );
  unlink( filename );

  if( r ) printf( "disk_write_open_udi_roundtrip_test failed\n" );
  return r;
}

/* A UDI image whose stored CRC or length field does not match its contents
   must be refused: open_udi() validates both before decoding any tracks */
static int
disk_open_malformed_udi_rejected_test( void )
{
  char temporary_path[] = "/tmp/fuse-disk-malformed-XXXXXX";
  char filename[ PATH_MAX ];
  disk_t write_disk, open_disk;
  utils_file file;
  libspectrum_dword eof = 0;
  int fd, r = 0;

  /* mkdtemp is not provided by MinGW; use mkstemp to obtain a unique
     filename prefix instead. */
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );
  snprintf( filename, sizeof( filename ), "%s.udi", temporary_path );

  memset( &write_disk, 0, sizeof( write_disk ) );
  if( disk_new( &write_disk, 2, 40, DISK_DENS_AUTO, DISK_UDI ) != DISK_OK )
    return 1;
  if( disk_write( &write_disk, filename ) != DISK_OK ) r++;
  disk_close( &write_disk );

  utils_file_init( &file, filename );
  if( !r && utils_file_read( &file ) ) r++;
  if( !r && file.length < 8 ) r++;

  if( !r ) {
    /* The stored length is the file length minus the four CRC bytes */
    eof = (libspectrum_dword) file.buffer[4] |
          (libspectrum_dword) file.buffer[5] << 8 |
          (libspectrum_dword) file.buffer[6] << 16 |
          (libspectrum_dword) file.buffer[7] << 24;
    if( eof != file.length - 4 ) r++;
  }

  /* A corrupted CRC must be refused */
  if( !r ) {
    file.buffer[eof] ^= 0xff;
    memset( &open_disk, 0, sizeof( open_disk ) );
    if( disk_open_loaded( &open_disk, &file, 0, 0 ) != DISK_OPEN ) r++;
    disk_close( &open_disk );
    file.buffer[eof] ^= 0xff;
  }

  /* A mismatched length field must be refused */
  if( !r ) {
    file.buffer[4] ^= 0x01;
    memset( &open_disk, 0, sizeof( open_disk ) );
    if( disk_open_loaded( &open_disk, &file, 0, 0 ) != DISK_OPEN ) r++;
    disk_close( &open_disk );
    file.buffer[4] ^= 0x01;
  }

  utils_file_free( &file );
  unlink( filename );

  if( r ) printf( "disk_open_malformed_udi_rejected_test failed\n" );
  return r;
}

static int
disk_open_fdi_bounds_test( void )
{
  libspectrum_byte image[512];
  utils_file file;
  disk_t disk;
  int i, r = 0;

  /* One track, one 128-byte sector, with enough room for the header. */
  for( i = 0; i < 9; i++ ) {
    memset( image, 0, sizeof( image ) );
    memcpy( image, "FDI", 3 );
    image[4] = image[6] = 1;
    image[10] = 0x00; image[11] = 1; /* data at 256 */
    image[20] = 1;                 /* one sector */
    image[25] = 1;                 /* sector data present */
    if( i == 1 ) image[15] = 2;    /* track beyond EOF */
    if( i == 2 ) image[27] = 1;    /* sector beyond EOF */
    if( i == 3 ) image[24] = 8;    /* invalid size code */
    if( i == 4 ) image[14] = image[15] = image[16] = image[17] = 255;
    if( i == 5 ) image[26] = 200;  /* sector crosses EOF */
    utils_file_init( &file, "bounds.fdi" );
    file.buffer = image;
    file.length = i == 6 ? 13 : i == 7 ? 20 : sizeof( image );
    if( i == 8 ) image[12] = image[13] = 255; /* header outside file */
    file.type = LIBSPECTRUM_ID_DISK_FDI;
    memset( &disk, 0, sizeof( disk ) );
    if( disk_open_loaded( &disk, &file, 0, 0 ) !=
        ( i < 6 && i != 3 ? DISK_OK : DISK_OPEN ) ) r++;
    disk_close( &disk );
  }
  if( r ) printf( "disk_open_fdi_bounds_test failed\n" );
  return r;
}

static int
disk_open_fdi_recovery_test( void )
{
  libspectrum_byte image[841];
  utils_file file;
  disk_t disk;
  int t, s, j, k, h, p, offset, ids, marks, r = 0;

  /* Two tracks, three sectors each, and three extra header bytes.
     Both tracks have a bad middle sector followed by readable data. */
  memset( image, 0, sizeof( image ) );
  memcpy( image, "FDI", 3 );
  image[4] = 2; image[6] = 1;
  image[10] = 73; image[12] = 3;
  for( t = 0; t < 2; t++ ) {
    h = 17 + t * 28;
    image[h] = ( t * 384 ) & 255;
    image[h + 1] = ( t * 384 ) >> 8;
    image[h + 6] = 3;
    for( s = 0; s < 3; s++ ) {
      p = h + 7 + s * 7;
      image[p] = t;
      image[p + 2] = s + 1;
      image[p + 4] = s == 2 ? 0x81 : 1;
      offset = s * 128;
      if( s == 1 ) offset = t ? 320 : 65535;
      image[p + 5] = offset & 255;
      image[p + 6] = offset >> 8;
      memset( image + 73 + t * 384 + s * 128, 0x30 + t * 3 + s, 128 );
    }
  }
  utils_file_init( &file, "recovery.fdi" );
  file.buffer = image;
  file.length = sizeof( image );
  file.type = LIBSPECTRUM_ID_DISK_FDI;
  memset( &disk, 0, sizeof( disk ) );
  if( disk_open_loaded( &disk, &file, 0, 0 ) != DISK_OK ) r++;
  else for( t = 0; t < 2; t++ ) {
    DISK_SET_TRACK_IDX( ( &disk ), t );
    ids = marks = 0;
    for( j = 0; j < disk.c_bpt; j++ ) {
      if( !bitmap_test( disk.clocks, j ) ) continue; /* FM marks */
      if( disk.track[j] == 0xfe ) ids++;
      if( disk.track[j] == 0xfb || disk.track[j] == 0xf8 ) {
        s = marks ? 2 : 0;
        if( disk.track[j] != ( s == 2 ? 0xf8 : 0xfb ) ) r++;
        for( k = 1; k <= 128; k++ )
          if( disk.track[j + k] != 0x30 + t * 3 + s ) { r++; break; }
        marks++;
      }
    }
    if( ids != 3 || marks != 2 ) r++;
  }
  disk_close( &disk );
  if( r ) printf( "disk_open_fdi_recovery_test failed\n" );
  return r;
}

static int
disk_open_fdi_semantics_test( void )
{
  static const int flags[] = { 1, 0, 2, 0x81, 0x80, 0x40, 0xc0,
                               0x40, 0x20, 0, 0x40 };
  libspectrum_byte image[8192];
  disk_t disk;
  utils_file file;
  libspectrum_word crc;
  int test, j, k, n, size, data_offset, length, ids, marks, pos;
  int id_pos, data_pos, start, missing, mfm, r = 0;

  for( test = 0; test < (int)( sizeof( flags ) / sizeof( flags[0] ) );
       test++ ) {
    n = test == 7 ? 36 : 1; /* cross the old 35-header chunk boundary */
    size = test == 8 || test == 9 ? 5 : 0;
    length = 128 << size;
    missing = flags[test] & 0x40;
    data_offset = 14 + 7 + n * 7;
    memset( image, 0, sizeof( image ) );
    memcpy( image, "FDI", 3 );
    image[4] = image[6] = 1;
    image[10] = data_offset & 0xff;
    image[11] = data_offset >> 8;
    image[20] = n;
    for( j = 0; j < n; j++ ) {
      image[21 + j * 7 + 2] = j + 1;
      image[21 + j * 7 + 3] = size;
      image[21 + j * 7 + 4] = flags[test];
      /* Missing-data offsets need not point into the file. */
      if( missing ) image[21 + j * 7 + 6] = 255;
    }
    memset( image + data_offset, 0x5a, missing ? 0 : length );
    utils_file_init( &file, "semantics.fdi" );
    file.buffer = image;
    file.length = data_offset + ( missing ? 0 : length );
    if( test == 10 ) file.length--; /* truncated sector header */
    file.type = LIBSPECTRUM_ID_DISK_FDI;
    memset( &disk, 0, sizeof( disk ) );
    pos = disk_open_loaded( &disk, &file, test & 1, 0 );
    if( test == 10 ) {
      if( pos != DISK_OPEN ) r++;
      disk_close( &disk );
      continue;
    }
    if( pos != DISK_OK ) {
      r++;
      disk_close( &disk );
      continue;
    }
    DISK_SET_TRACK_IDX( ( &disk ), 0 );
    ids = marks = 0;
    id_pos = data_pos = -1;
    mfm = test == 8 || test == 9;
    for( j = 0; j < disk.c_bpt; j++ ) {
      if( mfm ? j < 3 || !bitmap_test( disk.clocks, j - 1 ) :
                !bitmap_test( disk.clocks, j ) ) continue;
      if( disk.track[j] == 0xfe ) { ids++; id_pos = j; }
      if( disk.track[j] == 0xfb || disk.track[j] == 0xf8 ) {
        marks++; data_pos = j;
      }
    }
    if( ids != n || marks != ( missing ? 0 : 1 ) ) r++;
    /* Check ID and data CRC independently, including MFM's three A1s. */
    if( id_pos < 0 ) r++;
    else {
      start = id_pos - ( mfm ? 3 : 0 );
      crc = 0xffff;
      for( k = start; k < id_pos + 7; k++ )
        crc = crc_fdc( crc, disk.track[k] );
      if( crc ) r++;
      if( disk.track[id_pos + 3] != n || disk.track[id_pos + 4] != size ) r++;
    }
    if( !missing && data_pos >= 0 ) {
      if( disk.track[data_pos] != ( flags[test] & 0x80 ? 0xf8 : 0xfb ) ) r++;
      for( k = 0; k < length; k++ )
        if( disk.track[data_pos + 1 + k] != 0x5a ) { r++; break; }
      crc = 0xffff;
      start = data_pos - ( mfm ? 3 : 0 );
      for( k = start; k < data_pos + length + 3; k++ )
        crc = crc_fdc( crc, disk.track[k] );
      if( ( crc == 0 ) != ( ( flags[test] & ( 1 << size ) ) != 0 ) ) r++;
    }
    disk_close( &disk );
  }
  if( r ) printf( "disk_open_fdi_semantics_test failed: %d\n", r );
  return r;
}

static FILE compat_file_test_file;
static int compat_file_test_calls[ 7 ];

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
static int compat_file_test_unlink( const char *path GCC_UNUSED )
{ compat_file_test_calls[ 6 ]++; return 0; }

static int
compat_file_vtable_test( void )
{
  compat_file_vtable_t vtable = {
    compat_file_test_open, compat_file_test_get_length, compat_file_test_read,
    compat_file_test_write, compat_file_test_close, compat_file_test_exists,
    compat_file_test_unlink
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
      !compat_file_exists( "test" ) || compat_file_unlink( "test" ) ) r++;

  compat_file_set_vtable( &previous_vtable );
  for( i = 0; i < 7; i++ ) if( compat_file_test_calls[ i ] != 1 ) r++;
  if( r ) printf( "compat_file_vtable_test failed\n" );
  return r;
}

#ifdef ENABLE_AUTOMATION

/* Little-endian writers used to build the expected audio.wav contents */
static void
automation_audio_test_put_le16( libspectrum_byte *p, unsigned value )
{
  p[ 0 ] = value;
  p[ 1 ] = value >> 8;
}

static void
automation_audio_test_put_le32( libspectrum_byte *p, unsigned long value )
{
  p[ 0 ] = value;
  p[ 1 ] = value >> 8;
  p[ 2 ] = value >> 16;
  p[ 3 ] = value >> 24;
}

/* Round-trip the automation artifacts' WAV writer: capture a short PCM
   buffer, write audio.wav into a temporary directory and check the
   resulting RIFF header field by field and the s16le payload byte for
   byte */
static int
automation_audio_artifacts_test( void )
{
  static const libspectrum_signed_word samples[] = {
    -32768, -16000, -3200, -400, 0, 400, 3200, 16000
  };
  const size_t sample_count = ARRAY_SIZE( samples );
  const size_t byte_count = 2 * sample_count;
  const unsigned int rate = 44100, channels = 1;
  char dir_template[] = "/tmp/fuse-automation-audio-XXXXXX";
  char wav_path[ PATH_MAX ];
  libspectrum_byte expected[ 44 + 2 * ARRAY_SIZE( samples ) ];
  libspectrum_byte *payload;
  utils_file file;
  size_t i;
  int fd, r = 0;

  memset( &file, 0, sizeof( file ) );

  fd = mkstemp( dir_template );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( dir_template );
  if( mkdir( dir_template, 0700 ) ) {
    printf( "automation_audio_artifacts_test: failed to create directory\n" );
    return 1;
  }
  snprintf( wav_path, sizeof( wav_path ), "%s" FUSE_DIR_SEP_STR "audio.wav",
            dir_template );

  automation_artifacts_audio_initialized( (int) rate, (int) channels );
  automation_artifacts_capture_pcm( samples, (int) sample_count,
                                    (int) rate, (int) channels );

  if( automation_artifacts_write( dir_template, 0, 1 ) ||
      utils_read_file( wav_path, &file ) ||
      file.length != 44 + byte_count ) {
    printf( "automation_audio_artifacts_test: failed to write or read "
            "audio.wav\n" );
    r++;
    goto cleanup;
  }

  /* The header must describe exactly what was captured */
  memset( expected, 0, sizeof( expected ) );
  memcpy( expected, "RIFF", 4 );
  automation_audio_test_put_le32( expected + 4, 36 + byte_count );
  memcpy( expected + 8, "WAVEfmt ", 8 );
  automation_audio_test_put_le32( expected + 16, 16 );
  automation_audio_test_put_le16( expected + 20, 1 );	/* PCM */
  automation_audio_test_put_le16( expected + 22, channels );
  automation_audio_test_put_le32( expected + 24, rate );
  automation_audio_test_put_le32( expected + 28, rate * channels * 2 );
  automation_audio_test_put_le16( expected + 32, channels * 2 );
  automation_audio_test_put_le16( expected + 34, 16 );	/* bits per sample */
  memcpy( expected + 36, "data", 4 );
  automation_audio_test_put_le32( expected + 40, byte_count );

  payload = expected + 44;
  for( i = 0; i < sample_count; i++ )
    automation_audio_test_put_le16( payload + 2 * i,
                                    (unsigned short) samples[ i ] );

  if( memcmp( file.buffer, expected, sizeof( expected ) ) ) {
    printf( "automation_audio_artifacts_test: wrong audio.wav contents\n" );
    r++;
  }

cleanup:
  utils_close_file( &file );
  unlink( wav_path );
  rmdir( dir_template );
  automation_artifacts_end();
  if( r ) printf( "automation_audio_artifacts_test failed\n" );
  return r;
}

#endif /* ENABLE_AUTOMATION */

static int
screenshot_scr_roundtrip_test( void )
{
  static const char temp_prefix[] = "/tmp/fuse-scr-test-XXXXXX";
  char temporary_path[ sizeof( temp_prefix ) ];
  libspectrum_machine old_machine = machine_current->machine;
  libspectrum_dword *saved_last_screen;
  libspectrum_byte saved_screen[ DISPLAY_FILE_SIZE ];
  libspectrum_byte *expected;
  utils_file file;
  int fd, x, y, r = 0;
  size_t j, count;

  /* mkdtemp is not provided by MinGW; use mkstemp to obtain a unique
     filename prefix instead. */
  strcpy( temporary_path, temp_prefix );
  fd = mkstemp( temporary_path );
  if( fd < 0 ) return 1;
  close( fd );
  unlink( temporary_path );

  if( machine_select( LIBSPECTRUM_MACHINE_48 ) ) {
    unlink( temporary_path );
    return 1;
  }

  /* Save the display and screen state the test touches */
  saved_last_screen = libspectrum_new( libspectrum_dword,
    DISPLAY_SCREEN_WIDTH_COLS * DISPLAY_SCREEN_HEIGHT );
  memcpy( saved_last_screen, display_last_screen,
          sizeof( display_last_screen ) );
  memcpy( saved_screen, RAM[ memory_current_screen ], DISPLAY_FILE_SIZE );

  /* Fill the display's last-screen buffer with a distinctive pattern: the
     low byte carries the pixel value and the high byte the attribute */
  count = DISPLAY_SCREEN_WIDTH_COLS * DISPLAY_SCREEN_HEIGHT;
  for( j = 0; j < count; j++ )
    display_last_screen[ j ] = ( ( j % 256 ) << 8 ) | ( j / 256 );

  /* Derive what the standard .scr writer must produce from the known .scr
     layout rather than from screenshot.c itself */
  expected = libspectrum_new( libspectrum_byte, DISPLAY_FILE_SIZE );
  for( y = 0; y < DISPLAY_HEIGHT; y++ ) {
    for( x = 0; x < DISPLAY_WIDTH_COLS; x++ ) {
      libspectrum_dword cell;
      int index = ( x + DISPLAY_BORDER_WIDTH_COLS ) +
                  ( y + DISPLAY_BORDER_HEIGHT ) * DISPLAY_SCREEN_WIDTH_COLS;
      cell = display_last_screen[ index ];
      expected[ display_get_offset( x, y ) ] = cell & 0xff;
      if( y % 8 == 0 )
        expected[ DISPLAY_PIXEL_BYTES + x +
                  ( y / 8 ) * DISPLAY_WIDTH_COLS ] = ( cell >> 8 ) & 0xff;
    }
  }

  if( screenshot_scr_write( temporary_path ) ) r++;
  if( utils_read_file( temporary_path, &file ) ) {
    r++;
  } else {
    if( file.length != DISPLAY_FILE_SIZE ) r++;
    else if( memcmp( file.buffer, expected, DISPLAY_FILE_SIZE ) ) r++;
    utils_close_file( &file );
  }

  /* Reading the file back must place the same bytes in the screen page */
  if( screenshot_scr_read( temporary_path ) ) r++;
  if( memcmp( RAM[ memory_current_screen ], expected, DISPLAY_FILE_SIZE ) )
    r++;

  /* Restore the display and screen state */
  memcpy( display_last_screen, saved_last_screen,
          sizeof( display_last_screen ) );
  memcpy( RAM[ memory_current_screen ], saved_screen, DISPLAY_FILE_SIZE );
  libspectrum_free( saved_last_screen );
  libspectrum_free( expected );

  if( machine_select( old_machine ) ) r++;
  unlink( temporary_path );

  if( r ) printf( "screenshot_scr_roundtrip_test failed\n" );
  return r;
}

/* Event queue tests */

/* Fired-event log for the event queue tests */
static int event_test_fired;
static libspectrum_dword event_test_tstates[ 8 ];
static int event_test_type[ 8 ];
static void *event_test_user_data[ 8 ];

static void
log_event( libspectrum_dword fired_tstates, int type, void *user_data )
{
  event_test_tstates[ event_test_fired ] = fired_tstates;
  event_test_type[ event_test_fired ] = type;
  event_test_user_data[ event_test_fired ] = user_data;
  event_test_fired++;
}

/* Rescheduling handler: fires at fired_tstates, then queues itself again
   0x10 T-states later */
static void
reschedule_fn( libspectrum_dword fired_tstates, int type, void *user_data )
{
  log_event( fired_tstates, type, user_data );
  event_add( fired_tstates + 0x10, type );
}

static int
expect( int index, libspectrum_dword fired_tstates, int type, void *user_data )
{
  if( index >= event_test_fired ) return 1;
  if( event_test_tstates[ index ] != fired_tstates ) return 1;
  if( event_test_type[ index ] != type ) return 1;
  if( event_test_user_data[ index ] != user_data ) return 1;
  return 0;
}

static int
event_queue_test( void )
{
  libspectrum_dword saved_tstates = tstates;
  libspectrum_dword saved_tstates_per_frame;
  int type_q, type_p, type_r, type_a, type_d, type_h, type_i, type_j,
    type_resched, error = 0;

  /* Clear whatever the emulator itself queued; the main loop is done by
     the time the unit tests run */
  event_reset();

  /* --- Test 1: the comparator orders by unsigned T-state, not by signed
     subtraction (event.c pins this with the overflow-safe form) --- */
  type_q = event_register( log_event, "unittest-q" );
  type_p = event_register( log_event, "unittest-p" );
  type_r = event_register( log_event, "unittest-r" );
  if( type_q < 0 || type_p <= type_q || type_r <= type_p ) error++;
  if( strcmp( event_name( type_q ), "unittest-q" ) ) error++;

  event_add( 0x00000100, type_q );
  event_add( 0xe0000000, type_p );
  event_add( 0x40000000, type_r );

  if( event_next_event != 0x00000100 ) error++;

  tstates = 0xe0000001;
  if( event_do_events() ) error++;
  if( event_test_fired != 3 ) error++;
  if( expect( 0, 0x00000100, type_q, NULL ) ) error++;
  if( expect( 1, 0x40000000, type_r, NULL ) ) error++;
  if( expect( 2, 0xe0000000, type_p, NULL ) ) error++;

  /* --- Test 2: equal T-states fire in registration order --- */
  event_test_fired = 0;
  type_a = event_register( log_event, "unittest-a" );
  type_d = event_register( log_event, "unittest-d" );
  event_add( 0x80, type_a );
  event_add( 0x80, type_d );
  if( event_next_event != 0x80 ) error++;

  tstates = 0x80;
  if( event_do_events() ) error++;
  if( event_test_fired != 2 ) error++;
  if( expect( 0, 0x80, type_a, NULL ) ) error++;
  if( expect( 1, 0x80, type_d, NULL ) ) error++;

  /* --- Test 3: event_frame() reduces every entry and recomputes the next
     event time --- */
  event_test_fired = 0;
  event_add( 0x50, type_a );
  event_add( 0x60, type_d );
  event_frame( 0x20 );
  if( event_next_event != 0x30 ) error++;

  tstates = 0x45;
  if( event_do_events() ) error++;
  if( event_test_fired != 2 ) error++;
  if( expect( 0, 0x30, type_a, NULL ) ) error++;
  if( expect( 1, 0x40, type_d, NULL ) ) error++;

  /* --- Test 4: event_remove_type() turns matching events null, and null
     events are dispatched silently --- */
  event_test_fired = 0;
  type_h = event_register( log_event, "unittest-h" );
  type_i = event_register( log_event, "unittest-i" );
  event_add( 0x30, type_h );
  event_add( 0x40, type_h );
  event_add( 0x50, type_i );
  event_remove_type( type_h );

  tstates = 0x60;
  if( event_do_events() ) error++;
  if( event_test_fired != 1 ) error++;
  if( expect( 0, 0x50, type_i, NULL ) ) error++;
  if( event_next_event != 0xffffffff ) error++;

  /* --- Test 5: event_remove_type_user_data() nulls only matching
     user data --- */
  event_test_fired = 0;
  type_j = event_register( log_event, "unittest-j" );
  event_add_with_data( 0x30, type_j, (void *) 1 );
  event_add_with_data( 0x40, type_j, (void *) 2 );
  event_remove_type_user_data( type_j, (void *) 1 );

  tstates = 0x50;
  if( event_do_events() ) error++;
  if( event_test_fired != 1 ) error++;
  if( expect( 0, 0x40, type_j, (void *) 2 ) ) error++;

  /* --- Test 6: a callback may queue another event, and the dispatch loop
     picks it up in the same call --- */
  event_test_fired = 0;
  type_resched = event_register( reschedule_fn, "unittest-resched" );
  event_add( 0x100, type_resched );

  tstates = 0x115;
  if( event_do_events() ) error++;
  if( event_test_fired != 2 ) error++;
  if( expect( 0, 0x100, type_resched, NULL ) ) error++;
  if( expect( 1, 0x110, type_resched, NULL ) ) error++;

  /* --- Test 7: event_force_events() jumps to the next event (RZX
     playback path) --- */
  event_reset();
  event_test_fired = 0;
  saved_tstates_per_frame = machine_current->timings.tstates_per_frame;
  machine_current->timings.tstates_per_frame = 0x200;
  event_add( 0x50, type_i );
  event_force_events();
  if( event_test_fired != 1 ) error++;
  if( expect( 0, 0x50, type_i, NULL ) ) error++;
  machine_current->timings.tstates_per_frame = saved_tstates_per_frame;

  /* Leave the queue exactly as the emulator left it: empty */
  event_reset();
  tstates = saved_tstates;

  if( error ) printf( "event_queue_test failed\n" );
  return error;
}

int
unittests_run( void )
{
  int r = 0;

  r += contention_test();
  r += rzx_post_interrupt_autosave_test();
  r += rzx_retrigger_autosave_test();
  r += floating_bus_test();
  r += loader_unittest();
  r += tape_unittest();
  r += blip_synth_level_test();
  r += dc_filter_test();
  r += speaker_filter_test();
  r += tv_filter_test();
  r += ula_filter_test();
  r += sound_source_routes_test();
  r += ula_sound_levels_test();
  r += source_volume_test();
  r += ay_render_grid_continuity_test();
  r += ay_tail_write_survives_frame_boundary_test();
  r += floating_bus_merge_test();
  r += snapshot_copy_from_releases_keyboard_test();
  r += snapshot_copy_from_releases_joystick_test();
  r += snapshot_custom_rom_is_replaced_by_soft_reset_test();
  r += snapshot_machine_rom_without_file_test();
  r += snapshot_missing_rom_file_uses_snapshot_rom_test();
  r += snapshot_write_read_roundtrip_test();
  r += slt_is_cleared_by_reset_test();
  r += slt_screen_is_cleared_by_reset_test();
  r += snapshot_dock_carts_cleared_by_hard_reset_test();
  r += spec_se_dock_ram_reset_test();
  r += ulaplus_ports_test();
  r += ulaplus_snapshot_test();
  r += spec_se_dock_active_machine_switch_test();
  r += keyboard_read_test();
  r += keyboard_synthetic_test();
  r += keyboard_simulate_keypress_test();
  r += keyboard_shifted_arrows_test();
  r += keyboard_shifted_arrows_release_all_test();
  r += utils_safe_strdup_test();
  r += bitmap_ops_test();
  r += mempool_test();
  r += psg_unittest();
  r += paging_test();
  r += pokefinder_unittest();
  r += debugger_disassemble_unittest();
  r += debugger_expression_unittest();
  r += rectangle_test();
  r += rectangle_realloc_test();
  r += scaler_for_size_test();
  r += compat_file_vtable_test();
  r += machine_load_rom_bank_with_sizes_test();
  r += utils_file_lifecycle_test();
  r += utils_file_read_failure_test();
  r += utils_file_harddisk_identify_test();
  r += utils_open_loaded_file_test();
  r += utils_open_loaded_if2_test();
  r += utils_open_loaded_dck_test();
  r += dck_sparse_banks_test();
  r += dck_independent_pages_test();
  r += dck_lifecycle_test();
  r += dck_archive_test();
  r += dck_snapshot_reset_test();
  r += dck_replacement_power_cycle_test();
  r += dck_malformed_test();
  r += dck_home_rom_ram_slots_test();
  r += utils_open_loaded_microdrive_test();
  r += utils_open_loaded_disk_test();
  r += utils_open_loaded_compressed_disk_test();
  r += utils_open_loaded_disk_merge_test();
  r += disk_write_blank_disk_cpc_save_test();
  r += disk_write_open_udi_roundtrip_test();
  r += disk_open_malformed_udi_rejected_test();
  r += disk_open_fdi_bounds_test();
  r += disk_open_fdi_semantics_test();
  r += disk_open_fdi_recovery_test();
  r += screenshot_scr_roundtrip_test();
  r += event_queue_test();
#ifdef ENABLE_AUTOMATION
  r += automation_audio_artifacts_test();
#endif /* ENABLE_AUTOMATION */

  printf("Final return value: %d (should be 0)\n", r);

  return r;
}
