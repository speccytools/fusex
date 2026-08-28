#include "config.h"

#include "xfs.h"
#include "xfs_engines.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

#include "debugger/gdbserver.h"
#include "utils.h"

static char xfs_cwd_buffers[4][XFS_PATH_MAX];
static utils_file xfs_romfs_file = { 0 };
static bool xfs_romfs_loaded = false;

static const xfs_overlay_layer_config_t xfs_romfs_layer = {
    .engine = &xfs_romfs_engine,
    .hostname = "romfs",
    .path = "/",
};

static const xfs_overlay_layer_config_t xfs_ram_layer = {
    .engine = &xfs_ram_engine,
    .hostname = "ram",
    .path = "/",
};

xfs_romfs_config_t xfs_default_romfs = {
    .start = NULL,
    .end = NULL,
    .name = "romfs",
    /* Match the controller: ROMFS is the immutable system layer, not a
     * user-writable flash filesystem. Overlay policy distinguishes it. */
    .storage = FS_STORAGE_SYSTEM,
};

const xfs_overlay_config_t xfs_default_overlay = {
    .layers = { &xfs_ram_layer, &xfs_romfs_layer, NULL },
    .default_layer = &xfs_ram_layer,
};

static bool xfs_compat_load_romfs_file(const char* path)
{
    utils_file file = { 0 };
    if (utils_read_file(path, &file) != 0)
        return false;

    xfs_romfs_file = file;
    xfs_default_romfs.start = xfs_romfs_file.buffer;
    xfs_default_romfs.end = xfs_romfs_file.buffer + xfs_romfs_file.length;
    xfs_romfs_loaded = true;
    return true;
}

void xfs_compat_init(void)
{
    if (xfs_romfs_loaded)
        return;

    utils_file file = { 0 };
    if (utils_read_auxiliary_file("spxromfs.bin", &file, UTILS_AUXILIARY_ROM) == 0)
    {
        xfs_romfs_file = file;
        xfs_default_romfs.start = xfs_romfs_file.buffer;
        xfs_default_romfs.end = xfs_romfs_file.buffer + xfs_romfs_file.length;
        xfs_romfs_loaded = true;
    }
    else if (!xfs_compat_load_romfs_file("fuse/roms/spxromfs.bin") &&
        !xfs_compat_load_romfs_file("../roms/spxromfs.bin") &&
        !xfs_compat_load_romfs_file("roms/spxromfs.bin"))
    {
        XFS_DEBUG("xfs: spxromfs.bin not found; ROMFS overlay disabled\n");
    }
}

char* xfs_compat_get_cwd_buffer(uint8_t mount_point)
{
    if (mount_point >= 4)
    {
        return NULL;
    }

    return xfs_cwd_buffers[mount_point];
}

void xfs_debug_log(const char *format, ...)
{
    va_list args;
    va_list remote_args;
    char buffer[1024];

    va_start(args, format);
    va_copy(remote_args, args);
    vprintf(format, args);
    vsnprintf(buffer, sizeof(buffer), format, remote_args);
    va_end(remote_args);
    va_end(args);

    gdbserver_send_remote_console_output(buffer);
}
