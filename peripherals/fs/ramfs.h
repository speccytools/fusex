#pragma once

#include <stddef.h>
#include <stdint.h>

#include "xfs.h"

#define RAMFS_MAGIC 0x46524e53u
#define RAMFS_VERSION 1
#define RAMFS_NAME_MAX 64
#define RAMFS_PARENT_MAX 128

enum ramfs_entry_type
{
    RAMFS_TYPE_REG = 1,
    RAMFS_TYPE_DIR = 2,
};

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t entry_size;
    uint32_t entry_count;
    uint32_t data_offset;
    uint32_t data_size;
} ramfs_header_t;

typedef struct __attribute__((packed))
{
    char parent[RAMFS_PARENT_MAX];
    char name[RAMFS_NAME_MAX];
    uint8_t type;
    uint8_t reserved[3];
    uint32_t offset;
    uint32_t size;
    uint32_t mtime;
} ramfs_entry_t;

typedef struct
{
    const uint8_t* start;
    size_t size;
    const ramfs_header_t* header;
    const ramfs_entry_t* entries;
    const uint8_t* data;
} ramfs_t;

typedef struct
{
    uint32_t index;
    char path[XFS_PATH_MAX];
} ramfs_dir_t;

uint8_t ramfs_mount(const uint8_t* start, const uint8_t* end, ramfs_t* fs);
const ramfs_entry_t* ramfs_find(const ramfs_t* fs, const char* path);
uint8_t ramfs_opendir(const ramfs_t* fs, const char* path, ramfs_dir_t* dir);
const ramfs_entry_t* ramfs_readdir(const ramfs_t* fs, ramfs_dir_t* dir);
const uint8_t* ramfs_file_data(const ramfs_t* fs, const ramfs_entry_t* entry);
