/* Ordered expansion-bus cycle routing. */

#ifndef FUSE_EXPANSION_BUS_H
#define FUSE_EXPANSION_BUS_H

#include "libspectrum.h"
#include "periph.h"

enum {
  EXPANSION_BUS_CAP_AUTOMAP       = 1 << 0,
  EXPANSION_BUS_CAP_RESET_ROM     = 1 << 1,
  EXPANSION_BUS_CAP_NMI_ROM       = 1 << 2,
};

enum {
  EXPANSION_BUS_POSITION_SPECTRANET = 100,
  EXPANSION_BUS_POSITION_DIVMMC     = 200,
};

typedef struct expansion_bus_device_t {
  periph_type type;
  int position;
  unsigned int capabilities;
  int (*active)( void );
  void (*m1_begin)( libspectrum_word address );
  void (*m1_end)( libspectrum_word address );
  libspectrum_word (*downstream_address)( libspectrum_word address );
  int (*nmi_suppressed)( void );
  void (*nmi_page)( void );
  void (*retn)( void );
} expansion_bus_device_t;

/* Observers implement protocols which involve more than one device. They
   observe the CPU-visible address rather than an address transformed for a
   particular point in the physical expansion chain. */
typedef struct expansion_bus_observer_t {
  int (*active)( void );
  void (*m1_begin)( libspectrum_word address );
  void (*m1_end_begin)( void );
  void (*m1_end)( libspectrum_word address, libspectrum_byte opcode );
  void (*reset)( void );
} expansion_bus_observer_t;

void expansion_bus_register( const expansion_bus_device_t *device );
void expansion_bus_register_observer(
  const expansion_bus_observer_t *observer );
int expansion_bus_active( void );
int expansion_bus_has_active_downstream( periph_type requester,
                                         unsigned int capability );
void expansion_bus_m1_begin( libspectrum_word address );
void expansion_bus_m1_end( libspectrum_word address,
                           libspectrum_byte opcode );

int expansion_bus_nmi_suppressed( void );
void expansion_bus_nmi_page( void );
void expansion_bus_retn( void );

/* Whether an expansion device is electrically allowed to assert ROMCS as a
   consequence of reset, after upstream/downstream ownership is resolved. */
int expansion_bus_romcs_granted_on_reset( periph_type requester );

#endif /* #ifndef FUSE_EXPANSION_BUS_H */
