#include "engine_fs.h"
#include "engine_compat.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int engine_fs_parse_mount_spec(const char *spec, int *mount_index_out, const char **path_out)
{
    if (!spec || !mount_index_out || !path_out)
        return -1;

    const char *p = spec;
    while (*p == ' ')
        p++;

    int idx;
    if (isdigit((unsigned char)p[0]) && p[1] == ':')
    {
        idx = *p - '0';
        p += 2;
    }
    else
    {
        /* Spectranet's default VFS mount lives in permanent RAM at $3F6F. */
        idx = engine_default_mount_index();
    }
    if (*p == '\0')
        return -1;

    if (idx < 0 || idx >= 4)
        return -1;

    *mount_index_out = idx;
    *path_out = p;
    return 0;
}

int engine_fs_ram_source_aliases_destination(int mount_index, const char *source_path,
                                             const char *destination_path)
{
    if (mount_index < 0 || mount_index >= 4 || !source_path || !destination_path)
        return 0;
    const struct xfs_engine_t *engine = xfs_mounted_engines[mount_index].engine;
    if (engine != &xfs_overlay_engine && engine != &xfs_ram_engine)
        return 0;
    while (*source_path == '/') source_path++;
    while (*destination_path == '/') destination_path++;
    return strcmp(source_path, destination_path) == 0;
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
    return engine_prepare_ram_mount(ram);
}

int engine_fs_ram_open_write(struct xfs_engine_mount_t *ram, struct xfs_handle_t *h, const char *path)
{
    if (!ram || !ram->engine || !h || !path)
        return -1;
    /* Reclaim the old file before opening a truncating writer. */
    const int16_t removed = ram->engine->unlink(ram, path);
    if (removed != XFS_ERR_OK && removed != XFS_ERR_NOENT)
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

    uint8_t *buf = engine_read_buffer();
    const size_t max_payload = ENGINE_FS_READ_BUF_SIZE - 1u;
    size_t used = 0;
    while (used < max_payload)
    {
        size_t requested = max_payload - used;
        if (requested > 65535u) requested = 65535u;
        const int32_t n = mnt->engine->read(mnt, h, buf + used, (uint32_t)requested);
        if (n < 0) return -1;
        if (n == 0) break;
        used += (size_t)n;
    }
    if (used == max_payload) return -1;

    *out = buf;
    *out_len = used;
    buf[*out_len] = '\0';
    return 0;
}
