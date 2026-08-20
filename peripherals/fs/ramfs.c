#include "ramfs.h"

#include <stdio.h>
#include <string.h>

static uint32_t read_u32(uint32_t value)
{
    return value;
}

static uint16_t read_u16(uint16_t value)
{
    return value;
}

static void normalize_path(const char* in, char* out, size_t out_size)
{
    if (!out || out_size == 0)
        return;

    if (!in || in[0] == '\0' || strcmp(in, "/") == 0)
    {
        strncpy(out, "/", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    size_t pos = 0;
    out[pos++] = '/';
    while (*in == '/')
        in++;
    while (*in && pos + 1 < out_size)
    {
        out[pos++] = *in++;
    }
    while (pos > 1 && out[pos - 1] == '/')
        pos--;
    out[pos] = '\0';
}

static uint8_t entry_full_path(const ramfs_entry_t* entry, char* out, size_t out_size)
{
    if (!entry || !out || out_size == 0)
        return 0;
    if (strncmp(entry->parent, "/", sizeof(entry->parent)) == 0)
    {
        int n = snprintf(out, out_size, "/%s", entry->name);
        return n > 0 && (size_t)n < out_size;
    }
    int n = snprintf(out, out_size, "%s/%s", entry->parent, entry->name);
    return n > 0 && (size_t)n < out_size;
}

uint8_t ramfs_mount(const uint8_t* start, const uint8_t* end, ramfs_t* fs)
{
    if (!start || !end || !fs || end < start || (size_t)(end - start) < sizeof(ramfs_header_t))
        return 0;

    memset(fs, 0, sizeof(*fs));
    fs->start = start;
    fs->size = (size_t)(end - start);
    fs->header = (const ramfs_header_t*)start;

    if (read_u32(fs->header->magic) != RAMFS_MAGIC ||
        read_u16(fs->header->version) != RAMFS_VERSION ||
        read_u16(fs->header->entry_size) != sizeof(ramfs_entry_t))
    {
        return 0;
    }

    const uint32_t entry_count = read_u32(fs->header->entry_count);
    const uint32_t data_offset = read_u32(fs->header->data_offset);
    const uint32_t data_size = read_u32(fs->header->data_size);
    const size_t table_end = sizeof(ramfs_header_t) + (size_t)entry_count * sizeof(ramfs_entry_t);
    if (table_end > fs->size || data_offset < table_end || (size_t)data_offset + data_size > fs->size)
        return 0;

    fs->entries = (const ramfs_entry_t*)(start + sizeof(ramfs_header_t));
    fs->data = start + data_offset;
    return 1;
}

const ramfs_entry_t* ramfs_find(const ramfs_t* fs, const char* path)
{
    if (!fs || !fs->entries)
        return NULL;

    char wanted[XFS_PATH_MAX];
    char current[XFS_PATH_MAX];
    normalize_path(path, wanted, sizeof(wanted));
    if (strcmp(wanted, "/") == 0)
        return NULL;

    for (uint32_t i = 0; i < read_u32(fs->header->entry_count); ++i)
    {
        if (!entry_full_path(&fs->entries[i], current, sizeof(current)))
            continue;
        if (strcmp(current, wanted) == 0)
            return &fs->entries[i];
    }
    return NULL;
}

uint8_t ramfs_opendir(const ramfs_t* fs, const char* path, ramfs_dir_t* dir)
{
    if (!fs || !fs->entries || !dir)
        return 0;

    normalize_path(path, dir->path, sizeof(dir->path));
    if (strcmp(dir->path, "/") != 0)
    {
        const ramfs_entry_t* entry = ramfs_find(fs, dir->path);
        if (!entry || entry->type != RAMFS_TYPE_DIR)
            return 0;
    }
    dir->index = 0;
    return 1;
}

const ramfs_entry_t* ramfs_readdir(const ramfs_t* fs, ramfs_dir_t* dir)
{
    if (!fs || !fs->entries || !dir)
        return NULL;

    const uint32_t count = read_u32(fs->header->entry_count);
    while (dir->index < count)
    {
        const ramfs_entry_t* entry = &fs->entries[dir->index++];
        if (strncmp(entry->parent, dir->path, sizeof(entry->parent)) == 0)
            return entry;
    }
    return NULL;
}

const uint8_t* ramfs_file_data(const ramfs_t* fs, const ramfs_entry_t* entry)
{
    if (!fs || !entry || entry->type != RAMFS_TYPE_REG)
        return NULL;

    const uint32_t offset = read_u32(entry->offset);
    const uint32_t size = read_u32(entry->size);
    if ((size_t)offset + size > read_u32(fs->header->data_size))
        return NULL;
    return fs->data + offset;
}
