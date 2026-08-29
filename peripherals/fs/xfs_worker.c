#include "config.h"

#include "xfs.h"
#include "xfs_engines.h"
#include "xfs_worker.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#if defined(WIN32) || defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "libspectrum.h"
#include "../spectranet.h"
#include "debugger/gdbserver.h"
#include "memory_pages.h"
#include "ui/ui.h"
#include "compat.h"
#include "utils.h"

volatile struct xfs_registers_t xfs_registers = {};

// XFS debug logging control
static bool xfs_debug_enabled = false;

// Function to enable/disable XFS debug logging
void xfs_debug_enable(bool enable)
{
    xfs_debug_enabled = enable;
}

bool xfs_debug_is_enabled(void)
{
    return xfs_debug_enabled;
}

// XFS base directory path
char xfs_base_path[512];

void xfs_init()
{
    xfs_compat_init();

    snprintf(xfs_base_path, sizeof(xfs_base_path), "%s/xfs", compat_get_config_path());
    
    // Create "xfs" directory in Application Support directory
#if defined(WIN32) || defined(_WIN32)
    if (_mkdir(xfs_base_path) != 0 && errno != EEXIST)
#else
    if (mkdir(xfs_base_path, 0755) != 0 && errno != EEXIST)
#endif
    {
        ui_error( UI_ERROR_WARNING, "xfs: failed to create xfs directory: %s\n", strerror(errno) );
    }
    
    XFS_DEBUG("xfs: initialized with base path: %s\n", xfs_base_path ? xfs_base_path : "(null)");
}

void xfs_reset(void)
{
    XFS_DEBUG("xfs: reset - cleaning up all mounts and handles\n");
    
    // Call xfs_free() from xfs.c to clean up all resources
    xfs_free();
    
    XFS_DEBUG("xfs: reset complete\n");
}

libspectrum_byte xfs_read( memory_page *page GCC_UNUSED, libspectrum_word address )
{
    libspectrum_word offset = address & 0xfff;  // XFS is single page (0x49)
    uint8_t *registers = (uint8_t*)&xfs_registers;
    return registers[offset];
}

void xfs_write( memory_page *page GCC_UNUSED, libspectrum_word address, libspectrum_byte b )
{
    libspectrum_word offset = address & 0xfff;
    uint8_t *registers = (uint8_t*)&xfs_registers;
    registers[offset] = b;
    
    if (offset == 0)
    {
        // Command register written - process command via dispatcher
        xfs_handle_command(&xfs_registers);
    }
}
