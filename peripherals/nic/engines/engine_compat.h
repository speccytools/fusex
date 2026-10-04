#pragma once

#include "../../fs/xfs.h"
#include "../../fs/xfs_engines.h"
#include "../../../../common/badapple_lz4.h"
#include "../../spectranet.h"
#include "../spectranext.h"
#include "parson.h"

#include <stdint.h>
#include <stdlib.h>

#define ENGINE_FS_READ_BUF_SIZE (256u * 1024u)

static inline int engine_default_mount_index(void)
{
    const uint8_t *ram = spectranet_ram_page(0xC0);
    return ram ? ram[0xF6F] : -1;
}

static inline const struct xfs_engine_t *engine_ram_destination(void)
{
    return &xfs_ram_engine;
}

static inline int engine_prepare_ram_mount(struct xfs_engine_mount_t *ram)
{
    static struct xfs_engine_mount_t singleton;
    static int ready;
    if (!ram) return -1;
    if (!ready) {
        singleton.engine = engine_ram_destination();
        if (singleton.engine->mount(singleton.engine, "ram", "/", NULL, NULL,
                                    &singleton) != XFS_ERR_OK) return -1;
        ready = 1;
    }
    *ram = singleton;
    return 0;
}

static inline uint8_t *engine_read_buffer(void)
{
    static uint8_t buffer[ENGINE_FS_READ_BUF_SIZE];
    return buffer;
}

static inline uint8_t *engine_cp_input_buffer(void)
{
    static uint8_t buffer[4096];
    return buffer;
}

static inline void *engine_lz4_alloc(size_t size)
{
    return malloc(size);
}

static inline void engine_lz4_free(void *ptr)
{
    free(ptr);
}

static inline void engine_json_allocators_begin(void) {}
static inline void engine_json_allocators_end(void) {}
