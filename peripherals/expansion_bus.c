/* Ordered expansion-bus cycle routing.

   The Z80 presents CPU cycles here. This layer applies the electrical effect
   of upstream devices before forwarding the cycle to downstream devices. */

#include "config.h"

#include <stddef.h>

#include "peripherals/expansion_bus.h"

#define EXPANSION_BUS_MAX_DEVICES 16
#define EXPANSION_BUS_MAX_OBSERVERS 16

static const expansion_bus_device_t *devices[ EXPANSION_BUS_MAX_DEVICES ];
static const expansion_bus_observer_t *observers[
  EXPANSION_BUS_MAX_OBSERVERS ];
static libspectrum_word m1_addresses[ EXPANSION_BUS_MAX_DEVICES ];
static int m1_active[ EXPANSION_BUS_MAX_DEVICES ];
static size_t device_count;
static size_t observer_count;

void
expansion_bus_register( const expansion_bus_device_t *device )
{
  size_t position = device_count;

  if( device_count == EXPANSION_BUS_MAX_DEVICES ) return;

  while( position && devices[ position - 1 ]->position > device->position ) {
    devices[ position ] = devices[ position - 1 ];
    position--;
  }

  devices[ position ] = device;
  device_count++;
}

void
expansion_bus_register_observer( const expansion_bus_observer_t *observer )
{
  if( observer_count == EXPANSION_BUS_MAX_OBSERVERS ) return;
  observers[ observer_count++ ] = observer;
}

int
expansion_bus_active( void )
{
  size_t i;

  for( i = 0; i < device_count; i++ )
    if( devices[i]->active() ) return 1;

  return 0;
}

int
expansion_bus_has_active_downstream( periph_type requester,
                                     unsigned int capability )
{
  size_t i;
  int requester_position = -1;

  for( i = 0; i < device_count; i++ )
    if( devices[i]->type == requester ) {
      requester_position = devices[i]->position;
      break;
    }

  if( requester_position < 0 ) return 0;

  for( i = 0; i < device_count; i++ )
    if( devices[i]->position > requester_position && devices[i]->active() &&
        ( devices[i]->capabilities & capability ) )
      return 1;

  return 0;
}

void
expansion_bus_m1_begin( libspectrum_word address )
{
  size_t i;

  for( i = 0; i < observer_count; i++ )
    if( observers[i]->active() && observers[i]->m1_begin )
      observers[i]->m1_begin( address );

  for( i = 0; i < device_count; i++ ) {
    const expansion_bus_device_t *device = devices[i];

    m1_active[i] = device->active();
    m1_addresses[i] = address;
    if( !m1_active[i] ) continue;

    if( device->m1_begin ) device->m1_begin( address );
    if( device->downstream_address )
      address = device->downstream_address( address );
  }
}

void
expansion_bus_m1_end( libspectrum_word address, libspectrum_byte opcode )
{
  size_t i;

  for( i = 0; i < observer_count; i++ )
    if( observers[i]->active() && observers[i]->m1_end_begin )
      observers[i]->m1_end_begin();

  /* The downstream device releases a cycle before an upstream device changes
     the signals which were presented to it for that cycle. */
  for( i = device_count; i; i-- ) {
    const expansion_bus_device_t *device = devices[i - 1];
    if( m1_active[i - 1] && device->m1_end )
      device->m1_end( m1_addresses[i - 1] );
  }

  for( i = 0; i < observer_count; i++ )
    if( observers[i]->active() && observers[i]->m1_end )
      observers[i]->m1_end( address, opcode );
}

int
expansion_bus_nmi_suppressed( void )
{
  size_t i;

  for( i = device_count; i; i-- ) {
    const expansion_bus_device_t *device = devices[i - 1];
    if( device->active() &&
        ( device->capabilities & EXPANSION_BUS_CAP_NMI_ROM ) )
      return device->nmi_suppressed ? device->nmi_suppressed() : 0;
  }

  return 0;
}

void
expansion_bus_nmi_page( void )
{
  size_t i;

  for( i = device_count; i; i-- ) {
    const expansion_bus_device_t *device = devices[i - 1];
    if( device->active() &&
        ( device->capabilities & EXPANSION_BUS_CAP_NMI_ROM ) ) {
      if( device->nmi_page ) device->nmi_page();
      return;
    }
  }
}

void
expansion_bus_retn( void )
{
  size_t i;

  for( i = 0; i < device_count; i++ )
    if( devices[i]->active() && devices[i]->retn )
      devices[i]->retn();
}

int
expansion_bus_romcs_granted_on_reset( periph_type requester )
{
  size_t i;

  for( i = 0; i < observer_count; i++ )
    if( observers[i]->reset ) observers[i]->reset();

  return !expansion_bus_has_active_downstream(
    requester, EXPANSION_BUS_CAP_RESET_ROM );
}
