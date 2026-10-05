/* Spectranext/esxDOS coexistence protocol.

   This file intentionally owns knowledge of the hand-off sequences used by
   esxDOS firmware. Neither the Z80 core nor the emulated devices should need
   to know these ROM implementation details. */

#include "config.h"

#include "periph.h"
#include "peripherals/expansion_bus.h"
#include "peripherals/ide/divmmc.h"
#include "peripherals/spectranet.h"
#include "peripherals/spectranext_esxdos.h"
#include "settings.h"

/* Valid only between the $1ffb M1 and the immediately following M1. */
static int rst8_rejection_pending;
static int divmmc_was_paged;

static int
spectranext_esxdos_active( void )
{
  return spectranet_available &&
         periph_is_active( PERIPH_TYPE_DIVMMC );
}

static void
spectranext_esxdos_reset( void )
{
  rst8_rejection_pending = 0;
}

static void
spectranext_esxdos_m1_begin( libspectrum_word address )
{
  if( !spectranext_esxdos_active() ) {
    rst8_rejection_pending = 0;
    return;
  }

  if( rst8_rejection_pending && address == 0x0058 &&
      !divmmc_is_paged() ) {
    spectranet_page( 0 );
  }
  rst8_rejection_pending = 0;

  if( settings_current.spectranet_disable ) return;

  /* Replace the 48K ROM's OUT ($FE),A with our LD-BYTES jump. */
  if( address == 0x055c && spectranet_programmable_trap_is( 0x0562 ) &&
      !spectranet_paged && !divmmc_is_paged() )
    spectranet_page( 0 );

  /* Spectranet's explicit page-in island remains available once the
     downstream DivMMC has released the lower ROM. */
  if( ( address & 0xfff8 ) == 0x3ff8 && !divmmc_is_paged() )
    spectranet_page( 0 );
}

static void
spectranext_esxdos_m1_end_begin( void )
{
  divmmc_was_paged = spectranext_esxdos_active() && divmmc_is_paged();
}

static void
spectranext_esxdos_m1_end( libspectrum_word address,
                           libspectrum_byte opcode )
{
  if( !spectranext_esxdos_active() ) return;

  /* esxDOS 0.8.9 rejects an unclaimed BASIC RST 8 request by executing
     JP (HL) at $1ffb, after its delayed automapper has released the ROM.
     The next-M1 check in m1_begin confirms that HL selected ROM $0058. */
  if( address == 0x1ffb && opcode == 0xe9 && divmmc_was_paged &&
      !divmmc_is_paged() )
    rst8_rejection_pending = 1;
}

static const expansion_bus_observer_t spectranext_esxdos_observer = {
  /* .active = */ spectranext_esxdos_active,
  /* .m1_begin = */ spectranext_esxdos_m1_begin,
  /* .m1_end_begin = */ spectranext_esxdos_m1_end_begin,
  /* .m1_end = */ spectranext_esxdos_m1_end,
  /* .reset = */ spectranext_esxdos_reset,
};

void
spectranext_esxdos_register( void )
{
  expansion_bus_register_observer( &spectranext_esxdos_observer );
}
