#pragma once

#include "../../fs/xfs.h"
#include "../../fs/xfs_engines.h"
#include "../../spectranet.h"
#include "../spectranext.h"
#include "parson.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define ENGINE_CP_LOG(...) fprintf(stderr, __VA_ARGS__)

#define ENGINE_FS_READ_BUF_SIZE (256u * 1024u)

static inline int engine_default_mount_index(void)
{
    return spectranext_controller_default_mount();
}

static inline const struct xfs_engine_t *engine_ram_destination(void)
{
    return &xfs_ram_engine;
}

static inline int engine_prepare_ram_mount(struct xfs_engine_mount_t *ram)
{
    if (!ram) return -1;
    if (ram->mount_data) return 0;
    ram->engine = engine_ram_destination();
    return ram->engine->mount(ram->engine, "ram", "/", NULL, NULL,
                              ram) == XFS_ERR_OK ? 0 : -1;
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

static inline void engine_json_allocators_begin(void) {}
static inline void engine_json_allocators_end(void) {}
