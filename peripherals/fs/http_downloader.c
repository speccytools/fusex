#include "config.h"

#include "http_downloader.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libspectrum.h"
#include "../http/httpc.h"
#include "../http/http_sck.h"

enum { RING_SIZE = 256 * 1024, WAIT_SECONDS = 10 };

struct http_download {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    char *url;
    uint8_t *ring;
    size_t head, tail, used;
    bool ready, finished, cancelled, orphaned;
    int result, response;
    httpc_options_t options;
    void *socket;
};

static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker_thread;
static bool worker_started;
static http_download_t *active, *pending;

static void destroy_download(http_download_t *h)
{
    pthread_cond_destroy(&h->changed);
    pthread_mutex_destroy(&h->lock);
    libspectrum_free(h->url);
    libspectrum_free(h->ring);
    libspectrum_free(h);
}

static int download_socket_open(httpc_options_t *options, void **socket,
                                void *opts, const char *domain,
                                unsigned short port, int use_ssl)
{
    http_download_t *h = options->context;
    const int rc = sck_http_open(options, socket, opts, domain, port, use_ssl);
    if (rc != HTTPC_OK) return rc;
    pthread_mutex_lock(&h->lock);
    h->socket = *socket;
    if (h->cancelled) sck_http_abort(*socket);
    pthread_mutex_unlock(&h->lock);
    return rc;
}

static int download_socket_close(httpc_options_t *options, void *socket)
{
    http_download_t *h = options->context;
    pthread_mutex_lock(&h->lock);
    h->socket = NULL;
    pthread_mutex_unlock(&h->lock);
    return sck_http_close(options, socket);
}

static struct timespec deadline(void)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += WAIT_SECONDS;
    return until;
}

static int receive_chunk(void *context, unsigned char *data, size_t length,
                         size_t position, size_t content_length)
{
    (void)position;
    (void)content_length;
    http_download_t *h = context;
    size_t copied = 0;
    struct timespec until = deadline();
    pthread_mutex_lock(&h->lock);
    h->response = h->options.response;
    if (h->response < 200 || h->response >= 300) {
        pthread_mutex_unlock(&h->lock);
        return -1;
    }
    while (copied < length && !h->cancelled) {
        while (h->used == RING_SIZE && !h->cancelled) {
            if (pthread_cond_timedwait(&h->changed, &h->lock, &until) == ETIMEDOUT) {
                h->result = HTTP_DOWNLOAD_TIMEOUT;
                h->cancelled = true;
            }
        }
        if (h->cancelled) break;
        size_t take = length - copied;
        if (take > RING_SIZE - h->used) take = RING_SIZE - h->used;
        if (take > RING_SIZE - h->head) take = RING_SIZE - h->head;
        memcpy(h->ring + h->head, data + copied, take);
        h->head = (h->head + take) % RING_SIZE;
        h->used += take;
        copied += take;
        until = deadline();
        h->ready = true;
        pthread_cond_broadcast(&h->changed);
    }
    const bool cancelled = h->cancelled;
    pthread_mutex_unlock(&h->lock);
    return cancelled ? -1 : 0;
}

static void *download_worker(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&worker_lock);
        while (!pending) pthread_cond_wait(&worker_changed, &worker_lock);
        http_download_t *h = pending;
        pending = NULL;
        pthread_mutex_unlock(&worker_lock);

        pthread_mutex_lock(&h->lock);
        const bool cancelled = h->cancelled;
        pthread_mutex_unlock(&h->lock);
        int rc = HTTPC_ERROR;
        if (!cancelled) {
            sck_http_options_init(&h->options);
            h->options.context = h;
            h->options.open = download_socket_open;
            h->options.close = download_socket_close;
            h->options.receive_buffer_size = 4096;
            rc = httpc_get(&h->options, h->url, receive_chunk, h);
        }

        pthread_mutex_lock(&worker_lock);
        active = NULL;
        pthread_mutex_unlock(&worker_lock);
        pthread_mutex_lock(&h->lock);
        h->response = h->options.response;
        if (!h->cancelled && (rc != HTTPC_OK || h->response < 200 || h->response >= 300))
            h->result = h->response == 404 ? HTTP_DOWNLOAD_NOENT : HTTP_DOWNLOAD_IO;
        h->ready = true;
        h->finished = true;
        const bool orphaned = h->orphaned;
        pthread_cond_broadcast(&h->changed);
        pthread_mutex_unlock(&h->lock);
        if (orphaned) destroy_download(h);
    }
    return NULL;
}

http_download_t *http_downloader_open(const char *url, int *error)
{
    if (error) *error = HTTP_DOWNLOAD_IO;
    if (!url) return NULL;
    http_download_t *h = libspectrum_malloc(sizeof(*h));
    if (!h) { if (error) *error = HTTP_DOWNLOAD_NOMEM; return NULL; }
    memset(h, 0, sizeof(*h));
    h->ring = libspectrum_malloc(RING_SIZE);
    h->url = libspectrum_malloc(strlen(url) + 1);
    if (!h->ring || !h->url) {
        libspectrum_free(h->url);
        libspectrum_free(h->ring);
        libspectrum_free(h);
        if (error) *error = HTTP_DOWNLOAD_NOMEM;
        return NULL;
    }
    strcpy(h->url, url);
    pthread_mutex_init(&h->lock, NULL);
    pthread_cond_init(&h->changed, NULL);

    pthread_mutex_lock(&worker_lock);
    if (active) {
        pthread_mutex_unlock(&worker_lock);
        if (error) *error = HTTP_DOWNLOAD_BUSY;
        goto fail;
    }
    if (!worker_started) {
        if (pthread_create(&worker_thread, NULL, download_worker, NULL) != 0) {
            pthread_mutex_unlock(&worker_lock);
            if (error) *error = HTTP_DOWNLOAD_IO;
            goto fail;
        }
        pthread_detach(worker_thread);
        worker_started = true;
    }
    active = pending = h;
    pthread_cond_signal(&worker_changed);
    pthread_mutex_unlock(&worker_lock);

    pthread_mutex_lock(&h->lock);
    const struct timespec until = deadline();
    bool timed_out = false;
    while (!h->ready) {
        if (pthread_cond_timedwait(&h->changed, &h->lock, &until) == ETIMEDOUT) {
            h->result = HTTP_DOWNLOAD_TIMEOUT;
            h->cancelled = true;
            sck_http_abort(h->socket);
            h->orphaned = true;
            timed_out = true;
            break;
        }
    }
    const int result = h->result;
    pthread_mutex_unlock(&h->lock);
    if (timed_out) {
        if (error) *error = HTTP_DOWNLOAD_TIMEOUT;
        return NULL;
    }
    if (result) {
        if (error) *error = result;
        http_downloader_close(h);
        return NULL;
    }
    if (error) *error = HTTP_DOWNLOAD_OK;
    return h;
fail:
    destroy_download(h);
    return NULL;
}

int32_t http_downloader_read(http_download_t *h, void *buffer, uint32_t size)
{
    if (!h || (!buffer && size)) return HTTP_DOWNLOAD_IO;
    if (!size) return 0;
    if (size > INT32_MAX) size = INT32_MAX;
    pthread_mutex_lock(&h->lock);
    const struct timespec until = deadline();
    while (!h->used && !h->finished && !h->result) {
        if (pthread_cond_timedwait(&h->changed, &h->lock, &until) == ETIMEDOUT) {
            h->result = HTTP_DOWNLOAD_TIMEOUT;
            h->cancelled = true;
            sck_http_abort(h->socket);
            pthread_cond_broadcast(&h->changed);
            break;
        }
    }
    size_t take = size < h->used ? size : h->used;
    if (take) {
        size_t first = take < RING_SIZE - h->tail ? take : RING_SIZE - h->tail;
        memcpy(buffer, h->ring + h->tail, first);
        memcpy((uint8_t *)buffer + first, h->ring, take - first);
        h->tail = (h->tail + take) % RING_SIZE;
        h->used -= take;
        pthread_cond_broadcast(&h->changed);
    }
    const int result = h->result;
    pthread_mutex_unlock(&h->lock);
    return take ? (int32_t)take : result;
}

void http_downloader_close(http_download_t *h)
{
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    h->cancelled = true;
    sck_http_abort(h->socket);
    pthread_cond_broadcast(&h->changed);
    while (!h->finished) pthread_cond_wait(&h->changed, &h->lock);
    pthread_mutex_unlock(&h->lock);
    destroy_download(h);
}
