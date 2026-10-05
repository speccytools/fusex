#include "config.h"

#include "xfs_https_compat.h"

#include <string.h>

#include "libspectrum.h"
#include "../http/httpc.h"
#include "../http/http_sck.h"

void* xfs_https_alloc(size_t size) { return libspectrum_malloc(size); }

void* xfs_https_calloc(size_t count, size_t size)
{
    void *ptr = libspectrum_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

void* xfs_https_realloc(void* ptr, size_t size) { return libspectrum_realloc(ptr, size); }
void xfs_https_free(void* ptr) { libspectrum_free(ptr); }

char* xfs_https_index_buffer_acquire(size_t* out_size)
{
    if (!out_size) return NULL;
    *out_size = 2048;
    return libspectrum_malloc(*out_size);
}

void xfs_https_index_buffer_release(char* buffer) { libspectrum_free(buffer); }

int xfs_https_http_get_buffer(const char* url, char* buffer, size_t* length)
{
    return httpc_get_buffer(&tls_sck, url, buffer, length);
}

int xfs_https_http_head(const char* url) { return httpc_head(&tls_sck, url); }
int xfs_https_http_response(void) { return tls_sck.response; }
