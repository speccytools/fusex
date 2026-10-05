#pragma once

#include <stddef.h>
#include <stdint.h>

struct xfs_handle_t;
struct xfs_engine_mount_t;

/**
 * Parse "N:path" (explicit mount 0..3) or "path" (current default VFS mount).
 * On success, *path_out points into spec; the spec string must remain valid.
 */
int engine_fs_parse_mount_spec(const char *spec, int *mount_index_out, const char **path_out);

int engine_fs_ram_source_aliases_destination(int mount_index, const char *source_path,
                                             const char *destination_path);

int engine_fs_open_read(int mount_index, const char *path, struct xfs_handle_t *h,
                        struct xfs_engine_mount_t **mnt_out);

void engine_fs_close(struct xfs_engine_mount_t *mnt, struct xfs_handle_t *h);

/** Ensure the RAM-backed XFS engine is mounted; fills *ram (caller-owned storage). */
int engine_fs_ram_mount(struct xfs_engine_mount_t *ram);

/** Open a file on the RAM mount for write (create + truncate). */
int engine_fs_ram_open_write(struct xfs_engine_mount_t *ram, struct xfs_handle_t *h, const char *path);

/** Write a 16-bit little-endian length prefix and the string including its NUL. */
int engine_fs_write_le_string(struct xfs_engine_mount_t *mnt, struct xfs_handle_t *fh, const char *s);

/**
 * Read an entire file and NUL-terminate. On success sets *out and *out_len.
 * Reads sequentially into the platform's shared 256 KB engine buffer.
 * *out points to that buffer.
 * Returns -1 on error.
 */
int engine_fs_read_entire(struct xfs_engine_mount_t *mnt, struct xfs_handle_t *h, uint8_t **out, size_t *out_len);
