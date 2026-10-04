#pragma once

#include <stdint.h>

typedef struct http_download http_download_t;

enum http_download_error {
    HTTP_DOWNLOAD_OK = 0,
    HTTP_DOWNLOAD_IO = -1,
    HTTP_DOWNLOAD_NOENT = -2,
    HTTP_DOWNLOAD_BUSY = -3,
    HTTP_DOWNLOAD_NOMEM = -4,
    HTTP_DOWNLOAD_TIMEOUT = -5,
};

/* One network transfer at a time. Each successful open owns its buffered bytes
 * until close, even after the transfer has finished. */
http_download_t *http_downloader_open(const char *url, int *error);
int32_t http_downloader_read(http_download_t *download, void *buffer, uint32_t size);
void http_downloader_close(http_download_t *download);
