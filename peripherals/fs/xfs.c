#include "xfs.h"
#include "xfs_engines.h"
#include <stdio.h>
#include <string.h>

// Mounted engines array - shared between task and emulator
struct xfs_engine_mount_t xfs_mounted_engines[4] = {0};

// Handles array - shared between task and emulator  
struct xfs_handle_t xfs_handles[XFS_MAX_FDS] = {};

// Helper functions
static inline struct xfs_handle_t* get_handle(uint8_t handle)
{
    if (handle < 1 || handle > XFS_MAX_FDS)
        return NULL;
    return &xfs_handles[handle - 1];
}

static inline uint8_t allocate_handle(enum xfs_handle_type_t type)
{
    for (uint8_t i = 0; i < XFS_MAX_FDS; i++)
    {
        if (xfs_handles[i].type == XFS_HANDLE_TYPE_NONE)
        {
            xfs_handles[i].type = type;
            xfs_handles[i].owner_mount = 0xFF;
            xfs_handles[i].data = NULL;
            return i + 1;
        }
    }
    return 0;
}

static inline void free_handle(const uint8_t handle, const uint8_t mount_point)
{
    struct xfs_handle_t* h = get_handle(handle);
    if (h)
    {
        const struct xfs_engine_t* current_engine = xfs_mounted_engines[mount_point].engine;
        if (current_engine && current_engine->free_handle)
        {
            current_engine->free_handle(&xfs_mounted_engines[mount_point], h);
        }
        h->type = XFS_HANDLE_TYPE_NONE;
        h->owner_mount = 0xFF;
        h->data = NULL;
    }
}

static inline bool ensure_mounted(const uint8_t mount_point)
{
    return (xfs_mounted_engines[mount_point].engine != NULL);
}

static void xfs_set_root_cwd(struct xfs_engine_mount_t* mount)
{
    if (!mount->cwd)
    {
        return;
    }
    mount->cwd[0] = '/';
    mount->cwd[1] = '\0';
}

static int16_t xfs_resolve_path(const struct xfs_engine_mount_t* mount, const char* path,
    char* out, const size_t out_size)
{
    if (!path || !out || out_size < 2)
    {
        return XFS_ERR_INVAL;
    }

    const char* cwd = (mount && mount->cwd && mount->cwd[0]) ? mount->cwd : "/";

    char combined[XFS_PATH_MAX];
    int written;
    if (path[0] == '/')
    {
        written = snprintf(combined, sizeof(combined), "%s", path);
    }
    else if (cwd[0] == '/' && cwd[1] == '\0')
    {
        written = snprintf(combined, sizeof(combined), "/%s", path);
    }
    else
    {
        written = snprintf(combined, sizeof(combined), "%s/%s", cwd, path);
    }
    if (written < 0 || (size_t)written >= sizeof(combined))
    {
        return XFS_ERR_NAMETOOLONG;
    }

    size_t out_len = 0;
    out[out_len++] = '/';
    out[out_len] = '\0';

    char* segment = combined;
    while (*segment)
    {
        while (*segment == '/')
        {
            segment++;
        }
        if (!*segment)
        {
            break;
        }

        char* next = segment;
        while (*next && *next != '/')
        {
            next++;
        }

        const size_t len = (size_t)(next - segment);
        if (len == 1 && segment[0] == '.')
        {
            /* Stay in the current directory. */
        }
        else if (len == 2 && segment[0] == '.' && segment[1] == '.')
        {
            if (out_len > 1)
            {
                out_len--;
                while (out_len > 1 && out[out_len - 1] != '/')
                {
                    out_len--;
                }
                out[out_len] = '\0';
            }
        }
        else
        {
            if (out_len > 1)
            {
                if (out_len + 1 >= out_size)
                {
                    return XFS_ERR_NAMETOOLONG;
                }
                out[out_len++] = '/';
            }
            if (out_len + len >= out_size)
            {
                return XFS_ERR_NAMETOOLONG;
            }
            memcpy(out + out_len, segment, len);
            out_len += len;
            out[out_len] = '\0';
        }

        segment = next;
    }

    return XFS_ERR_OK;
}

/**
 * Handle XFS mount command
 */
void xfs_handle_mount(volatile struct xfs_registers_t* registers)
{
    const char* protocol = (const char*)registers->arguments.mount.protocol;
    const char* hostname = (const char*)registers->arguments.mount.hostname;
    const char* path = (const char*)registers->arguments.mount.path;
    const int mount_point = registers->mount_point;

    if (mount_point < 0 || mount_point >= 4)
    {
        XFS_DEBUG("xfs: mount failed: invalid mount_point=%d\n", mount_point);
        registers->result = XFS_ERR_INVAL;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    if (xfs_mounted_engines[mount_point].engine != NULL)
    {
        XFS_DEBUG("xfs: mount failed: mount_point=%d already mounted\n", mount_point);
        registers->result = XFS_ERR_EXIST;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    if (!protocol || !hostname || !path)
    {
        XFS_DEBUG("xfs: mount failed: null pointer\n");
        registers->result = XFS_ERR_INVAL;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const struct xfs_engine_t* engine = NULL;

    if (strcmp(protocol, "xfs") == 0)
    {
        XFS_DEBUG("xfs: mount hostname='%s' path='%s' mount_point=%d\n", hostname, path, mount_point);

        if (strcmp(hostname, "ram") == 0 && (path[0] == '\0' || strcmp(path, "/") == 0))
        {
            engine = &xfs_overlay_engine;
        }
        else
        {
            XFS_DEBUG("xfs: mount failed: invalid ram mount target\n");
            registers->result = XFS_ERR_INVAL;
            registers->status = XFS_STATUS_ERROR;
            return;
        }
    }
    else if (strcmp(protocol, "https") == 0)
    {
        XFS_DEBUG("xfs: mount https hostname='%s' path='%s' mount_point=%d\n", hostname, path, mount_point);
        engine = &https_engine;
    }
    else if (strcmp(protocol, "http") == 0)
    {
        XFS_DEBUG("xfs: mount http hostname='%s' path='%s' mount_point=%d\n", hostname, path, mount_point);
        engine = &http_engine;
    }
    else
    {
        XFS_DEBUG("xfs: mount failed: unknown protocol '%s' mount_point=%d\n", protocol, mount_point);
        registers->result = XFS_ERR_INVAL;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const int16_t mount_result = engine->mount(engine, hostname, path, &xfs_mounted_engines[mount_point]);
    if (mount_result != XFS_ERR_OK)
    {
        XFS_DEBUG("xfs: mount failed: result=%d mount_point=%d\n", mount_result, mount_point);
        registers->result = mount_result;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        xfs_mounted_engines[mount_point].engine = engine;
        xfs_mounted_engines[mount_point].cwd = xfs_compat_get_cwd_buffer(mount_point);
        xfs_set_root_cwd(&xfs_mounted_engines[mount_point]);
        XFS_DEBUG("xfs: mount success mount_point=%d\n", mount_point);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_close_handles_for_mount(const struct xfs_engine_mount_t *mount)
{
    if (!mount)
        return;

    int mp = -1;
    for (int i = 0; i < 4; i++)
    {
        if (&xfs_mounted_engines[i] == mount)
        {
            mp = i;
            break;
        }
    }
    if (mp < 0)
        return;

    const uint8_t mpu = (uint8_t)mp;
    for (uint8_t hi = 1; hi <= XFS_MAX_FDS; hi++)
    {
        struct xfs_handle_t *h = get_handle(hi);
        if (h && h->type != XFS_HANDLE_TYPE_NONE && h->owner_mount == mpu)
            free_handle(hi, mpu);
    }
}

void xfs_handle_umount(volatile struct xfs_registers_t* registers)
{
    const int mount_point = registers->mount_point;

    if (mount_point < 0 || mount_point >= 4)
    {
        XFS_DEBUG("xfs: umount failed: invalid mount_point=%d\n", mount_point);
        registers->result = XFS_ERR_INVAL;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    if (xfs_mounted_engines[mount_point].engine == NULL)
    {
        XFS_DEBUG("xfs: umount mount_point=%d (already idle)\n", mount_point);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
        return;
    }

    const struct xfs_engine_t* const eng = xfs_mounted_engines[mount_point].engine;
    if (eng->unmount)
        eng->unmount(eng, &xfs_mounted_engines[mount_point]);

    xfs_mounted_engines[mount_point].engine = NULL;
    xfs_mounted_engines[mount_point].cwd = NULL;

    XFS_DEBUG("xfs: umount success mount_point=%d\n", mount_point);
    registers->result = 0;
    registers->status = XFS_STATUS_COMPLETE;
}

void xfs_handle_mount_info(volatile struct xfs_registers_t* registers)
{
    const int mount_point = registers->mount_point;

    if (mount_point < 0 || mount_point >= 4)
    {
        XFS_DEBUG("xfs: mount_info failed: invalid mount_point=%d\n", mount_point);
        registers->result = XFS_ERR_INVAL;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    struct xfs_engine_mount_t* mount = &xfs_mounted_engines[mount_point];
    if (mount->engine == NULL)
    {
        XFS_DEBUG("xfs: mount_info failed: not mounted mount_point=%d\n", mount_point);
        registers->result = XFS_ERR_NOENT;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    char* const out = (char*)registers->workspace;
    out[0] = '\0';

    if (mount->engine->mount_info)
    {
        mount->engine->mount_info(mount, out, 128);
    }
    else
    {
        strncpy(out, "XFS", 127);
        out[127] = '\0';
    }

    XFS_DEBUG("xfs: mount_info mount_point=%d value='%s'\n", mount_point, out);
    registers->result = 0;
    registers->status = XFS_STATUS_COMPLETE;
}

void xfs_handle_open(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.open.path;
    const uint16_t flags = registers->arguments.open.flags;
    const uint16_t mode = registers->arguments.open.mode;
    const uint8_t mount_point = registers->mount_point;
    XFS_DEBUG("xfs: open path=%s flags=0x%04x mode=0x%04x mount_point=%d\n", path, flags, mode, mount_point);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: open failed: not mounted mount_point=%d\n", mount_point);
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const uint8_t handle = allocate_handle(XFS_HANDLE_TYPE_FILE);
    if (handle == 0)
    {
        XFS_DEBUG("xfs: open failed: too many open files\n");
        registers->result = XFS_ERR_NOMEM;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    int xfs_flags = 0;
    const uint8_t accmode = flags & 0x0003;
    if (accmode == 0x0001)
        xfs_flags = XFS_O_RDONLY;
    else if (accmode == 0x0002)
        xfs_flags = XFS_O_WRONLY;
    else if (accmode == 0x0003)
        xfs_flags = XFS_O_RDWR;
    else
        xfs_flags = XFS_O_RDONLY;

    if (flags & 0x0008)
        xfs_flags |= XFS_O_APPEND;
    if (flags & 0x0100)
        xfs_flags |= XFS_O_CREAT;
    if (flags & 0x0200)
        xfs_flags |= XFS_O_CREAT | XFS_O_TRUNC;

    struct xfs_handle_t* h = get_handle(handle);
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: open path resolution failed: result=%d\n", err);
        free_handle(handle, mount_point);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }
    
    err = mounted_engine->engine->open(mounted_engine, h, resolved_path, xfs_flags);

    if (err)
    {
        XFS_DEBUG("xfs: open failed: result=%d\n", err);
        free_handle(handle, mount_point);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        h->owner_mount = mount_point;
        XFS_DEBUG("xfs: open success path=%s handle=%d\n", resolved_path, handle);
        registers->file_handle = handle;
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}
void xfs_handle_read(volatile struct xfs_registers_t* registers)
{
    const uint8_t handle = registers->file_handle;
    const uint16_t size = registers->arguments.read.size;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: read handle=%d size=%d remaining=%d total=%d\n", handle, size,
        registers->fops.remaining, registers->fops.total);
    
    struct xfs_handle_t* h = get_handle(handle);
    if (h == NULL || h->type != XFS_HANDLE_TYPE_FILE)
    {
        XFS_DEBUG("xfs: read failed: invalid handle\n");
        registers->result = XFS_ERR_BADF;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const int16_t bytes_read = mounted_engine->engine->read(mounted_engine, h,
        (uint8_t*)registers->workspace, size);
    
    if (bytes_read < 0)
    {
        XFS_DEBUG("xfs: read failed: result=%d\n", (int)bytes_read);
        registers->result = bytes_read;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: read success bytes=%d\n", (int)bytes_read);
        registers->result = bytes_read;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_write(volatile struct xfs_registers_t* registers)
{
    const uint8_t handle = registers->file_handle;
    const uint16_t size = registers->arguments.write.size;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];

    XFS_DEBUG("xfs: write handle=%d size=%d remaining=%d total=%d\n", handle, size,
        registers->fops.remaining, registers->fops.total);

    struct xfs_handle_t* h = get_handle(handle);
    if (h == NULL || h->type != XFS_HANDLE_TYPE_FILE)
    {
        XFS_DEBUG("xfs: write failed: invalid handle\n");
        registers->result = XFS_ERR_BADF;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const int16_t bytes_written = mounted_engine->engine->write(mounted_engine, h,
        (uint8_t*)registers->workspace, size);
    
    if (bytes_written < 0)
    {
        XFS_DEBUG("xfs: write failed: result=%d\n", (int)bytes_written);
        registers->result = bytes_written;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: write success bytes=%d\n", (int)bytes_written);
        registers->result = bytes_written;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_close(volatile struct xfs_registers_t* registers)
{
    const uint8_t handle = registers->file_handle;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: close handle=%d mount_point=%d\n", handle, mount_point);
    struct xfs_handle_t* h = get_handle(handle);
    
    if (h == NULL)
    {
        XFS_DEBUG("xfs: close failed: invalid handle\n");
        registers->result = XFS_ERR_BADF;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    int16_t err = XFS_ERR_OK;
    if (h->type == XFS_HANDLE_TYPE_FILE)
    {
        err = mounted_engine->engine->close(mounted_engine, h);
    }
    else if (h->type == XFS_HANDLE_TYPE_DIR)
    {
        err = mounted_engine->engine->closedir(mounted_engine, h);
    }

    free_handle(handle, mount_point);
    
    if (err)
    {
        XFS_DEBUG("xfs: close failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: close success\n");
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_opendir(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.opendir.path;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: opendir path=%s mount_point=%d\n", path, mount_point);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: opendir failed: not mounted mount_point=%d\n", mount_point);
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const uint8_t handle = allocate_handle(XFS_HANDLE_TYPE_DIR);
    if (handle == 0)
    {
        XFS_DEBUG("xfs: opendir failed: too many open files mount_point=%d\n", mount_point);
        registers->result = XFS_ERR_NOMEM;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    struct xfs_handle_t* h = get_handle(handle);
    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: opendir path resolution failed: result=%d mount_point=%d\n", err, mount_point);
        free_handle(handle, mount_point);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }
    err = mounted_engine->engine->opendir(mounted_engine, h, resolved_path);

    if (err)
    {
        XFS_DEBUG("xfs: opendir %s failed: result=%d mount_point=%d\n", resolved_path, err, mount_point);
        free_handle(handle, mount_point);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        h->owner_mount = mount_point;
        XFS_DEBUG("xfs: opendir %s success handle=%d mount_point=%d\n", resolved_path, handle, mount_point);
        registers->result = 0;
        registers->file_handle = handle;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_readdir(volatile struct xfs_registers_t* registers)
{
    const uint8_t handle = registers->file_handle;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: readdir handle=%d\n", handle);
    struct xfs_handle_t* h = get_handle(handle);
    
    if (h == NULL || h->type != XFS_HANDLE_TYPE_DIR)
    {
        XFS_DEBUG("xfs: readdir failed: invalid handle\n");
        registers->result = XFS_ERR_BADF;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    struct xfs_stat_info info = {0};
    const int16_t err = mounted_engine->engine->readdir(mounted_engine, h, &info);
    
    if (err < 0)
    {
        XFS_DEBUG("xfs: readdir failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        if (err == 0)
        {
            XFS_DEBUG("xfs: readdir eod\n");
            /* Z80 F_readdir treats empty workspace as EOD; clear before result so a torn
             * 16-bit read of result cannot pair stale filename bytes with a bogus "success". */
            registers->workspace[0] = '\0';
            registers->result = 1;
        }
        else
        {
            XFS_DEBUG("xfs: readdir success name=%s\n", info.name);
            strcpy((char*)registers->workspace, info.name);
            volatile uint8_t* tail = registers->workspace + strlen(info.name) + 1;
            tail[0] = 'X';
            tail[1] = info.type;
            tail[2] = (uint8_t)(info.size & 0xff);
            tail[3] = (uint8_t)((info.size >> 8) & 0xff);
            tail[4] = (uint8_t)((info.size >> 16) & 0xff);
            tail[5] = (uint8_t)((info.size >> 24) & 0xff);
            tail[6] = (uint8_t)(info.mtime & 0xff);
            tail[7] = (uint8_t)((info.mtime >> 8) & 0xff);
            tail[8] = (uint8_t)((info.mtime >> 16) & 0xff);
            tail[9] = (uint8_t)((info.mtime >> 24) & 0xff);
            registers->result = 0;
        }
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_closedir(volatile struct xfs_registers_t* registers)
{
    uint8_t handle = registers->file_handle;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: closedir handle=%d\n", handle);
    struct xfs_handle_t* h = get_handle(handle);
    
    if (h == NULL || h->type != XFS_HANDLE_TYPE_DIR)
    {
        XFS_DEBUG("xfs: closedir failed: invalid handle\n");
        registers->result = XFS_ERR_BADF;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const int16_t err = mounted_engine->engine->closedir(mounted_engine, h);
    free_handle(handle, mount_point);
    
    if (err)
    {
        XFS_DEBUG("xfs: closedir failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: closedir success\n");
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_stat(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.stat.path;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: stat path=%s\n", path);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: stat failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }
    
    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: stat path resolution failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    struct xfs_stat_info info = {0};
    err = mounted_engine->engine->stat(mounted_engine, resolved_path, &info);
    
    if (err)
    {
        XFS_DEBUG("xfs: stat failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        struct xfs_stat_t* stat = (struct xfs_stat_t*)registers->workspace;
        stat->mode = 0x0644;
        if (info.type == XFS_TYPE_DIR)
        {
            stat->mode |= 0x4000;
        }
        else
        {
            stat->mode |= 0x8000;
        }
        stat->uid = 0;
        stat->gid = 0;
        stat->size = (uint32_t)info.size;
        stat->atime = info.atime;
        stat->mtime = info.mtime;
        stat->ctime = info.ctime;
        uint8_t* strings = (uint8_t*)registers->workspace + 22;
        strings[0] = 0;
        strings[1] = 0;
        XFS_DEBUG("xfs: stat success path=%s size=%lu mode=0x%04x\n", resolved_path, info.size, stat->mode);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_unlink(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.unlink.path;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: unlink path=%s\n", path);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: unlink failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: unlink path resolution failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    err = mounted_engine->engine->unlink(mounted_engine, resolved_path);
    
    if (err)
    {
        XFS_DEBUG("xfs: unlink failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: unlink success path=%s\n", resolved_path);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_mkdir(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.mkdir.path;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: mkdir path=%s\n", path);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: mkdir failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: mkdir path resolution failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    err = mounted_engine->engine->mkdir(mounted_engine, resolved_path);
    
    if (err)
    {
        XFS_DEBUG("xfs: mkdir failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: mkdir success path=%s\n", resolved_path);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_rmdir(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.rmdir.path;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: rmdir path=%s\n", path);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: rmdir failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: rmdir path resolution failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    err = mounted_engine->engine->rmdir(mounted_engine, resolved_path);
    
    if (err)
    {
        XFS_DEBUG("xfs: rmdir failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: rmdir success path=%s\n", resolved_path);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_chdir(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.chdir.path;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];
    XFS_DEBUG("xfs: chdir path=%s\n", path);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: chdir failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: chdir path resolution failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    struct xfs_stat_info info;
    if (strcmp(resolved_path, "/") == 0)
    {
        memset(&info, 0, sizeof(info));
        info.type = XFS_TYPE_DIR;
        err = XFS_ERR_OK;
    }
    else
    {
        err = mounted_engine->engine->stat(mounted_engine, resolved_path, &info);
    }
    
    if (err || info.type != XFS_TYPE_DIR)
    {
        XFS_DEBUG("xfs: chdir failed: path not found or not a directory\n");
        registers->result = XFS_ERR_NOENT;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        snprintf(xfs_mounted_engines[mount_point].cwd, XFS_PATH_MAX, "%s", resolved_path);
        XFS_DEBUG("xfs: chdir success cwd=%s\n", xfs_mounted_engines[mount_point].cwd);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_getcwd(volatile struct xfs_registers_t* registers)
{
    const uint8_t mount_point = registers->mount_point;
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: getcwd failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }
    
    char* path = (char*)registers->workspace;
    uint16_t len = registers->arguments.getcwd.buffer_size;

    if (len == 0)
    {
        registers->result = XFS_ERR_INVAL;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    const char* cwd = xfs_mounted_engines[mount_point].cwd ? xfs_mounted_engines[mount_point].cwd : "/";
    snprintf(path, len, "%s", cwd);
    XFS_DEBUG("xfs: getcwd success path=%s\n", path);
    registers->result = 0;
    registers->status = XFS_STATUS_COMPLETE;
}

void xfs_handle_rename(volatile struct xfs_registers_t* registers)
{
    const char* old_path = (const char*)registers->arguments.rename.old_path;
    const char* new_path = (const char*)registers->arguments.rename.new_path;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];

    XFS_DEBUG("xfs: rename old=%s new=%s\n", old_path, new_path);
    
    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: rename failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    char resolved_old_path[XFS_PATH_MAX];
    char resolved_new_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, old_path, resolved_old_path, sizeof(resolved_old_path));
    if (!err)
    {
        err = xfs_resolve_path(mounted_engine, new_path, resolved_new_path, sizeof(resolved_new_path));
    }
    if (err)
    {
        XFS_DEBUG("xfs: rename path resolution failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    err = mounted_engine->engine->rename(mounted_engine, resolved_old_path, resolved_new_path);
    
    if (err)
    {
        XFS_DEBUG("xfs: rename failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: rename success old=%s new=%s\n", resolved_old_path, resolved_new_path);
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_chmod(volatile struct xfs_registers_t* registers)
{
    const char* path = (const char*)registers->arguments.chmod.path;
    const uint16_t mode = registers->arguments.chmod.mode;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];

    XFS_DEBUG("xfs: chmod path=%s mode=0x%04x\n", path, mode);

    if (!ensure_mounted(mount_point))
    {
        XFS_DEBUG("xfs: chmod failed: not mounted\n");
        registers->result = XFS_ERR_IO;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    if (!mounted_engine->engine->chmod)
    {
        XFS_DEBUG("xfs: chmod failed: not supported\n");
        registers->result = XFS_ERR_INVAL;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    char resolved_path[XFS_PATH_MAX];
    int16_t err = xfs_resolve_path(mounted_engine, path, resolved_path, sizeof(resolved_path));
    if (err)
    {
        XFS_DEBUG("xfs: chmod path resolution failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    err = mounted_engine->engine->chmod(mounted_engine, resolved_path, mode);

    if (err)
    {
        XFS_DEBUG("xfs: chmod failed: result=%d\n", err);
        registers->result = err;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: chmod success\n");
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

void xfs_handle_lseek(volatile struct xfs_registers_t* registers)
{
    uint8_t handle = registers->file_handle;
    int32_t offset = registers->arguments.lseek.offset;
    uint8_t whence = registers->arguments.lseek.whence;
    const uint8_t mount_point = registers->mount_point;
    const struct xfs_engine_mount_t* mounted_engine = &xfs_mounted_engines[mount_point];

    XFS_DEBUG("xfs: lseek handle=%d offset=%ld whence=%d\n", handle, (long)offset, whence);
    
    struct xfs_handle_t* h = get_handle(handle);
    if (h == NULL || h->type != XFS_HANDLE_TYPE_FILE)
    {
        XFS_DEBUG("xfs: lseek failed: invalid handle\n");
        registers->result = XFS_ERR_BADF;
        registers->status = XFS_STATUS_ERROR;
        return;
    }

    int32_t new_pos = mounted_engine->engine->lseek(mounted_engine, h, offset, whence);

    if (new_pos < 0)
    {
        XFS_DEBUG("xfs: lseek failed: result=%ld\n", (long)new_pos);
        registers->result = new_pos;
        registers->status = XFS_STATUS_ERROR;
    }
    else
    {
        XFS_DEBUG("xfs: lseek success new_pos=%lu\n", (unsigned long)new_pos);
        *(uint32_t*)&registers->workspace[0] = (uint32_t)new_pos;
        registers->result = 0;
        registers->status = XFS_STATUS_COMPLETE;
    }
}

/**
 * Handle XFS command dispatch
 * This function processes a command from the registers and dispatches to the appropriate handler
 * FreeRTOS-independent, usable in emulator
 */
void xfs_handle_command(volatile struct xfs_registers_t* registers)
{
    const uint8_t command = registers->command;

    if (command)
    {
        registers->command = 0;
        registers->result = 0;
        registers->status = XFS_STATUS_BUSY;

        switch (command)
        {
            case XFS_CMD_MOUNT:
            {
                xfs_handle_mount(registers);
                break;
            }
            case XFS_CMD_OPEN:
            {
                xfs_handle_open(registers);
                break;
            }
            case XFS_CMD_READ:
            {
                xfs_handle_read(registers);
                break;
            }
            case XFS_CMD_WRITE:
            {
                xfs_handle_write(registers);
                break;
            }
            case XFS_CMD_CLOSE:
            {
                xfs_handle_close(registers);
                break;
            }
            case XFS_CMD_OPENDIR:
            {
                xfs_handle_opendir(registers);
                break;
            }
            case XFS_CMD_READDIR:
            {
                xfs_handle_readdir(registers);
                break;
            }
            case XFS_CMD_CLOSEDIR:
            {
                xfs_handle_closedir(registers);
                break;
            }
            case XFS_CMD_STAT:
            {
                xfs_handle_stat(registers);
                break;
            }
            case XFS_CMD_UNLINK:
            {
                xfs_handle_unlink(registers);
                break;
            }
            case XFS_CMD_MKDIR:
            {
                xfs_handle_mkdir(registers);
                break;
            }
            case XFS_CMD_RMDIR:
            {
                xfs_handle_rmdir(registers);
                break;
            }
            case XFS_CMD_CHDIR:
            {
                xfs_handle_chdir(registers);
                break;
            }
            case XFS_CMD_GETCWD:
            {
                xfs_handle_getcwd(registers);
                break;
            }
            case XFS_CMD_RENAME:
            {
                xfs_handle_rename(registers);
                break;
            }
            case XFS_CMD_LSEEK:
            {
                xfs_handle_lseek(registers);
                break;
            }
            case XFS_CMD_UNMOUNT:
            {
                xfs_handle_umount(registers);
                break;
            }
            case XFS_CMD_MOUNT_INFO:
            {
                xfs_handle_mount_info(registers);
                break;
            }
            case XFS_CMD_CHMOD:
            {
                xfs_handle_chmod(registers);
                break;
            }
            default:
            {
                XFS_DEBUG("xfs: unknown command=%d\n", command);
                registers->result = XFS_ERR_INVAL;
                registers->status = XFS_STATUS_ERROR;
                break;
            }
        }
    }
}

/**
 * Free all XFS resources (handles and mounts)
 * This function is FreeRTOS-independent and can be used in an emulator
 */
void xfs_free(void)
{
    XFS_DEBUG("xfs: free - cleaning up all mounts and handles\n");
    
    // unmount all engines to clean up engine-specific mount resources
    for (uint8_t mount_point = 0; mount_point < 4; mount_point++)
    {
        if (xfs_mounted_engines[mount_point].engine != NULL)
        {
            XFS_DEBUG("xfs: free - unmounting engine at mount_point=%d\n", mount_point);
            
            // Call engine's unmount function to clean up engine-specific resources
            if (xfs_mounted_engines[mount_point].engine->unmount)
            {
                xfs_mounted_engines[mount_point].engine->unmount(
                    xfs_mounted_engines[mount_point].engine, &xfs_mounted_engines[mount_point]);
            }
            
            xfs_mounted_engines[mount_point].engine = NULL;
        }
    }
    
    // Clear xfs_handles array (should already be cleared, but be explicit)
    memset(&xfs_handles[0], 0, sizeof(xfs_handles));
}
