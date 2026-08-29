/* spectranet.h: Spectranet emulation
   Copyright (c) 2011-2016 Philip Kendall

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

#ifndef FUSE_SPECTRANET_H
#define FUSE_SPECTRANET_H

#include "libspectrum.h"
#include "memory_pages.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Configuration page is ROM page 0x1F (31)
#define SPECTRANET_CONFIG_PAGE (0x1F)
#define SPECTRANET_CONFIG_PAGE_SIZE (4096)

#define OFS_INIT_FLAGS (0x0F12)
#define INIT_ENABLE_SPECTRANEXT_BIT (2)

// Configuration item type flags (top 2 bits of item ID)
#define CONFIG_ITEM_TYPE_STRING (0x00)
#define CONFIG_ITEM_TYPE_BYTE   (0x80)
#define CONFIG_ITEM_TYPE_INT    (0xC0)
#define CONFIG_ITEM_TYPE_MASK   (0xC0)

// Auto-mount section ID
#define CONFIG_SECTION_AUTO_MOUNT (0x01FF)

// Auto-mount item IDs
#define CONFIG_ITEM_MOUNT_RESOURCE (0x00)  // String: mount resource (e.g., "ram", "1:/path")
#define CONFIG_ITEM_AUTO_BOOT      (0x81)  // Byte: auto-boot flag (0 or 1)

// Configuration option structure
typedef struct {
    uint8_t type;  // 's' = string, 'b' = byte, 'i' = int
    union {
        const char* string_value;
        uint8_t byte_value;
        uint16_t int_value;
    } value;
} spectranet_config_option_t;

typedef struct {
  int available;
  int paged;
  int page_0;
  int page_a;
  int page_b;
  int page_3;
} spectranet_paging_info_t;

// get currently loaded config
const uint8_t* spectranet_config_get_memory(void);

// Read functions
int spectranet_config_get_string(uint16_t section_id, uint8_t item_id, char* buffer, size_t buffer_size);
int spectranet_config_get_byte(uint16_t section_id, uint8_t item_id, uint8_t* value);
int spectranet_config_get_int(uint16_t section_id, uint8_t item_id, uint16_t* value);

// Write functions (modify configuration in memory)
int spectranet_config_set_string(uint16_t section_id, uint8_t item_id, const char* value);
int spectranet_config_set_byte(uint16_t section_id, uint8_t item_id, uint8_t value);
int spectranet_config_set_int(uint16_t section_id, uint8_t item_id, uint16_t value);

// Utility: Get total configuration size
uint16_t spectranet_config_get_total_size(void);

// Utility: Check if configuration is valid
bool spectranet_config_is_valid(void);

void spectranet_register_startup( void );
void spectranet_map_page( int dest, int source );
/* Return the backing storage for a 4K Spectranet RAM page, or NULL. */
uint8_t *spectranet_ram_page( uint8_t page );
void spectranet_page( int via_io );
void spectranet_nmi( void );
void spectranet_unpage( void );
void spectranet_retn( void );

int spectranet_nmi_flipflop( void );

libspectrum_byte spectranet_w5100_read( memory_page *page, libspectrum_word address );
void spectranet_w5100_write( memory_page *page, libspectrum_word address, libspectrum_byte b );
libspectrum_byte spectranet_xfs_read( memory_page *page, libspectrum_word address );
void spectranet_xfs_write( memory_page *page, libspectrum_word address, libspectrum_byte b );
libspectrum_byte spectranet_spectranext_config_read( memory_page *page, libspectrum_word address );
void spectranet_spectranext_config_write( memory_page *page, libspectrum_word address, libspectrum_byte b );
libspectrum_byte spectranet_flash_rom_read( memory_page *page, libspectrum_word address );
void spectranet_flash_rom_write( libspectrum_word address, libspectrum_byte b );

extern int spectranet_available;
extern int spectranet_paged;
extern int spectranet_w5100_paged_0, spectranet_w5100_paged_a,
           spectranet_w5100_paged_b, spectranet_w5100_paged_3;
extern int spectranet_xfs_paged_0, spectranet_xfs_paged_a,
           spectranet_xfs_paged_b, spectranet_xfs_paged_3;
extern int spectranet_spectranext_config_paged_0,
           spectranet_spectranext_config_paged_a,
           spectranet_spectranext_config_paged_b,
           spectranet_spectranext_config_paged_3;
extern int spectranet_flash_paged_0, spectranet_flash_paged_a,
           spectranet_flash_paged_b, spectranet_flash_paged_3;
extern int spectranet_programmable_trap_active;
extern libspectrum_word spectranet_programmable_trap;

/* Get pointer to Spectranet ROM configuration page (page 0x1F) */
libspectrum_byte* spectranet_get_config_page(void);
int spectranet_dump_ram( const char *filename );
spectranet_paging_info_t spectranet_get_paging_info( void );

#endif /* #ifndef FUSE_SPECTRANET_H */
