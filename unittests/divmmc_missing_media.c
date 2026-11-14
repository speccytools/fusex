/* Link against a null-UI libfusex and run with:
 * --machine=48 --no-spectranet --no-divmmc --rom-dir=<roms>
 * --divmmc-file=<nonexistent image> --no-banner
 * Optionally set FUSEX_TEST_DISK to a valid image to check reinsertion.
 */
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuse.h"
#include "machine.h"
#include "settings.h"
#include "spectrum.h"
#include "peripherals/ide/divmmc.h"

static void
check_basic_boot( void )
{
  unsigned i;
  libspectrum_byte frames;
  for( i = 0; i < 250; ++i ) spectrum_do_frame();
  /* The 48K ROM has reached its interrupt-driven BASIC loop. */
  frames = readbyte( 23672 );
  for( i = 0; i < 10; ++i ) spectrum_do_frame();
  assert( (libspectrum_byte)( readbyte( 23672 ) - frames ) == 10 );
}

int
main( int argc, char **argv )
{
  const char *valid_image = getenv( "FUSEX_TEST_DISK" );
  assert( fuse_init( argc, argv ) == 0 );
  assert( settings_current.divmmc_file == NULL );
  check_basic_boot();
  assert( machine_reset( 0 ) == 0 );
  check_basic_boot();
  assert( machine_reset( 1 ) == 0 );
  check_basic_boot();
  assert( divmmc_eject() == 0 );
  if( valid_image ) {
    assert( divmmc_insert( valid_image ) == 0 );
    assert( !strcmp( settings_current.divmmc_file, valid_image ) );
  }
  assert( divmmc_insert( "/nonexistent/fusex-test-missing-card.hdf" ) != 0 );
  assert( settings_current.divmmc_file == NULL );
  assert( machine_reset( 1 ) == 0 );
  check_basic_boot();
  if( valid_image ) {
    assert( divmmc_insert( valid_image ) == 0 );
    assert( !strcmp( settings_current.divmmc_file, valid_image ) );
    assert( divmmc_eject() == 0 );
    assert( settings_current.divmmc_file == NULL );
  }
  assert( fuse_end() == 0 );
  puts( "PASS: missing media cleared; BASIC boots, warm/hard resets, insertion and ejection recover" );
  return 0;
}
