/* Project: Embeddable HTTP 1.0/1.1 Client
 * Author:  Richard James Howe
 * License: The Unlicense
 * Email:   howe.r.j.89@gmail.com */
#ifndef HTTPC_H
#define HTTPC_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdarg.h>

#ifndef HTTPC_API
#define HTTPC_API /* Used to apply attributes to exported functions */
#endif

#ifndef ALLOCATOR_FN
#define ALLOCATOR_FN
typedef void *(*allocator_fn)(void *arena, void *ptr, size_t oldsz, size_t newsz);
#endif

enum {
	HTTPC_OPT_HTTP_1_0     = 1u << 0, /* attempt HTTP 1.0 request, although still process HTTP 1.1 responses */
	HTTPC_OPT_LOGGING_ON   = 1u << 1, /* turn logging on, if compiled in */
	HTTPC_OPT_NON_BLOCKING = 1u << 2, /* turn on non-blocking mode, library will return HTTPC_YIELD instead of blocking */
	HTTPC_OPT_REUSE        = 1u << 3, /* turn on connection reuse */
};

typedef int (*httpc_callback)(void *param, unsigned char *buf, size_t length, size_t position, size_t content_length);

struct httpc_options;
typedef struct httpc_options httpc_options_t;

struct httpc_options { /* Note that get/put/etcetera functions can *write* to this structure */
	allocator_fn allocator;

	int (*open)(httpc_options_t *os, void **socket, void *opts, const char *domain, unsigned short port, int use_ssl);
	int (*close)(httpc_options_t *os, void *socket);
	int (*read)(httpc_options_t *os, void *socket, unsigned char *buf, size_t *length);
	int (*write)(httpc_options_t *os, void *socket, const unsigned char *buf, size_t *length);
	int (*sleep)(httpc_options_t *os, unsigned long milliseconds );
	int (*time)(httpc_options_t *os, unsigned long *millisecond);
	int (*logger)(httpc_options_t *os, void *file, const char *fmt, va_list ap);

	void *arena       /* passed to allocator */,
	     *logfile,    /* passed to logger */
	     *socketopts, /* passed to open */
	     *state;      /* internal state for each operation; do not use */

	unsigned flags;   /* options for library */
	size_t receive_buffer_size; /* zero uses the default small response buffer */

	int argc;         /* custom headers count; number of custom headers */
	char **argv;      /* custom headers; appended to the HTTP request */

	void *context;    /* For your use, feel free to fill with good thoughts and positive affirmations */
	int response;     /* HTTP response code; set after the header is retrieved. */
};


enum {
	HTTPC_ERROR = -1, /* negated HTTP error codes also returned */
	HTTPC_OK    =  0, /* all operations completed successfully */
	HTTPC_YIELD =  1, /* call again later - the operation has not finished */
	HTTPC_REUSE =  2, /* operation complete, connection not closed */
};

struct httpc;
typedef struct httpc httpc_t;

/* all functions: return negative on failure, zero or positive on success */
HTTPC_API int httpc_get(httpc_options_t *a, const char *url, httpc_callback fn, void *param);
HTTPC_API int httpc_put(httpc_options_t *a, const char *url, httpc_callback fn, void *param); /* fn should return size, 0 on stop, -1 on failure */
HTTPC_API int httpc_post(httpc_options_t *a, const char *url, httpc_callback fn, void *param); /* fn should return size, 0 on stop, -1 on failure */
HTTPC_API int httpc_get_buffer(httpc_options_t *a, const char *url, char *buffer, size_t *length); /* store GET to buffer */
HTTPC_API int httpc_put_buffer(httpc_options_t *a, const char *url, char *buffer, size_t length); /* PUT from buffer */
HTTPC_API int httpc_post_buffer(httpc_options_t *a, const char *url, char *buffer, size_t length); /* POST from buffer */
HTTPC_API int httpc_head(httpc_options_t *a, const char *url);
HTTPC_API int httpc_delete(httpc_options_t *a, const char *url);
HTTPC_API int httpc_trace(httpc_options_t *a, const char *url);
HTTPC_API int httpc_options(httpc_options_t *a, const char *url);
HTTPC_API int httpc_end_session(httpc_options_t *a);
HTTPC_API int httpc_tests(httpc_options_t *a);

/* You provide these functions and populate 'httpc_options_t' with them when porting to a new platform 
 * (i.e. Not Unix or Windows) - return negative on failure, zero (HTTPC_OK) on success, and for 
 * open/close/read/write return HTTPC_YIELD if you want the client to yield to its' caller. */
HTTPC_API extern int httpc_open(httpc_options_t *a, void **socket, void *socketopts, const char *domain, unsigned short port, int use_ssl);
HTTPC_API extern int httpc_close(httpc_options_t *a, void *socket);
HTTPC_API extern int httpc_read(httpc_options_t *a, void *socket, unsigned char *buf, size_t *length);
HTTPC_API extern int httpc_write(httpc_options_t *a, void *socket, const unsigned char *buf, size_t *length);
HTTPC_API extern int httpc_sleep(httpc_options_t *a, unsigned long milliseconds);
HTTPC_API extern int httpc_time(httpc_options_t *a, unsigned long *milliseconds);
HTTPC_API extern int httpc_logger(httpc_options_t *a, void *logfile, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif
#endif
