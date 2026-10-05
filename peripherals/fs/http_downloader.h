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
    HTTP_DOWNLOAD_INVAL = -6,
};

/* One network transfer at a time. Each successful open owns its buffered bytes
 * until close, even after the transfer has finished. */
http_download_t *http_downloader_open(const char *url, int *error);
/* Known responses below 256 KiB are retained in full and support seek. */
int32_t http_downloader_seek(http_download_t *download, int32_t offset, uint8_t whence);
int32_t http_downloader_read(http_download_t *download, void *buffer, uint32_t size);
void http_downloader_close(http_download_t *download);

/* Abort the active transfer and wake any blocked reader or open. */
void http_downloader_cancel_all(void);
