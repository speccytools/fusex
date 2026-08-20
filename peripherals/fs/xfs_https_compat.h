#pragma once

#include <stddef.h>
#include <stdint.h>

#define XFS_HTTPS_HTTP_OK 0

void* xfs_https_alloc(size_t size);
void* xfs_https_calloc(size_t count, size_t size);
void* xfs_https_realloc(void* ptr, size_t size);
void xfs_https_free(void* ptr);

char* xfs_https_index_buffer_acquire(size_t* out_size);
void xfs_https_index_buffer_release(char* buffer);

int xfs_https_http_get_buffer(const char* url, char* buffer, size_t* length);
int xfs_https_http_head(const char* url);
int xfs_https_http_response(void);

int xfs_https_download_file(const char* url, uint8_t** out_blob, size_t* out_size);
void xfs_https_download_file_free(uint8_t* blob);
