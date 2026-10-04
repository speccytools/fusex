#include "engine.h"
#include "engine_compat.h"
#include "engine_fs.h"
#include "engine_utf8.h"
#include "jsonpath/matcher.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

enum
{
    JSON_ENGINE_ERR_OPEN = -2,
    JSON_ENGINE_ERR_PARSE = -3,
    JSON_ENGINE_ERR_JSONPATH = -4,
    JSON_ENGINE_ERR_NON_SCALAR = -5,
};

static int scalar_value_to_string(const JSON_Value *v, char *buf, size_t buf_sz)
{
    switch (json_value_get_type(v))
    {
        case JSONString:
            snprintf(buf, buf_sz, "%s", json_value_get_string(v));
            engine_utf8_fold(buf, buf_sz);
            return 0;
        case JSONNumber: {
            const double d = json_value_get_number(v);
            if (!isfinite(d))
                return -1;
            /* Whole numbers: use integer formatting (avoids %g and matches typical JSON IDs). */
            if (d >= (double)INT64_MIN && d <= (double)INT64_MAX)
            {
                const int64_t iv = (int64_t)d;
                if ((double)iv == d)
                {
                    snprintf(buf, buf_sz, "%" PRId64, (int64_t)iv);
                    engine_utf8_fold(buf, buf_sz);
                    return 0;
                }
            }
            /* Requires -Wl,-u,_printf_float (see CMakeLists): nano %g truncates exponents otherwise. */
            snprintf(buf, buf_sz, "%.17g", d);
            engine_utf8_fold(buf, buf_sz);
            return 0;
        }
        case JSONBoolean:
            snprintf(buf, buf_sz, "%s", json_value_get_boolean(v) ? "true" : "false");
            engine_utf8_fold(buf, buf_sz);
            return 0;
        case JSONNull:
            snprintf(buf, buf_sz, "null");
            engine_utf8_fold(buf, buf_sz);
            return 0;
        default:
            return -1;
    }
}

struct collect_ctx
{
    JSON_Value **items;
    size_t count;
    size_t cap;
    int non_scalar;
};

static void collect_cb(JSON_Value *res, void *priv)
{
    struct collect_ctx *ctx = (struct collect_ctx *)priv;
    const JSON_Value_Type t = json_value_get_type(res);
    if (t == JSONArray || t == JSONObject)
    {
        ctx->non_scalar = 1;
        return;
    }

    if (ctx->count >= ctx->cap)
    {
        const size_t ncap = ctx->cap ? ctx->cap * 2u : 16u;
        JSON_Value **nitems = (JSON_Value **)realloc(ctx->items, ncap * sizeof(JSON_Value *));
        if (!nitems)
            return;
        ctx->items = nitems;
        ctx->cap = ncap;
    }
    ctx->items[ctx->count++] = res;
}

static void eval_path_collect(JSON_Value *root, struct jp_opcode *op, struct collect_ctx *ctx)
{
    ctx->items = NULL;
    ctx->count = 0;
    ctx->cap = 0;
    ctx->non_scalar = 0;
    (void)jp_match(op, root, collect_cb, ctx);
}

int engine_json_call(const char *input_file, const char *output_file, int argc, char *argv[])
{
    int rc = 0;
    if (argc < 2)
    {
        SNX_CTRL_DEBUG("snx: json: invalid argc=%d\n", argc);
        return JSON_ENGINE_ERR_JSONPATH;
    }

    SNX_CTRL_DEBUG("snx: json: start in='%s' out='%s' paths=%d\n", input_file, output_file, argc - 1);

    int mount_index = 0;
    const char *path_in = NULL;
    if (engine_fs_parse_mount_spec(input_file, &mount_index, &path_in) != 0)
    {
        SNX_CTRL_DEBUG("snx: json: mount spec parse failed for '%s'\n", input_file);
        return JSON_ENGINE_ERR_OPEN;
    }

    struct xfs_handle_t in_h = {};
    struct xfs_engine_mount_t *in_mnt = NULL;
    uint8_t *buf = NULL;
    size_t raw_len = 0;
    JSON_Value *root = NULL;
    struct xfs_engine_mount_t ram = {0};
    struct xfs_handle_t out_h = {};
    char *val_buf = NULL;
    int out_open = 0;
    int parson_psram_heap_allocators = 0;

    SNX_CTRL_DEBUG("snx: using mount %d\n", mount_index);

    if (engine_fs_open_read(mount_index, path_in, &in_h, &in_mnt) != 0)
    {
        SNX_CTRL_DEBUG("snx: json: open read failed mount=%d path='%s'\n", mount_index, path_in);
        return JSON_ENGINE_ERR_OPEN;
    }

    SNX_CTRL_DEBUG("snx: open ok, reading..\n");

    if (engine_fs_read_entire(in_mnt, &in_h, &buf, &raw_len) != 0)
    {
        SNX_CTRL_DEBUG("snx: json: read failed path='%s'\n", path_in);
        rc = JSON_ENGINE_ERR_OPEN;
        goto cleanup;
    }

    SNX_CTRL_DEBUG("snx: read %zu bytes, parsing json...\n", raw_len);

    engine_json_allocators_begin();
    parson_psram_heap_allocators = 1;

    root = json_parse_string((char *)buf);

    if (!root)
    {
        SNX_CTRL_DEBUG("snx: json: parse failed bytes=%zu path='%s'\n", raw_len, path_in);
        rc = JSON_ENGINE_ERR_PARSE;
        goto cleanup;
    }

    SNX_CTRL_DEBUG("snx: parse ok\n");

    if (engine_fs_ram_mount(&ram) != 0)
    {
        SNX_CTRL_DEBUG("snx: json: ram mount failed\n");
        rc = JSON_ENGINE_ERR_OPEN;
        goto cleanup;
    }

    SNX_CTRL_DEBUG("snx: writing output...\n");

    if (engine_fs_ram_open_write(&ram, &out_h, output_file) != 0)
    {
        SNX_CTRL_DEBUG("snx: json: open write failed out='%s'\n", output_file);
        rc = JSON_ENGINE_ERR_OPEN;
        goto cleanup;
    }
    out_open = 1;

    char count_buf[32];
    val_buf = (char *)malloc(512u);
    if (!val_buf)
    {
        SNX_CTRL_DEBUG("snx: json: val buf alloc failed\n");
        rc = JSON_ENGINE_ERR_OPEN;
        goto cleanup;
    }

    for (int pi = 1; pi < argc; pi++)
    {
        SNX_CTRL_DEBUG("snx: json: path[%d]='%s'\n", pi - 1, argv[pi]);
        struct jp_state *st = jp_parse(argv[pi]);
        if (!st || st->error_code != 0 || !st->path)
        {
            SNX_CTRL_DEBUG("snx: json: path[%d] parse failed err=%d\n", pi - 1, st ? st->error_code : -1);
            if (st)
                jp_free(st);
            rc = JSON_ENGINE_ERR_JSONPATH;
            goto cleanup;
        }

        struct collect_ctx ctx;
        eval_path_collect(root, st->path, &ctx);
        jp_free(st);

        SNX_CTRL_DEBUG("snx: json: path[%d] matched=%zu non_scalar=%d\n", pi - 1, ctx.count, ctx.non_scalar);

        if (ctx.non_scalar)
        {
            SNX_CTRL_DEBUG("snx: json: path[%d] has non-scalar match\n", pi - 1);
            free(ctx.items);
            rc = JSON_ENGINE_ERR_NON_SCALAR;
            goto cleanup;
        }

        snprintf(count_buf, sizeof(count_buf), "%zu", ctx.count);
        if (engine_fs_write_le_string(&ram, &out_h, count_buf) != 0)
        {
            SNX_CTRL_DEBUG("snx: json: path[%d] write count failed count=%zu\n", pi - 1, ctx.count);
            free(ctx.items);
            rc = JSON_ENGINE_ERR_OPEN;
            goto cleanup;
        }

        for (size_t i = 0; i < ctx.count; i++)
        {
            if (scalar_value_to_string(ctx.items[i], val_buf, 512u) != 0)
            {
                SNX_CTRL_DEBUG("snx: json: path[%d] item[%zu] stringify failed\n", pi - 1, i);
                free(ctx.items);
                rc = JSON_ENGINE_ERR_NON_SCALAR;
                goto cleanup;
            }
            if (engine_fs_write_le_string(&ram, &out_h, val_buf) != 0)
            {
                SNX_CTRL_DEBUG("snx: json: path[%d] item[%zu] write failed\n", pi - 1, i);
                free(ctx.items);
                rc = JSON_ENGINE_ERR_OPEN;
                goto cleanup;
            }
        }

        free(ctx.items);
    }

    SNX_CTRL_DEBUG("snx: json: done paths=%d\n", argc - 1);
cleanup:
    if (val_buf)
        free(val_buf);
    if (out_open)
        engine_fs_close(&ram, &out_h);
    if (root)
        json_value_free(root);
    if (in_mnt)
        engine_fs_close(in_mnt, &in_h);
    if (parson_psram_heap_allocators)
        engine_json_allocators_end();
    /* buf: HTTPS blob (freed on close) or PSRAM.engine_fs_read_buf — do not free. */
    return rc;
}
