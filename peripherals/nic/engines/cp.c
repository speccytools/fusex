#include "engine.h"
#include "engine_fs.h"
#include "engine_compat.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CP_BUFFER_SIZE = 4096 };

static int parse_nonnegative(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !*text || *text == '-')
        return -1;
    errno = 0;
    const int base = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
    parsed = strtoul(text, &end, base);
    if (errno || *end || parsed > INT32_MAX)
        return -1;
    *value = (uint32_t)parsed;
    return 0;
}

int engine_cp_call(const char *input_file, const char *output_file, int argc, char *argv[])
{
    int source_index;
    const char *source_path;
    uint32_t offset = 0;
    uint32_t remaining = UINT32_MAX;
    struct xfs_handle_t source = {0}, destination = {0};
    struct xfs_engine_mount_t *source_mount = NULL;
    struct xfs_engine_mount_t ram = {0};
    int destination_open = 0;
    int result = -2;
    uint32_t copied = 0;
    uint8_t *buffer = engine_cp_input_buffer();

    if ((argc != 1 && argc != 3) ||
        engine_fs_parse_mount_spec(input_file, &source_index, &source_path) != 0 ||
        !output_file || !*output_file || strchr(output_file, ':'))
        return -3;
    if (argc == 3)
    {
        if (parse_nonnegative(argv[1], &offset) ||
            parse_nonnegative(argv[2], &remaining) ||
            remaining > INT32_MAX - offset)
            return -3;
    }
    if (engine_fs_ram_source_aliases_destination(source_index, source_path, output_file))
        return -3;
    if (engine_fs_open_read(source_index, source_path, &source, &source_mount))
    {
        ENGINE_CP_LOG("snx: cp source open failed: %s\n", input_file);
        return -2;
    }
    while (offset)
    {
        const uint32_t requested = offset < CP_BUFFER_SIZE ? offset : CP_BUFFER_SIZE;
        const int32_t count = source_mount->engine->read(source_mount, &source, buffer, requested);
        if (count <= 0) {
            ENGINE_CP_LOG("snx: cp skip failed: %s rc=%ld remaining=%lu\n",
                          input_file, (long)count, (unsigned long)offset);
            goto done;
        }
        offset -= (uint32_t)count;
    }
    if (engine_fs_ram_mount(&ram) ||
        engine_fs_ram_open_write(&ram, &destination, output_file))
    {
        ENGINE_CP_LOG("snx: cp destination open failed: %s\n", output_file);
        goto done;
    }
    destination_open = 1;
    for (;;)
    {
        const uint32_t requested = remaining < CP_BUFFER_SIZE ? remaining : CP_BUFFER_SIZE;
        if (!requested)
        {
            result = 0;
            break;
        }
        const int32_t count = source_mount->engine->read(source_mount, &source, buffer, requested);
        if (count < 0)
        {
            ENGINE_CP_LOG("snx: cp source read failed: %s rc=%ld copied=%lu\n",
                          input_file, (long)count, (unsigned long)copied);
            break;
        }
        if (count == 0)
        {
            result = argc == 1 ? 0 : -2;
            break;
        }
        const int32_t written = ram.engine->write(&ram, &destination, buffer, (uint32_t)count);
        if (written != count)
        {
            ENGINE_CP_LOG("snx: cp destination write failed: %s rc=%ld copied=%lu\n",
                          output_file, (long)written, (unsigned long)copied);
            break;
        }
        copied += (uint32_t)count;
        if (argc == 3)
            remaining -= (uint32_t)count;
    }
done:
    if (destination_open)
    {
        if (ram.engine->close(&ram, &destination) != XFS_ERR_OK)
            result = -2;
        ram.engine->free_handle(&ram, &destination);
    }
    engine_fs_close(source_mount, &source);
    if (ram.mount_data)
        ram.engine->unmount(ram.engine, &ram);
    return result;
}

int engine_rm_call(const char *input_file, const char *output_file, int argc, char *argv[])
{
    (void)output_file;
    (void)argv;
    if (argc != 1 || !input_file || !*input_file || strchr(input_file, ':'))
        return -3;

    struct xfs_engine_mount_t ram = {0};
    if (engine_fs_ram_mount(&ram))
        return -2;
    const int16_t removed = ram.engine->unlink(&ram, input_file);
    ram.engine->unmount(ram.engine, &ram);
    return removed == XFS_ERR_OK || removed == XFS_ERR_NOENT ? 0 : -2;
}
