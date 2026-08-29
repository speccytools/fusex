#include "config.h"

#include "xfs_https_compat.h"

#include <string.h>

#include "libspectrum.h"
#include "../http/httpc.h"
#include "../http/http_sck.h"

#include "xfs.h"

void* xfs_https_alloc(size_t size)
{
    return libspectrum_malloc(size);
}

void* xfs_https_calloc(size_t count, size_t size)
{
    void* ptr = libspectrum_malloc(count * size);
    if (ptr)
    {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

void* xfs_https_realloc(void* ptr, size_t size)
{
    return libspectrum_realloc(ptr, size);
}

void xfs_https_free(void* ptr)
{
    libspectrum_free(ptr);
}

char* xfs_https_index_buffer_acquire(size_t* out_size)
{
    if (!out_size)
    {
        return NULL;
    }

    *out_size = 2048;
    return (char*)libspectrum_malloc(*out_size);
}

void xfs_https_index_buffer_release(char* buffer)
{
    libspectrum_free(buffer);
}

int xfs_https_http_get_buffer(const char* url, char* buffer, size_t* length)
{
    return httpc_get_buffer(&tls_sck, url, buffer, length);
}

int xfs_https_http_head(const char* url)
{
    return httpc_head(&tls_sck, url);
}

int xfs_https_http_response(void)
{
    return tls_sck.response;
}

struct xfs_https_download_ctx
{
    uint8_t* data;
    size_t size;
    size_t capacity;
    size_t expected_size;
};

static int write_buffer_callback(void *param, unsigned char *buf, size_t length, size_t position, size_t content_length)
{
    struct xfs_https_download_ctx* ctx = (struct xfs_https_download_ctx*)param;

    if (!ctx)
    {
        return -1;
    }

    if (content_length > 0 && ctx->expected_size == 0)
    {
        ctx->expected_size = content_length;
    }

    if (content_length > 0 && ctx->capacity == 0)
    {
        ctx->capacity = content_length;
        ctx->data = libspectrum_malloc(content_length);
        if (!ctx->data)
        {
            XFS_DEBUG("https: write_buffer_callback [ERROR] failed to allocate buffer of size %zu\n", content_length);
            return -1;
        }
    }

    if (position + length > ctx->capacity)
    {
        size_t new_capacity = ctx->capacity;
        if (new_capacity == 0)
        {
            new_capacity = 4096;
        }

        while (new_capacity < position + length)
        {
            new_capacity *= 2;
        }

        void* new_data = libspectrum_realloc(ctx->data, new_capacity);
        if (!new_data)
        {
            XFS_DEBUG("https: write_buffer_callback [ERROR] failed to reallocate buffer to size %zu\n", new_capacity);
            return -1;
        }

        ctx->data = new_data;
        ctx->capacity = new_capacity;
    }

    memcpy(ctx->data + position, buf, length);
    if (position + length > ctx->size)
    {
        ctx->size = position + length;
    }

    return (int)length;
}

int xfs_https_download_file(const char* url, uint8_t** out_blob, size_t* out_size)
{
    if (!out_blob || !out_size)
    {
        return -1;
    }

    struct xfs_https_download_ctx ctx = {0};
    const int result = httpc_get(&tls_sck, url, write_buffer_callback, &ctx);

    if (result != HTTPC_OK)
    {
        libspectrum_free(ctx.data);
        *out_blob = NULL;
        *out_size = 0;
        return result;
    }

    if (ctx.expected_size > 0 && ctx.size != ctx.expected_size)
    {
        XFS_DEBUG("https: open failed: short download bytes=%zu expected=%zu\n", ctx.size, ctx.expected_size);
        libspectrum_free(ctx.data);
        *out_blob = NULL;
        *out_size = 0;
        return -1;
    }

    *out_blob = ctx.data;
    *out_size = ctx.size;
    return result;
}

void xfs_https_download_file_free(uint8_t* blob)
{
    libspectrum_free(blob);
}
