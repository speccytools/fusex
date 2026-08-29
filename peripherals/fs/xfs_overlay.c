#include "xfs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define XFS_OVERLAY_MAX_READDIR_ENTRIES 256

typedef struct
{
    const xfs_overlay_layer_config_t* config;
    struct xfs_engine_mount_t mount;
} xfs_overlay_layer_t;

typedef struct
{
    xfs_overlay_layer_t layers[XFS_OVERLAY_MAX_LAYERS];
    uint8_t layer_count;
    xfs_overlay_layer_t* default_layer;
} xfs_overlay_mount_t;

typedef struct
{
    xfs_overlay_layer_t* layer;
    struct xfs_handle_t child;
} xfs_overlay_file_handle_t;

typedef struct
{
    xfs_overlay_mount_t* overlay;
    struct xfs_stat_info* entries;
    uint16_t entry_count;
    uint16_t position;
    bool parent_emitted;
} xfs_overlay_dir_handle_t;

static xfs_overlay_mount_t* overlay_mount_data(const struct xfs_engine_mount_t* mount)
{
    return (xfs_overlay_mount_t*)mount->mount_data;
}

static int overlay_layer_index_for_config(const xfs_overlay_mount_t* overlay,
    const xfs_overlay_layer_config_t* config)
{
    for (uint8_t i = 0; i < overlay->layer_count; ++i)
    {
        if (overlay->layers[i].config == config)
            return i;
    }
    return -1;
}

static int16_t overlay_join_path(char* out, size_t out_size, const char* dir, const char* name)
{
    const int written = (strcmp(dir, "/") == 0)
        ? snprintf(out, out_size, "/%s", name)
        : snprintf(out, out_size, "%s/%s", dir, name);
    if (written < 0 || (size_t)written >= out_size)
        return XFS_ERR_NAMETOOLONG;
    return XFS_ERR_OK;
}

static int16_t overlay_mount(const struct xfs_engine_t* engine, const char* hostname,
    const char* path, struct xfs_engine_mount_t* out_mount)
{
    (void)hostname;
    (void)path;
    const xfs_overlay_config_t* config = (const xfs_overlay_config_t*)engine->user;
    if (!config)
        return XFS_ERR_INVAL;

    xfs_overlay_mount_t* overlay = calloc(1, sizeof(*overlay));
    if (!overlay)
        return XFS_ERR_NOMEM;

    int16_t first_error = XFS_ERR_NOENT;
    for (uint8_t i = 0; config->layers[i] && i < XFS_OVERLAY_MAX_LAYERS; ++i)
    {
        const xfs_overlay_layer_config_t* layer_config = config->layers[i];
        xfs_overlay_layer_t* layer = &overlay->layers[overlay->layer_count];
        layer->config = layer_config;

        int16_t err = layer_config->engine->mount(layer_config->engine,
            layer_config->hostname, layer_config->path, &layer->mount);
        if (err == XFS_ERR_OK)
        {
            layer->mount.engine = layer_config->engine;
            overlay->layer_count++;
        }
        else if (first_error == XFS_ERR_NOENT)
        {
            first_error = err;
        }
    }

    if (overlay->layer_count == 0)
    {
        free(overlay);
        return first_error;
    }

    int default_layer = overlay_layer_index_for_config(overlay, config->default_layer);
    overlay->default_layer = &overlay->layers[default_layer >= 0 ? (uint8_t)default_layer : (overlay->layer_count - 1)];
    out_mount->mount_data = overlay;
    XFS_DEBUG("overlay: mount success layers=%u default=%u\n", overlay->layer_count, overlay->default_layer);
    return XFS_ERR_OK;
}

static uint8_t overlay_is_mounted(const struct xfs_engine_t* engine, struct xfs_engine_mount_t* mount)
{
    (void)engine;
    return mount->mount_data != NULL;
}

static void overlay_unmount(const struct xfs_engine_t* engine, struct xfs_engine_mount_t* mount)
{
    (void)engine;
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return;

    xfs_close_handles_for_mount(mount);
    for (uint8_t i = 0; i < overlay->layer_count; ++i)
    {
        xfs_overlay_layer_t* layer = &overlay->layers[i];
        if (layer->config->engine->unmount)
            layer->config->engine->unmount(layer->config->engine, &layer->mount);
    }
    free(overlay);
    mount->mount_data = NULL;
}

static void overlay_mount_info(const struct xfs_engine_mount_t* mount, char* buffer, size_t size)
{
    (void)mount;
    if (!buffer || size == 0)
        return;
    strncpy(buffer, "xfs://ram/", size - 1);
    buffer[size - 1] = '\0';
}

static bool overlay_path_exists(xfs_overlay_layer_t* layer, const char* path, struct xfs_stat_info* info)
{
    return layer->config->engine->stat(&layer->mount, path, info) == XFS_ERR_OK;
}

static bool overlay_is_system_dir(const xfs_overlay_mount_t* overlay, const char* path)
{
    if (overlay->layer_count == 0)
        return false;

    struct xfs_stat_info info;
    const xfs_overlay_layer_t* layer = &overlay->layers[overlay->layer_count - 1];
    return layer->config->engine->stat(&layer->mount, path, &info) == XFS_ERR_OK
        && info.type == XFS_TYPE_DIR
        && info.storage == FS_STORAGE_SYSTEM;
}

/* Shadow only an already-existing immediate parent; never create missing paths. */
static int16_t overlay_mkdir_shadow(xfs_overlay_mount_t* overlay, const char* path)
{
    const char* slash = strrchr(path, '/');
    if (!slash)
        return XFS_ERR_OK;
    if (slash == path)
        return XFS_ERR_OK;

    const size_t parent_len = (size_t)(slash - path);
    if (parent_len >= XFS_PATH_MAX)
        return XFS_ERR_NAMETOOLONG;

    char parent[XFS_PATH_MAX];
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';

    struct xfs_stat_info info;
    xfs_overlay_layer_t* first_layer = &overlay->layers[0];
    int16_t err = first_layer->config->engine->stat(&first_layer->mount, parent, &info);
    if (err == XFS_ERR_OK)
        return info.type == XFS_TYPE_DIR ? XFS_ERR_OK : XFS_ERR_NOTDIR;
    if (err != XFS_ERR_NOENT)
        return err;

    for (uint8_t i = 1; i < overlay->layer_count; ++i)
    {
        xfs_overlay_layer_t* layer = &overlay->layers[i];
        err = layer->config->engine->stat(&layer->mount, parent, &info);
        if (err == XFS_ERR_OK)
        {
            if (info.type != XFS_TYPE_DIR)
                return XFS_ERR_NOTDIR;

            err = first_layer->config->engine->mkdir(&first_layer->mount, parent);
            return err == XFS_ERR_EXIST ? XFS_ERR_OK : err;
        }
        if (err != XFS_ERR_NOENT)
            return err;
    }

    return XFS_ERR_NOENT;
}

static int16_t overlay_open(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    const char* path, int flags)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;

    if ((flags & XFS_O_CREAT) != 0)
    {
        // we always create on first layer
        xfs_overlay_layer_t* layer = &overlay->layers[0];
        const int16_t shadow_err = overlay_mkdir_shadow(overlay, path);
        if (shadow_err != XFS_ERR_OK)
            return shadow_err;

        xfs_overlay_file_handle_t* overlay_handle = calloc(1, sizeof(*overlay_handle));
        if (!overlay_handle)
            return XFS_ERR_NOMEM;
        overlay_handle->layer = layer;
        overlay_handle->child.type = XFS_HANDLE_TYPE_FILE;

        const int16_t err = layer->config->engine->open(&layer->mount, &overlay_handle->child, path, flags);
        if (err == XFS_ERR_OK)
        {
            handle->data = overlay_handle;
            return XFS_ERR_OK;
        }
        free(overlay_handle);
        return err;
    }

    struct xfs_stat_info stat_info;
    int16_t first_error = XFS_ERR_NOENT;

    for (uint8_t i = 0; i < overlay->layer_count; ++i)
    {
        xfs_overlay_layer_t* layer = &overlay->layers[i];
        const int16_t stat_err = layer->config->engine->stat(&layer->mount, path, &stat_info);
        if (stat_err == XFS_ERR_OK)
        {
            xfs_overlay_file_handle_t* overlay_handle = calloc(1, sizeof(*overlay_handle));
            if (!overlay_handle)
                return XFS_ERR_NOMEM;
            overlay_handle->layer = layer;
            overlay_handle->child.type = XFS_HANDLE_TYPE_FILE;

            const int16_t err = layer->config->engine->open(&layer->mount, &overlay_handle->child, path, flags);
            if (err == XFS_ERR_OK)
            {
                handle->data = overlay_handle;
                return XFS_ERR_OK;
            }
            free(overlay_handle);
            return err;
        }
        if (stat_err != XFS_ERR_NOENT && first_error == XFS_ERR_NOENT)
            first_error = stat_err;
    }

    xfs_overlay_layer_t* layer = overlay->default_layer;
    xfs_overlay_file_handle_t* overlay_handle = calloc(1, sizeof(*overlay_handle));
    if (!overlay_handle)
        return XFS_ERR_NOMEM;
    overlay_handle->layer = overlay->default_layer;
    overlay_handle->child.type = XFS_HANDLE_TYPE_FILE;

    const int16_t err = layer->config->engine->open(&layer->mount, &overlay_handle->child, path, flags);
    if (err == XFS_ERR_OK)
    {
        handle->data = overlay_handle;
        return XFS_ERR_OK;
    }
    free(overlay_handle);
    return err;
}

static int32_t overlay_read(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    void* buffer, uint32_t size)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    xfs_overlay_file_handle_t* overlay_handle = (xfs_overlay_file_handle_t*)handle->data;
    if (!overlay || !overlay_handle)
        return XFS_ERR_BADF;
    xfs_overlay_layer_t* layer = overlay_handle->layer;
    return layer->config->engine->read(&layer->mount, &overlay_handle->child, buffer, size);
}

static const uint8_t* overlay_direct_read(const struct xfs_engine_mount_t* mount,
    struct xfs_handle_t* handle, size_t* out_len)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    xfs_overlay_file_handle_t* overlay_handle = (xfs_overlay_file_handle_t*)handle->data;
    if (!overlay || !overlay_handle)
        return NULL;
    xfs_overlay_layer_t* layer = overlay_handle->layer;
    if (!layer->config->engine->direct_read)
        return NULL;
    return layer->config->engine->direct_read(&layer->mount, &overlay_handle->child, out_len);
}

static int32_t overlay_write(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    const void* buffer, uint32_t size)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    xfs_overlay_file_handle_t* overlay_handle = (xfs_overlay_file_handle_t*)handle->data;
    if (!overlay || !overlay_handle)
        return XFS_ERR_BADF;
    xfs_overlay_layer_t* layer = overlay_handle->layer;
    return layer->config->engine->write(&layer->mount, &overlay_handle->child, buffer, size);
}

static int16_t overlay_close(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle)
{
    xfs_overlay_file_handle_t* overlay_handle = (xfs_overlay_file_handle_t*)handle->data;
    if (!overlay_handle)
        return XFS_ERR_OK;
    xfs_overlay_layer_t* layer = overlay_handle->layer;
    const int16_t err = layer->config->engine->close(&layer->mount, &overlay_handle->child);
    if (err == XFS_ERR_OK && layer->config->engine->free_handle)
    {
        /* The child engine owns its handle wrapper; release it once the close succeeds. */
        layer->config->engine->free_handle(&layer->mount, &overlay_handle->child);
    }
    return err;
}

static int32_t overlay_lseek(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    int32_t offset, uint8_t whence)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    xfs_overlay_file_handle_t* overlay_handle = (xfs_overlay_file_handle_t*)handle->data;
    if (!overlay || !overlay_handle)
        return XFS_ERR_BADF;
    xfs_overlay_layer_t* layer = overlay_handle->layer;
    return layer->config->engine->lseek(&layer->mount, &overlay_handle->child, offset, whence);
}

static bool overlay_entry_shadowed(xfs_overlay_mount_t* overlay, const char* path,
    uint8_t layer_index, const char* name)
{
    if (strcmp(name, ".") == 0)
        return false;

    char child[XFS_PATH_MAX];
    if (overlay_join_path(child, sizeof(child), path, name) != XFS_ERR_OK)
        return false;

    struct xfs_stat_info info;
    for (uint8_t i = 0; i < layer_index; ++i)
    {
        if (overlay_path_exists(&overlay->layers[i], child, &info))
            return true;
    }
    return false;
}

static bool overlay_cached_dir_seen(const xfs_overlay_dir_handle_t* dir_handle, const char* name)
{
    for (uint16_t i = 0; i < dir_handle->entry_count; ++i)
    {
        if (dir_handle->entries[i].type == XFS_TYPE_DIR &&
            strcmp(dir_handle->entries[i].name, name) == 0)
            return true;
    }
    return false;
}

static int16_t overlay_cache_readdir_entry(xfs_overlay_dir_handle_t* dir_handle,
    const char* path, uint8_t layer_index, struct xfs_stat_info* info)
{
    if (strcmp(info->name, "..") == 0)
        return XFS_ERR_OK;
    if (info->type == XFS_TYPE_DIR && overlay_cached_dir_seen(dir_handle, info->name))
        return XFS_ERR_OK;
    if (overlay_entry_shadowed(dir_handle->overlay, path, layer_index, info->name))
        return XFS_ERR_OK;
    if (dir_handle->entry_count >= XFS_OVERLAY_MAX_READDIR_ENTRIES)
        return XFS_ERR_INVAL;

    if (info->type == XFS_TYPE_DIR)
    {
        char child[XFS_PATH_MAX];
        const int16_t err = overlay_join_path(child, sizeof(child), path, info->name);
        if (err != XFS_ERR_OK)
            return err;
        if (overlay_is_system_dir(dir_handle->overlay, child))
            info->storage = FS_STORAGE_SYSTEM;
    }

    dir_handle->entries[dir_handle->entry_count++] = *info;
    return XFS_ERR_OK;
}

static int16_t overlay_emit_parent(struct xfs_stat_info* info)
{
    memset(info, 0, sizeof(*info));
    info->type = XFS_TYPE_DIR;
    info->storage = FS_STORAGE_RAM;
    strncpy(info->name, "..", sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
    return 1;
}

static int16_t overlay_scan_dir(xfs_overlay_dir_handle_t* dir_handle, const char* path)
{
    bool opened_any = false;
    for (uint8_t layer_index = 0; layer_index < dir_handle->overlay->layer_count; ++layer_index)
    {
        xfs_overlay_layer_t* layer = &dir_handle->overlay->layers[layer_index];
        struct xfs_handle_t child = {.type = XFS_HANDLE_TYPE_DIR};
        int16_t err = layer->config->engine->opendir(&layer->mount, &child, path);
        if (err == XFS_ERR_NOENT)
            continue;
        if (err != XFS_ERR_OK)
            return err;
        opened_any = true;

        struct xfs_stat_info info;
        while ((err = layer->config->engine->readdir(&layer->mount, &child, &info)) > 0)
        {
            err = overlay_cache_readdir_entry(dir_handle, path, layer_index, &info);
            if (err != XFS_ERR_OK)
                break;
        }

        const int16_t close_err = layer->config->engine->closedir(&layer->mount, &child);
        if (close_err == XFS_ERR_OK && layer->config->engine->free_handle)
            layer->config->engine->free_handle(&layer->mount, &child);
        if (err < 0)
            return err;
        if (close_err != XFS_ERR_OK)
            return close_err;
    }
    return opened_any ? XFS_ERR_OK : XFS_ERR_NOENT;
}

static int16_t overlay_opendir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    const char* path)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;

    xfs_overlay_dir_handle_t* dir_handle = calloc(1, sizeof(*dir_handle));
    if (!dir_handle)
        return XFS_ERR_NOMEM;
    dir_handle->entries = calloc(XFS_OVERLAY_MAX_READDIR_ENTRIES, sizeof(*dir_handle->entries));
    if (!dir_handle->entries)
    {
        free(dir_handle);
        return XFS_ERR_NOMEM;
    }
    dir_handle->overlay = overlay;

    const int16_t err = overlay_scan_dir(dir_handle, path);
    if (err != XFS_ERR_OK)
    {
        free(dir_handle->entries);
        free(dir_handle);
        return err;
    }
    handle->data = dir_handle;
    handle->dir_position = 0;
    return XFS_ERR_OK;
}

static int16_t overlay_readdir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle,
    struct xfs_stat_info* info)
{
    (void)mount;
    xfs_overlay_dir_handle_t* dir_handle = (xfs_overlay_dir_handle_t*)handle->data;
    if (!dir_handle || !dir_handle->entries)
        return XFS_ERR_BADF;
    if (!dir_handle->parent_emitted)
    {
        dir_handle->parent_emitted = true;
        handle->dir_position = 0;
        return overlay_emit_parent(info);
    }
    if (dir_handle->position >= dir_handle->entry_count)
        return 0;

    *info = dir_handle->entries[dir_handle->position++];
    handle->dir_position = dir_handle->position;
    return 1;
}

static int16_t overlay_closedir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle)
{
    (void)mount;
    xfs_overlay_dir_handle_t* dir_handle = (xfs_overlay_dir_handle_t*)handle->data;
    if (!dir_handle)
        return XFS_ERR_OK;
    free(dir_handle->entries);
    dir_handle->entries = NULL;
    dir_handle->entry_count = 0;
    dir_handle->position = 0;
    dir_handle->parent_emitted = false;
    handle->dir_position = 0;
    return XFS_ERR_OK;
}

static int16_t overlay_telldir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle, uint32_t* position)
{
    (void)mount;
    xfs_overlay_dir_handle_t* dir_handle = (xfs_overlay_dir_handle_t*)handle->data;
    if (!dir_handle || !position)
        return XFS_ERR_BADF;
    *position = dir_handle->position;
    return XFS_ERR_OK;
}

static int16_t overlay_seekdir(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle, uint32_t position)
{
    (void)mount;
    xfs_overlay_dir_handle_t* dir_handle = (xfs_overlay_dir_handle_t*)handle->data;
    if (!dir_handle || !dir_handle->entries || position > dir_handle->entry_count)
        return XFS_ERR_INVAL;
    dir_handle->position = (uint16_t)position;
    dir_handle->parent_emitted = true;
    handle->dir_position = position;
    return XFS_ERR_OK;
}

static int16_t overlay_stat(const struct xfs_engine_mount_t* mount, const char* path,
    struct xfs_stat_info* stat_info)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;
    int16_t first_error = XFS_ERR_NOENT;
    for (uint8_t i = 0; i < overlay->layer_count; ++i)
    {
        int16_t err = overlay->layers[i].config->engine->stat(&overlay->layers[i].mount, path, stat_info);
        if (err == XFS_ERR_OK)
        {
            if (stat_info->type == XFS_TYPE_DIR)
            {
                if (overlay_is_system_dir(overlay, path))
                    stat_info->storage = FS_STORAGE_SYSTEM;
            }
            return XFS_ERR_OK;
        }
        if (err != XFS_ERR_NOENT && first_error == XFS_ERR_NOENT)
            first_error = err;
    }
    return first_error;
}

static int16_t overlay_unlink(const struct xfs_engine_mount_t* mount, const char* path)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;

    struct xfs_stat_info info;
    int16_t first_error = XFS_ERR_NOENT;
    for (uint8_t i = 0; i < overlay->layer_count; ++i)
    {
        xfs_overlay_layer_t* layer = &overlay->layers[i];
        int16_t err = layer->config->engine->stat(&layer->mount, path, &info);
        if (err == XFS_ERR_OK)
        {
            if (info.type == XFS_TYPE_DIR)
            {
                if (overlay_is_system_dir(overlay, path))
                    return XFS_ERR_INVAL;
            }
            return layer->config->engine->unlink(&layer->mount, path);
        }
        if (err != XFS_ERR_NOENT && first_error == XFS_ERR_NOENT)
            first_error = err;
    }
    return first_error;
}

static int16_t overlay_mkdir(const struct xfs_engine_mount_t* mount, const char* path)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;
    xfs_overlay_layer_t* layer = overlay->default_layer;
    return layer->config->engine->mkdir(&layer->mount, path);
}

static int16_t overlay_rmdir(const struct xfs_engine_mount_t* mount, const char* path)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;

    struct xfs_stat_info info;
    int16_t first_error = XFS_ERR_NOENT;
    for (uint8_t i = 0; i < overlay->layer_count; ++i)
    {
        xfs_overlay_layer_t* layer = &overlay->layers[i];
        int16_t err = layer->config->engine->stat(&layer->mount, path, &info);
        if (err == XFS_ERR_OK)
        {
            if (info.type == XFS_TYPE_DIR)
            {
                if (overlay_is_system_dir(overlay, path))
                    return XFS_ERR_INVAL;
            }
            return layer->config->engine->rmdir(&layer->mount, path);
        }
        if (err != XFS_ERR_NOENT && first_error == XFS_ERR_NOENT)
            first_error = err;
    }
    return first_error;
}

static int16_t overlay_chdir(const struct xfs_engine_mount_t* mount, const char* path)
{
    struct xfs_stat_info info;
    int16_t err = overlay_stat(mount, path, &info);
    if (err != XFS_ERR_OK || info.type != XFS_TYPE_DIR)
        return XFS_ERR_NOENT;
    return XFS_ERR_OK;
}

static int16_t overlay_getcwd(const struct xfs_engine_mount_t* mount, char* buffer, uint16_t size)
{
    (void)mount;
    if (size < 2)
        return XFS_ERR_INVAL;
    strncpy(buffer, "/", size - 1);
    buffer[size - 1] = '\0';
    return XFS_ERR_OK;
}

static int16_t overlay_rename(const struct xfs_engine_mount_t* mount, const char* old_path,
    const char* new_path)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;

    struct xfs_stat_info info;
    int16_t first_error = XFS_ERR_NOENT;
    for (uint8_t i = 0; i < overlay->layer_count; ++i)
    {
        xfs_overlay_layer_t* layer = &overlay->layers[i];
        int16_t err = layer->config->engine->stat(&layer->mount, old_path, &info);
        if (err == XFS_ERR_OK)
            return layer->config->engine->rename(&layer->mount, old_path, new_path);
        if (err != XFS_ERR_NOENT && first_error == XFS_ERR_NOENT)
            first_error = err;
    }
    return first_error;
}

static int16_t overlay_chmod(const struct xfs_engine_mount_t* mount, const char* path, uint16_t mode)
{
    xfs_overlay_mount_t* overlay = overlay_mount_data(mount);
    if (!overlay)
        return XFS_ERR_IO;
    xfs_overlay_layer_t* layer = overlay->default_layer;
    return layer->config->engine->chmod(&layer->mount, path, mode);
}

static void overlay_free_handle(const struct xfs_engine_mount_t* mount, struct xfs_handle_t* handle)
{
    if (!handle->data)
        return;
    if (handle->type == XFS_HANDLE_TYPE_FILE)
    {
        xfs_overlay_file_handle_t* overlay_handle = (xfs_overlay_file_handle_t*)handle->data;
        (void)overlay_close(mount, handle);
        free(overlay_handle);
    }
    else if (handle->type == XFS_HANDLE_TYPE_DIR)
    {
        xfs_overlay_dir_handle_t* dir_handle = (xfs_overlay_dir_handle_t*)handle->data;
        (void)overlay_closedir(mount, handle);
        free(dir_handle);
    }
    handle->data = NULL;
}

const struct xfs_engine_t xfs_overlay_engine = {
    .user = (void*)&xfs_default_overlay,
    .mount = overlay_mount,
    .is_mounted = overlay_is_mounted,
    .unmount = overlay_unmount,
    .mount_info = overlay_mount_info,
    .open = overlay_open,
    .read = overlay_read,
    .direct_read = overlay_direct_read,
    .write = overlay_write,
    .close = overlay_close,
    .lseek = overlay_lseek,
    .opendir = overlay_opendir,
    .readdir = overlay_readdir,
    .telldir = overlay_telldir,
    .seekdir = overlay_seekdir,
    .closedir = overlay_closedir,
    .stat = overlay_stat,
    .unlink = overlay_unlink,
    .mkdir = overlay_mkdir,
    .rmdir = overlay_rmdir,
    .chdir = overlay_chdir,
    .getcwd = overlay_getcwd,
    .rename = overlay_rename,
    .chmod = overlay_chmod,
    .free_handle = overlay_free_handle,
};
