#include "config.h"

#include "http_sck.h"
#include "httpc.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "libspectrum.h"
#include "compat.h"
#include "../security/tls.h"
#include "../nic/dns_resolver.h"

// Socket structure for httpc
typedef struct {
    compat_socket_t fd;
    tls_socket_t *tls;
} fuse_http_socket_t;

void* malloc_allocator(void *arena, void *ptr, size_t oldsz, size_t newsz)
{
    (void)arena;
    if (newsz)
    {
        if (ptr)
        {
            return libspectrum_realloc(ptr, newsz);
        }
        return libspectrum_malloc(newsz);
    }

    /* httpc frees with oldsz=0; the pointer is the ownership signal. */
    (void)oldsz;
    if (ptr)
    {
        libspectrum_free(ptr);
    }

    return NULL;
}

int sck_http_open(httpc_options_t *os, void **socket_out, void *opts, const char *domain, const unsigned short port, int use_ssl)
{
    (void)os;
    (void)opts;
    
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int ret;
    compat_socket_t fd = compat_socket_invalid;
    tls_socket_t *tls = NULL;
    fuse_http_socket_t *sock = NULL;

    // Resolve hostname
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);
    
    ret = getaddrinfo(domain, port_str, &hints, &result);
    if (ret != 0)
    {
        printf("http: getaddrinfo failed for %s: %s\n", domain, gai_strerror(ret));
        return HTTPC_ERROR;
    }

    // Create socket
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == compat_socket_invalid)
    {
        printf("http: failed to create socket\n");
        freeaddrinfo(result);
        return HTTPC_ERROR;
    }

    /* Streaming file handles must release their worker after a silent peer. */
    if (os->context)
    {
#ifdef WIN32
        DWORD timeout = 10000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
        struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    }

    // Set socket to blocking mode
    // Note: compat_socket_blocking_mode has inverted logic (bug in compat layer)
    // Pass 0 to get blocking mode, 1 to get non-blocking mode
    compat_socket_blocking_mode(fd, 0);

    // Connect
    if (connect(fd, result->ai_addr, result->ai_addrlen) == -1)
    {
        printf("http: connect failed: %s\n", compat_socket_get_strerror());
        compat_socket_close(fd);
        freeaddrinfo(result);
        return HTTPC_ERROR;
    }

    freeaddrinfo(result);

    // If SSL/TLS is required, set up TLS
    if (use_ssl)
    {
        tls = tls_socket_alloc(fd, domain);
        if (!tls)
        {
            printf("http: failed to allocate TLS socket\n");
            compat_socket_close(fd);
            return HTTPC_ERROR;
        }

        // Perform TLS handshake
        ret = tls_connect(tls);
        if (ret != 0)
        {
            printf("http: TLS handshake failed: %d\n", ret);
            tls_socket_free(tls);
            compat_socket_close(fd);
            return HTTPC_ERROR;
        }
    }

    // Allocate socket structure
    sock = libspectrum_malloc(sizeof(fuse_http_socket_t));
    if (!sock)
    {
        if (tls)
            tls_socket_free(tls);
        compat_socket_close(fd);
        return HTTPC_ERROR;
    }

    sock->fd = fd;
    sock->tls = tls;
    *socket_out = sock;

    return HTTPC_OK;
}

int sck_http_close(httpc_options_t *os, void *socket)
{
    (void)os;
    
    if (!socket)
        return HTTPC_OK;

    fuse_http_socket_t *sock = (fuse_http_socket_t*)socket;

    if (sock->tls)
    {
        tls_close(sock->tls);
        tls_socket_free(sock->tls);
        sock->tls = NULL;
    }

    if (sock->fd != compat_socket_invalid)
    {
        compat_socket_close(sock->fd);
        sock->fd = compat_socket_invalid;
    }

    libspectrum_free(sock);
    return HTTPC_OK;
}

void sck_http_abort(void *socket)
{
    if (!socket) return;
    fuse_http_socket_t *sock = socket;
    if (sock->fd != compat_socket_invalid)
    {
#ifdef WIN32
        shutdown(sock->fd, SD_BOTH);
#else
        shutdown(sock->fd, SHUT_RDWR);
#endif
    }
}

int sck_http_read(httpc_options_t *os, void *socket, unsigned char *buf, size_t *length)
{
    (void)os;
    
    if (!socket || !buf || !length)
        return HTTPC_ERROR;

    fuse_http_socket_t *sock = (fuse_http_socket_t*)socket;
    ssize_t ret;

    if (sock->tls)
    {
        // TLS read
        ret = tls_read(sock->tls, buf, *length);
        if (ret < 0)
        {
            return HTTPC_ERROR;
        }
        *length = (size_t)ret;
    }
    else
    {
        // Plain TCP read
        ret = recv(sock->fd, buf, *length, 0);
        if (ret < 0)
        {
            if (compat_socket_get_error() == COMPAT_EWOULDBLOCK ||
                compat_socket_get_error() == COMPAT_EINPROGRESS)
            {
                *length = 0;
                return HTTPC_YIELD;
            }
            return HTTPC_ERROR;
        }
        *length = (size_t)ret;
    }

    return HTTPC_OK;
}

int sck_http_write(httpc_options_t *os, void *socket, const unsigned char *buf, size_t *length)
{
    (void)os;
    
    if (!socket || !buf || !length)
        return HTTPC_ERROR;

    fuse_http_socket_t *sock = (fuse_http_socket_t*)socket;
    ssize_t ret;

    if (sock->tls)
    {
        // TLS write
        ret = tls_write(sock->tls, buf, *length);
        if (ret < 0)
        {
            return HTTPC_ERROR;
        }
        *length = (size_t)ret;
    }
    else
    {
        // Plain TCP write
        ret = send(sock->fd, buf, *length, 0);
        if (ret < 0)
        {
            if (compat_socket_get_error() == COMPAT_EWOULDBLOCK ||
                compat_socket_get_error() == COMPAT_EINPROGRESS)
            {
                *length = 0;
                return HTTPC_YIELD;
            }
            return HTTPC_ERROR;
        }
        *length = (size_t)ret;
    }

    return HTTPC_OK;
}

int sck_http_sleep(httpc_options_t *os, const unsigned long milliseconds)
{
    (void)os;
    compat_timer_sleep((int)milliseconds);
    return HTTPC_OK;
}

int sck_http_time(httpc_options_t *os, unsigned long *millisecond)
{
    (void)os;
    if (millisecond)
    {
        // Simple time implementation - just return 0 for now
        // Could use compat_timer_get_time() if needed
        *millisecond = 0;
    }
    return HTTPC_OK;
}

int sck_http_logger(httpc_options_t *os, void *file, const char *fmt, va_list ap)
{
    (void)os;
    (void)file;
    return vprintf(fmt, ap);
}

httpc_options_t tls_sck = {
    .allocator = malloc_allocator,
    .open = sck_http_open,
    .close = sck_http_close,
    .read = sck_http_read,
    .write = sck_http_write,
    .sleep = sck_http_sleep,
    .time = sck_http_time,
    .logger = sck_http_logger,
    .flags = HTTPC_OPT_LOGGING_ON
};

void sck_http_options_init(httpc_options_t *options)
{
    memset(options, 0, sizeof(*options));
    options->allocator = malloc_allocator;
    options->open = sck_http_open;
    options->close = sck_http_close;
    options->read = sck_http_read;
    options->write = sck_http_write;
    options->sleep = sck_http_sleep;
    options->time = sck_http_time;
    options->logger = sck_http_logger;
    options->flags = HTTPC_OPT_LOGGING_ON;
}
