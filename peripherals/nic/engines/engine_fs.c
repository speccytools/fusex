#include "engine_fs.h"

#include "../../fs/xfs.h"
#include "../../fs/xfs_engines.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t engine_fs_read_buf[ENGINE_FS_READ_BUF_SIZE];

int engine_fs_parse_mount_spec(const char *spec, int *mount_index_out, const char **path_out)
{
    if (!spec || !mount_index_out || !path_out)
        return -1;

    const char *p = spec;
    while (*p == ' ')
        p++;

    if (!isdigit((unsigned char)*p))
        return -1;

    int idx = *p++ - '0';
    if (isdigit((unsigned char)*p))
        return -1;
    if (*p != ':')
        return -1;
    p++;
    if (*p == '\0')
        return -1;

    if (idx < 0 || idx >= 4)
        return -1;

    *mount_index_out = idx;
    *path_out = p;
    return 0;
}

int engine_fs_open_read(int mount_index, const char *path, struct xfs_handle_t *h,
                        struct xfs_engine_mount_t **mnt_out)
{
    if (mount_index < 0 || mount_index >= 4 || !path || !h || !mnt_out)
        return -1;

    struct xfs_engine_mount_t *m = &xfs_mounted_engines[mount_index];
    if (m->engine == NULL)
        return -1;

    memset(h, 0, sizeof(*h));
    h->type = XFS_HANDLE_TYPE_FILE;

    const int16_t e = m->engine->open(m, h, path, XFS_O_RDONLY);
    if (e != XFS_ERR_OK)
        return -1;

    *mnt_out = m;
    return 0;
}

void engine_fs_close(struct xfs_engine_mount_t *mnt, struct xfs_handle_t *h)
{
    if (mnt && mnt->engine)
    {
        mnt->engine->close(mnt, h);
        mnt->engine->free_handle(mnt, h);
    }
    memset(h, 0, sizeof(*h));
}

int engine_fs_ram_mount(struct xfs_engine_mount_t *ram)
{
    static struct xfs_engine_mount_t ram_singleton;
    static int ram_ready;

    if (!ram_ready)
    {
        memset(&ram_singleton, 0, sizeof(ram_singleton));
        ram_singleton.engine = &xfs_ram_engine;
        if (xfs_ram_engine.mount(&xfs_ram_engine, "ram", "/", NULL, NULL,
                &ram_singleton) != XFS_ERR_OK)
            return -1;
        ram_ready = 1;
    }
    *ram = ram_singleton;
    return 0;
}

int engine_fs_ram_open_write(struct xfs_engine_mount_t *ram, struct xfs_handle_t *h, const char *path)
{
    if (!ram || !ram->engine || !h || !path)
        return -1;
    memset(h, 0, sizeof(*h));
    h->type = XFS_HANDLE_TYPE_FILE;
    return ram->engine->open(ram, h, path, XFS_O_WRONLY | XFS_O_CREAT | XFS_O_TRUNC) == XFS_ERR_OK ? 0 : -1;
}

int engine_fs_write_le_string(struct xfs_engine_mount_t *mnt, struct xfs_handle_t *fh, const char *s)
{
    const size_t slen = strlen(s) + 1u;
    if (slen > 65535u)
        return -1;

    const uint16_t le = (uint16_t)slen;
    uint8_t hdr[2] = {(uint8_t)(le & 0xFFu), (uint8_t)(le >> 8)};

    if (mnt->engine->write(mnt, fh, hdr, 2) != 2)
        return -1;
    if (mnt->engine->write(mnt, fh, s, (uint16_t)slen) != (int16_t)slen)
        return -1;
    return 0;
}

int engine_fs_read_entire(struct xfs_engine_mount_t *mnt, struct xfs_handle_t *h, uint8_t **out, size_t *out_len)
{
    if (!mnt || !mnt->engine || !h || !out || !out_len)
        return -1;

    const size_t max_payload = ENGINE_FS_READ_BUF_SIZE - 1u;
    const int16_t n = mnt->engine->read(mnt, h, engine_fs_read_buf, (uint16_t)(max_payload > 65535u ? 65535u : max_payload));
    if (n < 0)
        return -1;
    if ((size_t)n >= max_payload)
        return -1;

    const size_t total = (size_t)n;
    engine_fs_read_buf[total] = '\0';
    *out = engine_fs_read_buf;
    *out_len = total;
    return 0;
}
