#include "xfs.h"
#include "ramfs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    ramfs_t fs;
    const xfs_romfs_config_t* config;
} xfs_romfs_mount_t;

typedef struct
{
    const ramfs_entry_t* entry;
    uint32_t offset;
} xfs_romfs_file_handle_t;

typedef struct
{
    ramfs_dir_t dir;
} xfs_romfs_dir_handle_t;

static xfs_romfs_mount_t* romfs_mount_data(const struct xfs_engine_mount_t* mount)
{
    return (xfs_romfs_mount_t*)mount->mount_data;
}

static int16_t romfs_mount(const struct xfs_engine_t* engine, const char* hostname,
    const char* path, struct xfs_engine_mount_t* out_mount)
{
    (void)hostname;
    (void)path;
    const xfs_romfs_config_t* config = (const xfs_romfs_config_t*)engine->user;
    if (!config || !config->start || !config->end)
        return XFS_ERR_INVAL;

    xfs_romfs_mount_t* mount = calloc(1, sizeof(*mount));
    if (!mount)
        return XFS_ERR_NOMEM;

    if (!ramfs_mount(config->start, config->end, &mount->fs))
    {
        free(mount);
        return XFS_ERR_CORRUPT;
    }

    mount->config = config;
    out_mount->mount_data = mount;
    XFS_DEBUG("romfs: mount success entries=%lu\n", (unsigned long)mount->fs.header->entry_count);
    return XFS_ERR_OK;
}

static uint8_t romfs_is_mounted(const struct xfs_engine_t* engine, struct xfs_engine_mount_t* mount)
{
    (void)engine;
    return mount->mount_data != NULL;
}

static void romfs_unmount(const struct xfs_engine_t* engine, struct xfs_engine_mount_t* mount)
{
    (void)engine;
    xfs_close_handles_for_mount(mount);
    free(mount->mount_data);
    mount->mount_data = NULL;
}

static void romfs_mount_info(const struct xfs_engine_mount_t* mount, char* buffer, size_t size)
{
    if (!buffer || size == 0)
        return;
    const xfs_romfs_mount_t* romfs = romfs_mount_data(mount);
    const char* name = romfs && romfs->config && romfs->config->name ? romfs->config->name : "romfs";
    snprintf(buffer, size, "xfs://%s/", name);
    buffer[size - 1] = '\0';
}

static int16_t romfs_open(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    const char* path, int flags)
{
    xfs_romfs_mount_t* romfs = romfs_mount_data(mount);
    if (!romfs)
        return XFS_ERR_IO;
    const int accmode = flags & XFS_O_RDWR;
    if (accmode == XFS_O_WRONLY || accmode == XFS_O_RDWR || (flags & XFS_O_CREAT) ||
        (flags & XFS_O_TRUNC) || (flags & XFS_O_APPEND))
    {
        return -100;
    }

    const ramfs_entry_t* entry = ramfs_find(&romfs->fs, path);
    if (!entry)
        return XFS_ERR_NOENT;
    if (entry->type == RAMFS_TYPE_DIR)
        return XFS_ERR_ISDIR;

    xfs_romfs_file_handle_t* file = calloc(1, sizeof(*file));
    if (!file)
        return XFS_ERR_NOMEM;
    file->entry = entry;
    handle->data = file;
    return XFS_ERR_OK;
}

static int32_t romfs_read(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    void* buffer, uint32_t size)
{
    xfs_romfs_mount_t* romfs = romfs_mount_data(mount);
    xfs_romfs_file_handle_t* file = (xfs_romfs_file_handle_t*)handle->data;
    if (!romfs || !file || !file->entry)
        return XFS_ERR_BADF;

    const uint8_t* data = ramfs_file_data(&romfs->fs, file->entry);
    if (!data)
        return XFS_ERR_IO;

    uint32_t available = file->entry->size > file->offset ? file->entry->size - file->offset : 0;
    if (size > available)
        size = available;
    memcpy(buffer, data + file->offset, size);
    file->offset += size;
    return (int32_t)size;
}

static const uint8_t* romfs_direct_read(const struct xfs_engine_mount_t* mount,
    struct xfs_handle_t* handle, size_t* out_len)
{
    xfs_romfs_mount_t* romfs = romfs_mount_data(mount);
    xfs_romfs_file_handle_t* file = (xfs_romfs_file_handle_t*)handle->data;
    if (!romfs || !file || !file->entry || !out_len)
        return NULL;

    const uint8_t* data = ramfs_file_data(&romfs->fs, file->entry);
    if (!data)
        return NULL;
    *out_len = file->entry->size;
    return data;
}

static int32_t romfs_write(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    const void* buffer, uint32_t size)
{
    (void)mount;
    (void)handle;
    (void)buffer;
    (void)size;
    return XFS_ERR_INVAL;
}

static int16_t romfs_close(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle)
{
    (void)mount;
    (void)handle;
    return XFS_ERR_OK;
}

static int32_t romfs_lseek(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    int32_t offset, uint8_t whence)
{
    (void)mount;
    xfs_romfs_file_handle_t* file = (xfs_romfs_file_handle_t*)handle->data;
    if (!file || !file->entry)
        return XFS_ERR_BADF;

    int32_t next;
    switch (whence)
    {
        case XFS_SEEK_SET:
            next = offset;
            break;
        case XFS_SEEK_CUR:
            next = (int32_t)file->offset + offset;
            break;
        case XFS_SEEK_END:
            next = (int32_t)file->entry->size + offset;
            break;
        default:
            return XFS_ERR_INVAL;
    }

    if (next < 0)
        return XFS_ERR_INVAL;
    if ((uint32_t)next > file->entry->size)
        next = (int32_t)file->entry->size;
    file->offset = (uint32_t)next;
    return next;
}

static int16_t romfs_opendir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    const char* path)
{
    xfs_romfs_mount_t* romfs = romfs_mount_data(mount);
    if (!romfs)
        return XFS_ERR_IO;

    xfs_romfs_dir_handle_t* dir = calloc(1, sizeof(*dir));
    if (!dir)
        return XFS_ERR_NOMEM;
    if (!ramfs_opendir(&romfs->fs, path, &dir->dir))
    {
        free(dir);
        return XFS_ERR_NOENT;
    }
    handle->data = dir;
    return XFS_ERR_OK;
}

static int16_t romfs_readdir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    struct xfs_stat_info* info)
{
    xfs_romfs_mount_t* romfs = romfs_mount_data(mount);
    xfs_romfs_dir_handle_t* dir = (xfs_romfs_dir_handle_t*)handle->data;
    if (!romfs || !dir)
        return XFS_ERR_BADF;

    const ramfs_entry_t* entry = ramfs_readdir(&romfs->fs, &dir->dir);
    if (!entry)
        return 0;

    memset(info, 0, sizeof(*info));
    info->type = entry->type == RAMFS_TYPE_DIR ? XFS_TYPE_DIR : XFS_TYPE_REG;
    info->storage = romfs->config ? romfs->config->storage : FS_STORAGE_FLASH;
    info->size = entry->size;
    info->mtime = entry->mtime;
    info->ctime = entry->mtime;
    strncpy(info->name, entry->name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    return 1;
}

static int16_t romfs_closedir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle)
{
    (void)mount;
    (void)handle;
    return XFS_ERR_OK;
}

static int16_t romfs_stat(const struct xfs_engine_mount_t* mount, const char* path,
    struct xfs_stat_info* stat_info)
{
    xfs_romfs_mount_t* romfs = romfs_mount_data(mount);
    if (!romfs)
        return XFS_ERR_IO;

    if (!path || path[0] == '\0' || strcmp(path, "/") == 0)
    {
        memset(stat_info, 0, sizeof(*stat_info));
        stat_info->type = XFS_TYPE_DIR;
        stat_info->storage = romfs->config ? romfs->config->storage : FS_STORAGE_FLASH;
        strncpy(stat_info->name, "/", sizeof(stat_info->name) - 1);
        return XFS_ERR_OK;
    }

    const ramfs_entry_t* entry = ramfs_find(&romfs->fs, path);
    if (!entry)
        return XFS_ERR_NOENT;

    memset(stat_info, 0, sizeof(*stat_info));
    stat_info->type = entry->type == RAMFS_TYPE_DIR ? XFS_TYPE_DIR : XFS_TYPE_REG;
    stat_info->storage = romfs->config ? romfs->config->storage : FS_STORAGE_FLASH;
    stat_info->size = entry->size;
    stat_info->mtime = entry->mtime;
    stat_info->ctime = entry->mtime;
    strncpy(stat_info->name, entry->name, sizeof(stat_info->name) - 1);
    stat_info->name[sizeof(stat_info->name) - 1] = '\0';
    return XFS_ERR_OK;
}

static int16_t romfs_unlink(const struct xfs_engine_mount_t* mount, const char* path)
{
    (void)mount;
    (void)path;
    return XFS_ERR_INVAL;
}

static int16_t romfs_mkdir(const struct xfs_engine_mount_t* mount, const char* path)
{
    (void)mount;
    (void)path;
    return XFS_ERR_INVAL;
}

static int16_t romfs_rmdir(const struct xfs_engine_mount_t* mount, const char* path)
{
    (void)mount;
    (void)path;
    return XFS_ERR_INVAL;
}

static int16_t romfs_chdir(const struct xfs_engine_mount_t* mount, const char* path)
{
    struct xfs_stat_info info;
    int16_t err = romfs_stat(mount, path, &info);
    if (err != XFS_ERR_OK || info.type != XFS_TYPE_DIR)
        return XFS_ERR_NOENT;
    return XFS_ERR_OK;
}

static int16_t romfs_getcwd(const struct xfs_engine_mount_t* mount, char* buffer, uint16_t size)
{
    (void)mount;
    if (size < 2)
        return XFS_ERR_INVAL;
    strncpy(buffer, "/", size - 1);
    buffer[size - 1] = '\0';
    return XFS_ERR_OK;
}

static int16_t romfs_rename(const struct xfs_engine_mount_t* mount, const char* old_path,
    const char* new_path)
{
    (void)mount;
    (void)old_path;
    (void)new_path;
    return XFS_ERR_INVAL;
}

static int16_t romfs_chmod(const struct xfs_engine_mount_t* mount, const char* path, uint16_t mode)
{
    (void)mount;
    (void)path;
    (void)mode;
    return XFS_ERR_INVAL;
}

static void romfs_free_handle(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle)
{
    (void)mount;
    free(handle->data);
    handle->data = NULL;
}

const struct xfs_engine_t xfs_romfs_engine = {
    .user = (void*)&xfs_default_romfs,
    .mount = romfs_mount,
    .is_mounted = romfs_is_mounted,
    .unmount = romfs_unmount,
    .mount_info = romfs_mount_info,
    .open = romfs_open,
    .read = romfs_read,
    .direct_read = romfs_direct_read,
    .write = romfs_write,
    .close = romfs_close,
    .lseek = romfs_lseek,
    .opendir = romfs_opendir,
    .readdir = romfs_readdir,
    .closedir = romfs_closedir,
    .stat = romfs_stat,
    .unlink = romfs_unlink,
    .mkdir = romfs_mkdir,
    .rmdir = romfs_rmdir,
    .chdir = romfs_chdir,
    .getcwd = romfs_getcwd,
    .rename = romfs_rename,
    .chmod = romfs_chmod,
    .free_handle = romfs_free_handle,
};
