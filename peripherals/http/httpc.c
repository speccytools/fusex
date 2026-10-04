#define HTTPC_PROGRAM "Embeddable HTTP 1.0/1.1 client"
#define HTTPC_LICENSE "The Unlicense (public domain)"
#define HTTPC_AUTHOR  "Richard James Howe"
#define HTTPC_EMAIL   "howe.r.j.89@gmail.com"
#define HTTPC_REPO    "https://github.com/howerj/httpc"
#ifndef HTTPC_VERSION
#define HTTPC_VERSION "0.0.0" /* defined by build system */
#endif
#include "httpc.h"
#define LOCALELY_API static inline
#define LOCALELY_EXTERN LOCALELY_API
#define LOCALELY_IMPLEMENTATION
#include "localely.h" /* used because isX and toupper are locale dependent and thus suck */
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#ifndef HTTPC_STACK_BUFFER_SIZE /* buffers allocated on the stack, responsible for some arbitrary limits as well. */
#define HTTPC_STACK_BUFFER_SIZE (256ul)
#endif

#ifndef HTTPC_TESTS_ON /* Build in tests to the program */
#define HTTPC_TESTS_ON (0u)
#endif

#ifndef HTTPC_GROW /* Allow data structures to grow */
#define HTTPC_GROW (1u)
#endif

#ifndef HTTPC_LOGGING /* 0 == logging disabled, 1 == logging on */
#define HTTPC_LOGGING (0u)
#endif

#ifndef HTTPC_CONNECTION_ATTEMPTS /* default maximum number of connection attempts */
#define HTTPC_CONNECTION_ATTEMPTS (3u)
#endif

#ifndef HTTPC_REDIRECT_MAX /* default maximum number of HTTP redirects */
#define HTTPC_REDIRECT_MAX (3u)
#endif

#ifndef HTTPC_MAX_HEADER /* maximum size for the header; 0 == infinite length allowed */
#define HTTPC_MAX_HEADER (8192ul)
#endif

#ifndef HTTPC_ALLOW_ANY_RESPONSE_STRING /* Allow any response string, e.g. instead of requiring "200 ok", "200 beep boop" would be fine */
#define HTTPC_ALLOW_ANY_RESPONSE_STRING (0)
#endif

#define USED(X)                 ((void)(X)) /* warning suppression: variable is used conditionally */
#define UNUSED(X)               ((void)(X)) /* warning suppression: variable is unused in function */
#define MAX(X, Y)               ((X) > (Y) ? (X) : (Y))
#define MIN(X, Y)               ((X) < (Y) ? (X) : (Y))
#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#define implies(P, Q)           assert(!(P) || (Q))
#define NELEMS(X)               ((sizeof(X) / sizeof((X)[0])))

typedef struct {
	unsigned char stack[HTTPC_STACK_BUFFER_SIZE]; /* small temporary buffer */
	unsigned char *buffer;                        /* either points to buf or is allocated */
	size_t allocated, used; /* number of bytes allocates, number of byte actually used */
} httpc_buffer_t; /* growable buffer, also used for memory optimization reasons */

typedef struct {
	char *buffer;
	size_t length, used;
} httpc_buffer_cb_t; /* used in PUT/POST/GET to memory functions */

typedef struct {
	int code;
	const char *value;
} httpc_response_string_t;

typedef unsigned long httpc_length_t;

struct httpc {
	httpc_options_t *os; /* operating system dependent functions for socket, TLS/SLL and allocation. */
	httpc_buffer_t b0, burl; /* buffers for temporary values, heavily reused, be careful! */
	httpc_callback snd, /* callback for sending payload, called repeatedly to generate output in chunks */
		       rcv; /* callback for receiving payload, called piecemeal to on chunks of input */
	void *snd_param, /* parameter given to the "snd" callback */
	     *rcv_param; /* parameter given to the "rcv" callback */
	/* These strings point into 'url', which has been modified from the
	 * original URL to contain a bunch of NUL terminated strings where the
	 * delimiters were */
	char *domain, /* parsed: domain or IPv4/IPv6 */
	     *userpass, /* parsed: username/password used for basic authentication */
	     *path, /* parsed: file path */
	     *url;  /* full URL to be parsed */
	unsigned short port; /* port to talk on, parsed out at the same time as domain, userpass, path and URL. */
	void *socket; /* socket used to talk to the server (might actually be an SSL/TLS handle). */
	httpc_length_t position, /* file position */
			length,  /* length of file, if known */
			max;     /* maximum read in */
	int state,  /* HTTPC contains a state-machine, the state of which is encoded here. */
	    status; /* HTTP return code status */
	unsigned long start_ms, /* Start time of operation */
			current_ms, /* Current time, used within exponential back-off operation */
			end_ms; /* End time of operation */
	unsigned retries,   /* number of times to connection retried */
		 redirects; /* number of times to redirected */
	unsigned retries_max,  /* maximum number of times to retry */
		 redirects_max; /* maximum number of times to redirect */
	unsigned v1, v2; /* HTTP version (1.0 or 1.1) */
	unsigned use_ssl       :1, /* if set then SSL should be used on the connection */
		 fatal         :1, /* if set then something has gone fatally wrong */
		 accept_ranges :1, /* if set then the server accepts ranges */
		 identity      :1, /* 1 == identity encoded, 0 == chunked */
		 redirect      :1, /* if set then a redirect is going on */
		 length_set    :1, /* has length been set on a PUT/POST? */
		 open          :1, /* is the file handle open? */
		 keep_alive    :1, /* does the server support keep-alive? */
		 progress      :1; /* are we making progress? */
};

static inline void httpc_reverse_string(char * const r, const size_t length) {
	assert(r);
	const size_t last = length - 1;
	for (size_t i = 0; i < length / 2ul; i++) {
		const size_t t = r[i];
		r[i] = r[last - i];
		r[last - i] = t;
	}
}

static unsigned httpc_num_to_str(char b[64 + 1], unsigned long u, const unsigned long base) {
	assert(b);
	assert(base >= 2 && base <= 36);
	unsigned i = 0;
	do {
		const unsigned long q = u % base;
		const unsigned long r = u / base;
		b[i++] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[q];
		assert(i < 64u);
		u = r;
	} while (u);
	b[i] = '\0';
	httpc_reverse_string(b, i);
	return i;
}

static int httpc_kill(httpc_t *h) {
	assert(h);
	h->fatal = 1;
	return HTTPC_ERROR;
}

static int httpc_is_dead(httpc_t *h) {
	assert(h);
	return h->fatal;
}

static int httpc_is_yield_on(httpc_t *h) {
	assert(h);
	return !!(h->os->flags & HTTPC_OPT_NON_BLOCKING);
}

static int httpc_is_reuse(httpc_t *h) {
	assert(h);
	return !!(h->os->flags & HTTPC_OPT_REUSE);
}

#ifdef __GNUC__
static int httpc_log_fmt(httpc_t *h, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
static int httpc_log_line(httpc_t *h, const char *type, int die, int ret, const unsigned line, const char *fmt, ...) __attribute__ ((format (printf, 6, 7)));
#endif

static int httpc_log_fmt(httpc_t *h, const char *fmt, ...) {
	assert(fmt);
	assert(h);
	assert(h->os);
	httpc_options_t *os = h->os;
	assert(os->logger);
	va_list ap;
	va_start(ap, fmt);
	const int r = os->logger(os, os->logfile, fmt, ap);
	va_end(ap);
	if (r < 0)
		(void)httpc_kill(h);
	return r;
}

static int httpc_log_line(httpc_t *h, const char *type, int die, int ret, const unsigned line, const char *fmt, ...) {
	assert(h);
	assert(fmt);
	httpc_options_t *os = h->os;
	if (os->flags & HTTPC_OPT_LOGGING_ON) {
		assert(os->logger);
		assert(type);
		if (httpc_log_fmt(h, "%s:%u ", type, line) < 0)
			return HTTPC_ERROR;
		va_list ap;
		va_start(ap, fmt);
		if (os->logger(os, os->logfile, fmt, ap) < 0)
			(void)httpc_kill(h);
		va_end(ap);
		if (httpc_log_fmt(h, "\n") < 0)
			return HTTPC_ERROR;
	}
	if (die)
		return httpc_kill(h);
	return httpc_is_dead(h) ? HTTPC_ERROR : ret;
}

#if HTTPC_LOGGING == 0
static inline int httpc_rcode(const int c) { return c; } /* suppresses warnings */
#define debug(H, ...) httpc_rcode(HTTPC_OK)
#define info(H, ...)  httpc_rcode(HTTPC_OK)
#define error(H, ...) httpc_rcode(HTTPC_ERROR)
#define fatal(H, ...) httpc_kill((H))
#else
#define debug(H, ...) httpc_log_line((H), "debug", 0, HTTPC_OK,    __LINE__, __VA_ARGS__)
#define info(H, ...)  httpc_log_line((H), "info",  0, HTTPC_OK,    __LINE__, __VA_ARGS__)
#define error(H, ...) httpc_log_line((H), "error", 0, HTTPC_ERROR, __LINE__, __VA_ARGS__)
#define fatal(H, ...) httpc_log_line((H), "fatal", 1, HTTPC_ERROR, __LINE__, __VA_ARGS__)
#endif

static void *httpc_malloc(httpc_t *h, const size_t size) {
	assert(h);
	assert(h->os->allocator);
	if (httpc_is_dead(h))
		return NULL;
	void *r = h->os->allocator(h->os->arena, NULL, 0, size);
	if (!r) {
		(void)httpc_kill(h);
		(void)debug(h, "malloc %ld failed", (long)size);
		return NULL;
	}
	void *m = memset(r, 0, size);
	(void)debug(h, "malloc %p/%ld", m, (long)size);
	return m;
}

static void *httpc_realloc(httpc_t *h, void *pointer, const size_t size) {
	assert(h);
	assert(h->os->allocator);
	(void)debug(h, "%s %p/%ld", size > 0 ? "realloc" : "free", pointer, (long)size);
	if (httpc_is_dead(h) && size > 0)
		return NULL;
	void *r = h->os->allocator(h->os->arena, pointer, 0, size);
	if (r == NULL && size != 0)
		(void)httpc_kill(h);
	return r;
}

static int httpc_free(httpc_t *h, void *pointer) {
	assert(h);
	assert(h->os->allocator);
	(void)debug(h, "free %p", pointer);
	(void)h->os->allocator(h->os->arena, pointer, 0, 0);
	return HTTPC_OK;
}

static int httpc_network_read(httpc_t *h, unsigned char *bytes, size_t *length) {
	assert(h);
	assert(bytes);
	assert(length);
	const int r = h->os->read(h->os, h->socket, bytes, length);
	if (r < 0 || (r != HTTPC_OK && r != HTTPC_YIELD))
		return error(h, "network read error %d", r);
	if (r == HTTPC_YIELD)
		return fatal(h, httpc_is_yield_on(h) ? "yield not implemented" : "yielding when yield option not set");
	return r;
}

static int httpc_network_write(httpc_t *h, const unsigned char *bytes, size_t length) {
	assert(h);
	assert(bytes);
	if (length == 0)
		return HTTPC_OK;
	size_t l = length;
	const int r = h->os->write(h->os, h->socket, bytes, &l);
	if (l != length)
		return error(h, "network write incomplete");
	if (r < 0 || (r != HTTPC_OK && r != HTTPC_YIELD))
		return error(h, "network write error %d", r);
	if (r == HTTPC_YIELD)
		return fatal(h, httpc_is_yield_on(h) ? "yield not implemented" : "yielding when yield option not set");
	return r;
}

static int httpc_read_char(httpc_t *h) {
	assert(h);
	size_t length = 1;
	unsigned char x = 0;
	if (httpc_network_read(h, &x, &length) < 0)
		return -1;
	if (length != 1)
		return -1;
	return x;
}

static int buffer_free(httpc_t *h, httpc_buffer_t *s) {
	assert(h);
	assert(s);
	if (s->buffer != s->stack) {
		const int r  = httpc_free(h, s->buffer);
		s->buffer    = NULL; /* prevent double free */
		s->allocated = 0;
		return r;
	}
	return HTTPC_OK; /* pointer == buffer, no need to free */
}

static int httpc_buffer(httpc_t *h, httpc_buffer_t *s, size_t needed) { /* dynamically growable buffer */
	assert(h);
	assert(s);

	if (s->buffer == NULL) { /* take care of initialization */
		s->buffer    = s->stack;
		s->used      = 0;
		s->allocated = sizeof (s->stack);
		memset(s->stack, 0, sizeof s->stack);
	}

	if (needed <= s->allocated) /* we could free here if we only need stack buffer */
		return HTTPC_OK;
	if (HTTPC_GROW == 0)
		return fatal(h, "buffer not allowed to grow");
	if (s->buffer == s->stack) {
		if (!(s->buffer = httpc_malloc(h, needed)))
			return fatal(h, "allocation failed");
		s->allocated = needed;
		memcpy(s->buffer, s->stack, sizeof s->stack);
		return HTTPC_OK;
	}
	unsigned char *old = s->buffer;
	if ((s->buffer = httpc_realloc(h, s->buffer, needed)) == NULL) {
		(void)httpc_free(h, old);
		return fatal(h, "reallocation failed");
	}
	s->allocated = needed;
	return HTTPC_OK;
}

static int httpc_buffer_add_string(httpc_t *h, httpc_buffer_t *b, const char *s) {
	assert(h);
	assert(b);
	assert(s);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	const size_t l = strlen(s);
	const size_t newsz = l + b->used + !(b->used);
	if (httpc_buffer(h, b, newsz) < 0)
		return HTTPC_ERROR;
	memcpy(b->buffer + b->used - !!(b->used), s, l);
	b->used = newsz;
	b->buffer[b->used - 1] = '\0';
	return HTTPC_OK;
}

/* Modified from: <https://stackoverflow.com/questions/342409>.
 *
 * This function should be added to the API, along with many others
 * within this library. However it would need reworking slightly,
 * perhaps to work on raw buffers only. We would also need to add
 * a base-64 decode, as it would be odd to have one function without
 * the other. */
static int httpc_buffer_add_base64(httpc_t *h, httpc_buffer_t *out, const unsigned char *in, const size_t input_length) {
	assert(h);
	assert(in);
	assert(out);
	/* assert(shake_it_all_about); */
	const size_t encoded_length  = 4ull * ((input_length + 2ull) / 3ull);
	const size_t needs = 1u + encoded_length + out->used;
	assert(needs > encoded_length);
	assert(encoded_length > input_length);
	if (needs < encoded_length)
		return -1;
	if (httpc_buffer(h, out, needs) < 0)
		return -1;
	size_t j = out->used - (out->used != 0);
	for (size_t i = 0; i < input_length;) {
		static const char encoding_table[] = {
			'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
			'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
			'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
			'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'
		};
		const uint32_t octet_a = i < input_length ? (unsigned char)in[i++] : 0;
		const uint32_t octet_b = i < input_length ? (unsigned char)in[i++] : 0;
		const uint32_t octet_c = i < input_length ? (unsigned char)in[i++] : 0;
		const uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
		assert(j < (out->allocated - 4));
		out->buffer[j++] = encoding_table[(triple >> (3 * 6)) & 0x3F];
		out->buffer[j++] = encoding_table[(triple >> (2 * 6)) & 0x3F];
		out->buffer[j++] = encoding_table[(triple >> (1 * 6)) & 0x3F];
		out->buffer[j++] = encoding_table[(triple >> (0 * 6)) & 0x3F];
	}
	static const int mod_table[] = { 0, 2, 1, };
  {
    for (int i = 0; i < mod_table[input_length % 3]; i++)
      out->buffer[j - 1u - i] = '=';
  }
	assert(j < out->allocated);
	out->buffer[j] = '\0';
	out->used = j;
	return 0;
}

static inline int httpc_character_to_number(int ch) {
	ch = C_toupper(ch);
	static const char m[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	for (int d = 0; d < (int)(sizeof (m) - 1); d++)
		if (ch == m[d])
			return d;
	return -1;
}

static int httpc_string_to_number(const char *s, httpc_length_t *out, const size_t max, unsigned long base) {
	assert(s);
	assert(out);
	assert(max < SIZE_MAX);
	httpc_length_t result = 0;
	int ch = s[0];
	*out = 0;
	if (!ch)
		return -1;
	size_t j = 0;
	for (j = 0; j < max && (ch = s[j]); j++) {
		const int digit = httpc_character_to_number(ch);
		if (digit < 0)
			return -1;
		const httpc_length_t n = (httpc_length_t)digit + (result * (httpc_length_t)base);
		if (n < result) /* overflow */
			return -1;
		result = n;
	}
	if (ch && j < max)
		return -1;
	*out = result;
	return 0;
}

static int httpc_scan_number(const char *s, httpc_length_t *out, unsigned long base) {
	assert(s);
	assert(out);
	while (C_isspace(*s))
		s++;
	return httpc_string_to_number(s, out, 64, base);
}

/* URL Format is (roughly):
 *
 *  (http/https '://')? (user-info '@')? host (':' port)? ('/' path ('?' query)* ('#' fragment)?)
 *
 * The 'user-info' format is:
 *
 *     username ':' password
 *
 * Must ensure invalid characters are not present in path/domain/parsed-out-contents such
 * as spaces.
 *
 * The 'url' string is modified by putting in NUL terminating
 * characters where the separators were.  
 *
 * With a few modifications (such as accepting a struct for the various fields)
 * this could be added to the API. */
static int httpc_parse_url(httpc_t *h, const char *url) {
	assert(h);
	assert(url);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	h->url = NULL;
	const size_t l = strlen(url);
	if (httpc_buffer(h, &h->burl, l + 2) < 0)
		return HTTPC_ERROR;
	h->url = (char*)h->burl.buffer;
	memcpy(h->url, url, l + 1);
	h->port    = 80;
	h->use_ssl = 0;

	int ch = 0;
	size_t i = 0, j = 0;
	char *u = h->url;

	for (;(ch = u[i]); i++)
		if (!C_isspace(ch))
			break;
	if (!ch) {
		error(h, "invalid URL: %s", url);
		goto fail;
	}

	const char http[] = "http://", https[] = "https://";

	if (l > sizeof http && !memcmp(&u[i], http, sizeof(http) - 1ul)) {
		i += sizeof(http) - 1ul;
	} else if (l > sizeof https && !memcmp(&u[i], https, sizeof(https) - 1ul)) {
		h->use_ssl = 1u;
		h->port = 443;
		i += sizeof(https) - 1ul;
	}

	char *usr = memchr(&u[i], '@', l - i);
	if (usr) {
		h->userpass = &u[i];
		i = (usr - u) + 1ul;
		*usr = '\0';
		if (!strchr(h->userpass, ':')) {
			error(h, "user-pass contains no ':': %s", h->userpass);
			goto fail;
		}
	}

	h->domain = &u[i];
	for (j = i;(ch = u[j]);j++)
		if (ch == ':' || ch == '/')
			break;
	if (j == i)
		goto fail;
	if (ch == '/') {
		memmove(&u[j + 1], &u[j], strlen(&u[j]) + 1);
		u[j] = '\0';
	}
	if (!strlen(h->domain))
		goto fail;
	if (ch == ':') {
		u[j] = '\0';
		httpc_length_t port = 0;
		for (i = j + 1; (ch = u[i]); i++)
			if (!C_isdigit(ch))
				break;
		if (httpc_string_to_number(&u[j + 1], &port, i - (j + 1), 10) < 0) {
			error(h, "invalid port number");
			goto fail;
		}
		h->port = port;
		j = i - 1;
	}

	h->path = &u[j + 1];
	h->path = h->path[0] ? h->path : "/";

	info(h, "domain:    %s", h->domain);
	info(h, "port:      %d", h->port);
	info(h, "SSL:       %s", h->use_ssl ? "true" : "false");
	if (h->userpass)
		info(h, "user/pass: %s", h->userpass);
	info(h, "path       %s", h->path ? h->path : "/");
	return HTTPC_OK;
fail:
	h->url = NULL;
	return HTTPC_ERROR;
}

enum { HTTPC_GET, HTTPC_HEAD, HTTPC_PUT, HTTPC_POST, HTTPC_DELETE, HTTPC_TRACE, HTTPC_OPTIONS, };

static const char *httpc_op_to_str(int op) {
	switch (op) {
	case HTTPC_GET:     return "GET ";
	case HTTPC_HEAD:    return "HEAD ";
	case HTTPC_PUT:     return "PUT ";
	case HTTPC_POST:    return "POST ";
	case HTTPC_DELETE:  return "DELETE ";
	case HTTPC_TRACE:   return "TRACE ";
	case HTTPC_OPTIONS: return "OPTIONS ";
	}
	return NULL;
}

/* This allows us to be a bit more liberal in what we accept */
static inline int httpc_case_insensitive_compare(const char *a, const char *b, const size_t length) {
	assert(a);
	assert(b);
	assert(length < SIZE_MAX);
	for (size_t i = 0; i < length ; i++) {
		const int ach = C_tolower(a[i]);
		const int bch = C_tolower(b[i]);
		const int diff = ach - bch;
		if (!ach || diff)
			return diff;
	}
	return 0;
}

static const char *httpc_case_insensitive_search(const char *haystack, const char *needle) {
	assert(haystack);
	assert(needle);
	const size_t needle_length = strlen(needle);
	for (; *haystack; haystack++)
		if (0 == httpc_case_insensitive_compare(haystack, needle, needle_length))
			return haystack;
	return NULL;
}

static int httpc_request_send_header(httpc_t *h, httpc_buffer_t *b0, int op) {
	assert(h);
	assert(b0);
	implies(h->os->argc, h->os->argv);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	const char *operation = httpc_op_to_str(op);
	if (!operation)
		return fatal(h, "unknown operation '%d'", op);
	b0->used = 0;
	if (httpc_buffer_add_string(h, b0, operation) < 0)
		goto fail;
	if (httpc_buffer_add_string(h, b0, h->path ? h->path : "/") < 0)
		goto fail;
	if (h->os->flags & HTTPC_OPT_HTTP_1_0) {
		if (httpc_buffer_add_string(h, b0, " HTTP/1.0\r\nHost: ") < 0)
			goto fail;
	} else {
		if (httpc_buffer_add_string(h, b0, " HTTP/1.1\r\nHost: ") < 0)
			goto fail;
	}
	if (httpc_buffer_add_string(h, b0, h->domain) < 0)
		goto fail;
	if (httpc_buffer_add_string(h, b0, "\r\n") < 0)
		goto fail;
	if (op == HTTPC_GET && !(h->os->flags & HTTPC_OPT_HTTP_1_0) && h->position && h->accept_ranges) {
		char range[64 + 1] = { 0, };
		if (httpc_buffer_add_string(h, b0, "Range: bytes=") < 0)
			goto fail;
		httpc_num_to_str(range, h->position, 10);
		if (httpc_buffer_add_string(h, b0, range) < 0)
			goto fail;
		if (httpc_buffer_add_string(h, b0, "-\r\n") < 0)
			goto fail;
	}
	if (op == HTTPC_PUT || op == HTTPC_POST) {
		const char field[] = "Content-Length:";
		size_t field_len= sizeof(field) - 1;
		for (int i = 0; i < h->os->argc; i++) {
			const char *line = h->os->argv[i];
			if (httpc_case_insensitive_compare(field, line, field_len) == 0){
				if (httpc_scan_number(&line[field_len], &h->length, 10) == 0){
					h->length_set = 1;
				}
				break;
			}
		}
		if (h->length_set == 0) {
			if (httpc_buffer_add_string(h, b0, "Transfer-Encoding: chunked\r\n") < 0)
				goto fail;
		}
	}

	if (httpc_is_reuse(h)) {
		if (httpc_buffer_add_string(h, b0, "Connection: keep-alive\r\n") < 0)
			goto fail;
	} else {
		if (httpc_buffer_add_string(h, b0, "Connection: close\r\n") < 0)
			goto fail;
	}
	if (httpc_buffer_add_string(h, b0, "Accept-Encoding: identity\r\n") < 0)
		goto fail;
	if (h->userpass) {
		const size_t upl = strlen(h->userpass);
		if (httpc_buffer_add_string(h, b0, "Authorization: Basic ") < 0)
			goto fail;
		if (httpc_buffer_add_base64(h, b0, (uint8_t*)h->userpass, upl) < 0) {
			error(h, "base64 encoding fail");
			goto fail;
		}
		if (httpc_buffer_add_string(h, b0, "\r\n") < 0)
			goto fail;
	}

	/* N.B. We could split up the writes by instead calling 'httpc_network_write' instead 
	 * of 'httpc_buffer_add_string' if we wanted. It has some advantages. */
	assert(b0->used > 0u);
	if (httpc_network_write(h, b0->buffer, b0->used - 1u) < 0)
		goto fail;

	for (int i = 0; i < h->os->argc; i++) {
		const char *line = h->os->argv[i];
		size_t l = 0;
		for (l = 0; line[l]; l++)
			if (line[l] == '\r' || line[l] == '\n')
				return fatal(h, "invalid custom header field (illegal chars present)");
		if (httpc_network_write(h, (unsigned char *)line, l) < 0)
			goto fail;
		debug(h, "custom header '%s' added", line);
		if (httpc_network_write(h, (unsigned char *)"\r\n", 2) < 0)
			goto fail;
	}

	if (httpc_network_write(h, (unsigned char *)"\r\n", 2) < 0)
		goto fail;

	return info(h, "%s request complete", operation);
fail:
	return error(h, "send GET header failed");
}

static int httpc_backoff(httpc_t *h) {
	/* N.B. Instead of Xms, we could use the round trip time as estimated by the
	 * connection time as an initial guess as per RFC 2616 */
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	const unsigned long exponen = MIN(h->retries, 16u);
	const unsigned long backoff = 500ul * (1ul << exponen);
	const unsigned long limited = MIN(1000ul * 10ul * 1ul, backoff);
	if (httpc_is_yield_on(h)) {
		unsigned long now_ms = 0;
		if (h->os->time(h->os, &now_ms) < 0)
			return fatal(h, "unable to get time");
		if ((now_ms - h->current_ms) > limited)
			return HTTPC_OK;
		return HTTPC_YIELD;
	}
	info(h, "backing off for %lu ms, retried %u", limited, (h->retries));
	return h->os->sleep(h->os, limited);
}

/* N.B. We could add in a callback to handle unknown fields, however we would
 * need to add infrastructure so an external user could meaningfully interact
 * with the library internals, which would be too invasive. */
static int httpc_parse_response_field(httpc_t *h, char *line, size_t length) {
	assert(h);
	assert(line);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	if (length == 0)
		return HTTPC_OK;
	line[length - 1] = '\0';

#define X_MACRO_FIELDS \
	X("Transfer-Encoding:", FLD_TRANSFER_ENCODING) \
	X("Content-Length:",    FLD_CONTENT_LENGTH)\
	X("Content-Range:",     FLD_CONTENT_RANGE)\
	X("Accept-Ranges:",     FLD_ACCEPT_RANGES)\
	X("Connection:",        FLD_CONNECTION)\
	X("Location:",          FLD_REDIRECT)

	enum {
#define X(STR, ENUM) ENUM,
		X_MACRO_FIELDS
#undef X
	};

	static const struct field {
		const char *name;
		size_t length;
		int type;
	} fields[] = {
#define X(STR, ENUM) { .name = STR, .length = sizeof (STR) - 1, .type = ENUM },
		X_MACRO_FIELDS
#undef X
	};

	const size_t field_length = sizeof (fields) / sizeof (fields[0]);
	for (size_t i = 0; i < field_length; i++) {
		const struct field *fld = &fields[i];
		if (fld->length > length)
			continue;
		if (httpc_case_insensitive_compare(fld->name, line, fld->length))
			continue;
		/* N.B. Using 'httpc_case_insensitive_search' is a little too liberal in our input handling */
		switch (fld->type) {
		case FLD_ACCEPT_RANGES:
			if (httpc_case_insensitive_search(line, "bytes")) {
				h->accept_ranges = !(h->os->flags & HTTPC_OPT_HTTP_1_0);
				return info(h, "Accept-Ranges: bytes");
			}
			if (httpc_case_insensitive_search(line, "none")) {
				h->accept_ranges = 0;
				return info(h, "Accept-Ranges: none");
			}
			return error(h, "unknown accept ranges field: %s", line);
		case FLD_TRANSFER_ENCODING:
			if (strchr(line, ','))
				return error(h, "Transfer encoding too complex, cannot handle it: %s", line);
			if (httpc_case_insensitive_search(line, "identity")) {
				h->identity = 1;
				h->position = 0;
				return info(h, "identity transfer encoding");
			}
			if (httpc_case_insensitive_search(line, "chunked")) { /* chunky monkey setting */
				h->identity = 0;
				return info(h, "chunked transfer encoding");
			}
			return error(h, "cannot handle transfer encoding: %s", line);
		case FLD_CONNECTION:
			if (httpc_case_insensitive_search(line, "close")) {
				h->keep_alive = 0;
				return info(h, "connection close mandatory");
			}
			if (httpc_case_insensitive_search(line, "keep-alive")) {
				h->keep_alive = 1;
				return info(h, "connection may be kept alive");
			}
			return error(h, "unknown connection type");
		case FLD_CONTENT_LENGTH:
			if (httpc_scan_number(&line[fld->length], &h->length, 10) < 0)
				return error(h, "invalid content length: %s", line);
			if (h->os->response == 206 && h->position) {
				if ((h->length + h->position) < h->length)
					return error(h, "content length overflow");
				h->length += h->position;
			}
			h->length_set = 1;
			return info(h, "Content Length: %lu", (unsigned long)h->length);
		case FLD_CONTENT_RANGE: {
			char *slash = strchr(&line[fld->length], '/');
			if (!slash)
				return error(h, "invalid content range: %s", line);
			slash++;
			if (!*slash || *slash == '*')
				return info(h, "Content Range: %s", line);
			if (httpc_scan_number(slash, &h->length, 10) < 0)
				return error(h, "invalid content range total: %s", line);
			h->length_set = 1;
			return info(h, "Content Range total: %lu", (unsigned long)h->length);
		}
		case FLD_REDIRECT:
			if (h->os->response >= 300 && h->os->response < 399) {
				if (h->redirects++ > h->redirects_max)
					return error(h, "redirect count exceed max (%u)", (unsigned)h->redirects_max);
				size_t k = 0, j = 0;
				for (k = fld->length; C_isspace(line[k]); k++)
					;
				j = k;
				for (k = fld->length; !C_isspace(line[k]) && line[k]; k++)
					;
				line[k] = '\0';
				if (httpc_parse_url(h, &line[j]) < 0)
					return fatal(h, "redirect failed");
				h->redirect = 1;
				return info(h, "redirecting request");
			}
			return fatal(h, "invalid redirect");
		default:
			return fatal(h, "invalid state");
		}
	}
	return info(h, "unknown field: %s", line);
}

static int httpc_read_until_line_end(httpc_t *h, httpc_buffer_t *b, size_t *length) {
	assert(h);
	assert(b);
	assert(length);
	size_t olength = *length;
	*length = 0;
	if (olength == 0)
		return fatal(h, "expected length > 0");
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	b->buffer[olength - 1] = '\0';
	for (size_t i = 0; i < (olength - 1ul); i++) {
		const int ch = httpc_read_char(h);
		if (ch < 0) {
			assert(i < olength);
			b->buffer[i] = '\0';
			return error(h, "unexpected EOF");
		}
		if (ch == '\n' || ch == '\r') { /* accept either "\n" or "\r\n" */
			if (ch != '\n' && httpc_read_char(h) != '\n')
				return error(h, "Got '\\r' with no '\\n'");
			assert(i < olength);
			b->buffer[i] = '\0';
			*length = i;
			return HTTPC_OK;
		}
		assert(i < olength);
		b->buffer[i] = ch;
		if ((i + 1ul) >= (olength - 1ul)) {
			const size_t newsz = olength * 2ul;
			if (newsz < olength) /* overflow */
				return HTTPC_ERROR;
			if (httpc_buffer(h, b, newsz) < 0)
				return HTTPC_ERROR;
			olength = newsz;
		}
	}
	return fatal(h, "buffer too small");
}

/* N.B. We should check for end of string here (which can include white-space) */
static int httpc_response_string_matches(const httpc_response_string_t *m, const char *string) {
	assert(m);
	assert(string);
	const size_t elen = strlen(m->value), slen = strlen(string);
	if (slen < elen)
		return 0;
	return 0 == httpc_case_insensitive_compare(m->value, string, slen);
}

static int httpc_parse_response_header_start_line(httpc_t *h, char *line, const size_t length) {
	assert(h);
	const char v1_0[] = "HTTP/1.0 ", v1_1[] = "HTTP/1.1 ";
	size_t i = 0, j = 0;
	httpc_options_t *os = h->os;
	assert(length >= 1);

	if (length < sizeof (v1_0) && length < sizeof (v1_1))
		return error(h, "start line too small");

	if (!httpc_case_insensitive_compare(line, v1_0, sizeof (v1_0) - 1)) {
		h->v1 = 1;
		h->v2 = 0;
		i += sizeof (v1_0) - 1;
	} else if (!httpc_case_insensitive_compare(line, v1_1, sizeof (v1_1) - 1)) {
		h->v1 = 1;
		h->v2 = 1;
		i += sizeof (v1_1) - 1;
	} else {
		return error(h, "unknown HTTP protocol/version: %s", line);
	}
	while (C_isspace(line[i]))
		i++;
	j = i;
	while (C_isdigit(line[j]))
		j++;
	httpc_length_t resp = 0;
	if (httpc_string_to_number((const char *)&line[i], &resp, j - i, 10) < 0)
		return error(h, "invalid response number: %s", line);
	os->response = resp;
	if (os->response == 200 && h->position) {
		h->position = 0;
		h->max = 0;
	}
	while (C_isspace(line[j]))
		j++;
	if(j >= length)
		return error(h, "bounds exceeded");
	char *ok = &line[j];
	ok[length - 1u] = '\0';

	/* For handling redirections: <https://developer.mozilla.org/en-US/docs/Web/HTTP/Redirections> */
	if (os->response < 200 || os->response > 399)
		return error(h, "invalid response number: %u", os->response);
	if (os->response >= 200 && os->response <= 299) {
		if (HTTPC_ALLOW_ANY_RESPONSE_STRING)
			return HTTPC_OK;
		static const httpc_response_string_t resps[] = {
			{ 200, "OK", }, /* Default response, always allowed */
			{ 201, "Created", },
			{ 202, "Accepted", },
			{ 204, "No Content", },
			{ 206, "Partial Content", },
			{ 218, "This is fine", },
		};
		int found = 0;
		for (size_t i = 0; i < NELEMS(resps); i++) {
			const httpc_response_string_t *m = &resps[i];
			if (httpc_response_string_matches(m, ok) && m->code == os->response) {
				found = 1;
				break;
			}
		}
		if (!found) {
			httpc_response_string_t okay = { os->response, "OK", };
			found = httpc_response_string_matches(&okay, ok);
		}
		if (!found)
			return error(h, "unexpected HTTP response: %s", ok);
	}
	return HTTPC_OK;
}

static int httpc_parse_response_header(httpc_t *h, httpc_buffer_t *b0) {
	assert(h);
	assert(b0);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	size_t length = 0, hlen = 0;
	httpc_options_t *os = h->os;
	h->v1 = 0;
	h->v2 = 0;
	os->response = 0;
	h->length = 0;
	h->identity = 1;
	h->length_set = 0;
	h->keep_alive = !(os->flags & HTTPC_OPT_HTTP_1_0);
	h->accept_ranges = !(os->flags & HTTPC_OPT_HTTP_1_0);
	b0->used = 0;

	length = b0->allocated;
	if (httpc_read_until_line_end(h, b0, &length) < 0)
		return error(h, "protocol error (could not read first line)");
	hlen += length;
	info(h, "HEADER: %s/%lu", b0->buffer, (unsigned long)length);

	if (httpc_parse_response_header_start_line(h, (char*)b0->buffer, length) < 0)
		return error(h, "start line parse failed");
	for (; hlen < HTTPC_MAX_HEADER; hlen += length) {
		length = b0->allocated;
		if (httpc_read_until_line_end(h, b0, &length) < 0)
			return error(h, "invalid header: %s", b0->buffer);
		if (length == 0)
			break;
		if (httpc_parse_response_field(h, (char*)b0->buffer, b0->allocated) < 0)
			return error(h, "error parsing response line");
		if ((hlen + length) < hlen)
			return fatal(h, "overflow in length");
	}

	return info(h, "header done");
}

static int httpc_execute_rcv_callback(httpc_t *h, const unsigned char *buf, const size_t length) {
	assert(h);
	assert(buf);
	if (h->rcv == NULL) /* null operation */
		return HTTPC_OK;
	if ((h->position + length) <= h->max) /* discard previous data run */
		return HTTPC_OK;
	const size_t overlap = h->position < h->max ? h->max - h->position : 0;
	assert(overlap <= length);
	const size_t diff = length - overlap;
	const int r = h->rcv(h->rcv_param, (unsigned char*)buf + overlap, diff,
	                     h->position + overlap, h->length);
	if (r == HTTPC_YIELD)
		return fatal(h, "yield not supported here");
	if (r < 0)
		return fatal(h, "rcv callback failed");
	return HTTPC_OK;
}

static int httpc_parse_response_body_identity(httpc_t *h, httpc_buffer_t *b0) {
	assert(h);
	assert(h->identity);
	assert(b0);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;

	b0->used = 0;
	for (;;) {
		size_t length = b0->allocated;
		if (httpc_network_read(h, b0->buffer, &length) < 0)
			return error(h, "read error");
		if (length == 0)
			break;
		if ((h->position + length) < h->position)
			return fatal(h, "overflow in length");
		if (httpc_execute_rcv_callback(h, b0->buffer, length) < 0)
			return HTTPC_ERROR;
		h->position += length;
		h->max = MAX(h->max, h->position);
		if (h->length_set && h->position >= h->length)
			return HTTPC_OK;
		if (httpc_is_yield_on(h))
			return HTTPC_YIELD;
	}
	if (h->length_set && h->position != h->length)
		return error(h, "expected %lu bytes but got %lu", (unsigned long)h->position, (unsigned long)h->length);
	return HTTPC_OK;
}

static int httpc_parse_response_body_chunked(httpc_t *h, httpc_buffer_t *b0) {
	assert(h);
	assert(b0);
	assert(h->identity == 0);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;

	for (;;) {
		size_t nl = b0->allocated;
		if (httpc_read_until_line_end(h, b0, &nl) < 0) {
			if (h->length_set && h->length == 0)
				return info(h, "zero content length, nothing to read");
			return error(h, "unexpected EOF");
		}
		httpc_length_t length = 0;
		if (httpc_string_to_number((char*)b0->buffer, &length, nl, 16) < 0)
			return error(h, "number format error: %s", b0->buffer);
		if (length == 0)
			return info(h, "chunked done");

		b0->used = 0;
		for (size_t i = 0, l = 0; i < length; i += l) {
			const size_t requested = MIN(b0->allocated, length - i);
			l = requested;
			if (httpc_network_read(h, b0->buffer, &l) < 0)
				return error(h, "read failed");
			if (httpc_execute_rcv_callback(h, b0->buffer, l) < 0)
				return HTTPC_ERROR;
			if ((h->position + l) < h->position)
				return error(h, "overflow in position");
			h->position += l;
			h->max = MAX(h->max, h->position);
		}
		nl = 1;
		if (httpc_network_read(h, b0->buffer, &nl) < 0 || nl != 1)
			return HTTPC_ERROR;
		if (b0->buffer[0] == '\r') {
			nl = 1;
			if (httpc_network_read(h, b0->buffer, &nl) < 0 || nl != 1)
				return HTTPC_ERROR;
		}
		if (b0->buffer[0] != '\n')
			return HTTPC_ERROR;
		if (httpc_is_yield_on(h))
			return HTTPC_YIELD;
	}
	return HTTPC_OK;
}

static int httpc_parse_response_body(httpc_t *h) {
	assert(h);
	if (httpc_is_dead(h))
		return HTTPC_ERROR;
	if (h->identity)
		return httpc_parse_response_body_identity(h, &h->b0);
	return httpc_parse_response_body_chunked(h, &h->b0);
}

static int httpc_generate_request_body(httpc_t *h, httpc_buffer_t *b0) {
	assert(h);
	assert(b0);
	int r = HTTPC_OK;
	if (!(h->snd))
		return info(h, "no callback - nothing to do");
	const int chunky = h->length_set == 0;
	for (size_t pos = 0;;) {
		b0->used = 0;
		r = h->snd(h->snd_param, b0->buffer, b0->allocated, pos, 0);
		if (r == 0)
			break; /* done! */
		if (r < 0) {
			(void)error(h, "snd failed");
			break;
		}
		if (r > (int)(b0->allocated)) {
			r = error(h, "snd result too big");
			break;
		}
		if ((pos + (unsigned)r) < pos) {
			r = error(h, "snd overflow");
			break;
		}
		pos += r;
		if (chunky) {
			char n[64 + 1] = { 0, };
			assert(r < INT_MAX);
			const unsigned l = httpc_num_to_str(n, r, 16);
			assert((l + 2) <= sizeof (n));
			memcpy(&n[l], "\r\n", 3);
			if (httpc_network_write(h, (unsigned char*)n, l + 2) < 0) {
				r = error(h, "write failed");
				break;
			}
		}

		if (httpc_network_write(h, b0->buffer, r) < 0) {
			r = error(h, "write failed");
			break;
		}

		if (chunky) {
			if (httpc_network_write(h, (unsigned char*)"\r\n", 2) < 0) {
				r = error(h, "write failed");
				break;
			}
		}
		if (httpc_is_yield_on(h))
			return HTTPC_YIELD;
	}
	if (r < 0)
		return error(h, "body generation failed");
	if (chunky) {
		if (httpc_network_write(h, (unsigned char*)"0\r\n\r\n", 5) < 0)
			return error(h, "write failed");
	}
	return info(h, "body generated");
}

static inline int httpc_banner(httpc_t *h) {
	assert(h);
	USED(h);
	USED(httpc_log_line); /* warning suppression if HTTPC_LOGGING == 0 */
	info(h, "Program: "HTTPC_PROGRAM);
	info(h, "Version: "HTTPC_VERSION);
	info(h, "Repo:    "HTTPC_REPO);
	info(h, "Author:  "HTTPC_AUTHOR);
	info(h, "Email:   "HTTPC_EMAIL);
	info(h, "Options: stk=%lu tst=%u grw=%u log=%u cons=%u redirs=%u hmax=%lu sz=%u",
		HTTPC_STACK_BUFFER_SIZE, HTTPC_TESTS_ON, HTTPC_GROW,
		HTTPC_LOGGING, HTTPC_CONNECTION_ATTEMPTS, HTTPC_REDIRECT_MAX,
		HTTPC_MAX_HEADER, (unsigned)(sizeof *h));
	return info(h, "License: "HTTPC_LICENSE);
}

enum { SM_INIT, SM_OPEN, SM_SNDH, SM_SNDB, SM_RCVH, SM_RCVB, SM_REDR, SM_BCKO, SM_SLEP, SM_DONE, };

static inline const char *sm_to_str(const int sm) {
	switch (sm) {
	case SM_INIT: return "initial  ";
	case SM_OPEN: return "open     ";
	case SM_SNDH: return "send-head";
	case SM_SNDB: return "send-body";
	case SM_RCVH: return "recv-head";
	case SM_RCVB: return "recv-body";
	case SM_REDR: return "redirect ";
	case SM_BCKO: return "back-off ";
	case SM_SLEP: return "sleeps   ";
	case SM_DONE: return "done     ";
	}
	return "unknown";
}

static int httpc_state_machine(httpc_t *h, const char *url, int op) {
	assert(h);
	assert(url);
	int next = SM_DONE;
	httpc_options_t *os = h->os;
next_state:
	if (httpc_is_dead(h))
		h->state = SM_DONE;
	switch (h->state) {
	case SM_INIT:
		next        = SM_OPEN;
		h->open     = 0;
		h->progress = 0;
		os->response = 0;
		if (os->flags & ~(HTTPC_OPT_LOGGING_ON | HTTPC_OPT_HTTP_1_0 | HTTPC_OPT_NON_BLOCKING | HTTPC_OPT_REUSE)) {
			h->status = fatal(h, "unknown option provided %u", os->flags);
			next      = SM_DONE;
		}
		if (os->time(os, &h->start_ms) < 0) {
			h->status = fatal(h, "unable to get time");
			next      = SM_DONE;
		}
		if (httpc_banner(h) < 0) {
			h->status = HTTPC_ERROR;
			next      = SM_DONE;
		}
		if (httpc_buffer(h, &h->b0, os->receive_buffer_size ? os->receive_buffer_size : HTTPC_STACK_BUFFER_SIZE) < 0) { h->status = HTTPC_ERROR; next = SM_DONE; break; }
		if (httpc_buffer(h, &h->burl, strlen(url) + 1) < 0)         { h->status = HTTPC_ERROR; next = SM_DONE; break; }
		if (httpc_parse_url(h, url) < 0)                      { h->status = HTTPC_ERROR; next = SM_DONE; break; }
		if (h->retries_max == 0)
			h->retries_max = HTTPC_CONNECTION_ATTEMPTS;
		if (h->redirects_max == 0)
			h->redirects_max = HTTPC_REDIRECT_MAX;
		break;
	case SM_OPEN: {
		next = SM_SNDH;
		implies(h->open, httpc_is_reuse(h));
		implies(h->open, h->keep_alive);
		if (h->open) /* reuse connection */
			break;
		const int y = os->open(os, &h->socket, os->socketopts, h->domain, h->port, h->use_ssl);
		if (y == HTTPC_OK)
			h->open = 1;
		else if (y == HTTPC_YIELD)
			next = SM_OPEN;
		else
			next = SM_BCKO;
		break;
	}
	case SM_SNDH: {
		next = SM_SNDB;
		const int y = httpc_request_send_header(h, &h->b0, op);
		if (y < 0)
			next = SM_BCKO;
		else if (y == HTTPC_YIELD)
			next = SM_SNDH;
		break;
	}
	case SM_SNDB: {
		next = SM_RCVH;
		const int y = httpc_generate_request_body(h, &h->b0);
		if (y < 0)
			next = SM_BCKO;
		else if (y == HTTPC_YIELD)
			next = SM_SNDB;
		break;
	}
	case SM_RCVH: {
		next = SM_RCVB;
		const int y = httpc_parse_response_header(h, &h->b0);
		if (y < 0) {
			next = SM_BCKO;
			if (op == HTTPC_PUT || op == HTTPC_POST || op == HTTPC_DELETE) {
				if (os->response) {
					error(h, "request failed");
					h->status = -(int)(os->response);
				}
			}
			if (os->response >= 400 && os->response <= 499) {
				h->status = -(int)(os->response);
				next      = SM_DONE;
			}
		} else if (y == HTTPC_YIELD) {
			next = SM_RCVH;
		} else if (h->redirect) {
			next = SM_REDR;
		} else if (op == HTTPC_GET && h->max && h->position) {
			if (os->response == 200) {
				/* A server may ignore Range. Replay from zero and discard
				 * bytes already delivered to the callback. */
				h->position = 0;
			} else if (os->response == 206 && h->length_set) {
				if (h->length > ULONG_MAX - h->position) {
					h->status = fatal(h, "range length overflow");
					next = SM_DONE;
				} else {
					h->length += h->position;
				}
			}
		}
		break;
	}
	case SM_RCVB:
		next = SM_DONE;
		if (op == HTTPC_GET) {
			const httpc_length_t pos = h->position;
			h->progress = 0;
			const int r = httpc_parse_response_body(h);
			if (r < 0) {
				h->progress = pos < h->position; /* we have processed some data...*/
				next = SM_BCKO;
			} else if (r == HTTPC_YIELD) {
				next = SM_RCVB;
			}
		}
		break;
	case SM_REDR:
		next = SM_OPEN;
		if (os->close(os, h->socket) == HTTPC_YIELD) {
			next = SM_REDR;
		} else { /* do not care about errors -- only yield */
			h->open = 0;
			h->redirect = 0;
		}
		break;
	case SM_BCKO:
		next = SM_SLEP;
		if (h->open) {
			if (os->close(os, h->socket) == HTTPC_YIELD) {
				next = SM_BCKO;
				break; /* !! */
			} else { /* do not care about errors -- only yield */
				h->open = 0;
			}
		}

		if (h->retries >= h->retries_max) {
			h->status = HTTPC_ERROR;
			next      = SM_DONE;
			break;
		}
		h->retries += !(h->progress);
		h->progress = 0;
		h->redirect = 0;

		if (os->time(os, &h->current_ms) < 0) {
			h->status = fatal(h, "unable to get time");
			next = SM_DONE;
		}
		break;
	case SM_SLEP: {
		next = SM_OPEN;
		const int y = httpc_backoff(h);
		if (y < 0) {
			h->status = HTTPC_ERROR;
			next      = SM_DONE;
		} else if (y == HTTPC_YIELD) {
			next      = SM_SLEP;
		}
		break;
	}
	case SM_DONE:
		h->retries   = 0;
		h->redirects = 0;
		if (h->open) {
			if (httpc_is_reuse(h) && !httpc_is_dead(h) && h->status == HTTPC_OK && h->keep_alive) {
				next     = SM_OPEN;
				h->state = next; /* !! */
				return HTTPC_REUSE; /* !! */
			}
			if (os->close(os, h->socket) == HTTPC_YIELD) {
				/* stay in the same state */
				break; /* !! */
			} else { /* do not care about errors -- only yield */
				h->open   = 0;
				h->socket = NULL;
			}
		}
		h->url = NULL;
		if (buffer_free(h, &h->b0) < 0)
			h->status = HTTPC_ERROR;
		if (buffer_free(h, &h->burl) < 0)
			h->status = HTTPC_ERROR;
		if (os->time(os, &h->end_ms) < 0)
			h->status = HTTPC_ERROR;
		debug(h, "took %lu ms", h->end_ms - h->start_ms);
		const int dead = httpc_is_dead(h);
		/* must be last */
		int status = h->status;
		void *f = os->state;
		os->state = NULL;
		if (httpc_free(h, f) < 0)
			status = HTTPC_ERROR;
		assert(status != HTTPC_YIELD);
		assert(status != HTTPC_REUSE);
		assert(status <= HTTPC_OK);
		return dead ? HTTPC_ERROR : status;
	default:
		h->status = httpc_kill(h);
		next      = SM_DONE;
		break;
	}
	if (h->state != next)
		debug(h, "state -- %s -> %s", sm_to_str(h->state), sm_to_str(next));
	h->state = next;
	if (!httpc_is_yield_on(h))
		goto next_state;
	return HTTPC_YIELD;
}

int httpc_end_session(httpc_options_t *a) {
	assert(a);
	if (!(a->state))
		return HTTPC_OK;
	httpc_t *h = a->state;
	h->state = SM_DONE;
	(void)info(h, "end-session");
	(void)httpc_kill(h);
	for (;httpc_state_machine(h, "", HTTPC_GET) == HTTPC_YIELD;)
		;
	a->state = NULL;
	return HTTPC_OK; /* We might want to return `httpc_state_machine` value here */
}

static int httpc_op_stack(httpc_options_t *a, const char *url, int op, httpc_callback rcv, void *rcv_param, httpc_callback snd, void *snd_param) {
	if (a->state)
		return HTTPC_ERROR;
	httpc_t h = { .os = a, .rcv = rcv, .rcv_param = rcv_param, .snd = snd, .snd_param = snd_param, };
	assert(httpc_is_yield_on(&h) == 0);
	assert(httpc_is_reuse(&h)    == 0);
	return httpc_state_machine(&h, url, op);
}

static int httpc_op_heap(httpc_options_t *a, const char *url, int op, httpc_callback rcv, void *rcv_param, httpc_callback snd, void *snd_param) {
	assert(a);
	assert(url);
	httpc_t *h = a->state;
	if (!h) {
		h = a->allocator(a->arena, NULL, 0, sizeof *h);
		if (!h)
			return HTTPC_ERROR;
		memset(h, 0, sizeof *h);
		a->state     = h;
		h->os        = a;
		h->rcv       = rcv;
		h->snd       = snd;
		h->rcv_param = rcv_param;
		h->snd_param = snd_param;
	}
	const int r = httpc_state_machine(h, url, op);
	if (r != HTTPC_YIELD && r != HTTPC_REUSE)
		a->state = NULL; /* make sure this is not reused */
	return r;
}

static int httpc_operation(httpc_options_t *a, const char *url, int op, httpc_callback rcv, void *rcv_param, httpc_callback snd, void *snd_param) {
	assert(a);
	assert(url);
	const int yield = !!(a->flags & HTTPC_OPT_NON_BLOCKING);
	const int reuse = !!(a->flags & HTTPC_OPT_REUSE);
	if (yield || reuse)
		return httpc_op_heap(a, url, op, rcv, rcv_param, snd, snd_param);
	return httpc_op_stack(a, url, op, rcv, rcv_param, snd, snd_param);
}

int httpc_get(httpc_options_t *a, const char *url, httpc_callback fn, void *param) {
	assert(a);
	assert(url);
	return httpc_operation(a, url, HTTPC_GET, fn, param, NULL, NULL);
}

int httpc_put(httpc_options_t *a, const char *url, httpc_callback fn, void *param) {
	assert(a);
	assert(url);
	return httpc_operation(a, url, HTTPC_PUT, NULL, NULL, fn, param);
}

int httpc_post(httpc_options_t *a, const char *url, httpc_callback fn, void *param) {
	assert(a);
	assert(url);
	return httpc_operation(a, url, HTTPC_POST, NULL, NULL, fn, param);
}

int httpc_head(httpc_options_t *a, const char *url) {
	assert(a);
	assert(url);
	return httpc_operation(a, url, HTTPC_HEAD, NULL, NULL, NULL, NULL);
}

int httpc_delete(httpc_options_t *a, const char *url) { /* N.B. A DELETE body is technically allowed... */
	assert(a);
	assert(url);
	return httpc_operation(a, url, HTTPC_DELETE, NULL, NULL, NULL, NULL);
}

int httpc_trace(httpc_options_t *a, const char *url) {
	assert(a);
	assert(url);
	return httpc_operation(a, url, HTTPC_TRACE, NULL, NULL, NULL, NULL);
}

int httpc_options(httpc_options_t *a, const char *url) {
	assert(a);
	assert(url);
	return httpc_operation(a, url, HTTPC_OPTIONS, NULL, NULL, NULL, NULL);
}

static int httpc_get_buffer_cb(void *param, unsigned char *buf, size_t length, size_t position, size_t content_length) {
	assert(param);
	assert(buf);
	httpc_buffer_cb_t *b = param;
	if ((length + position) > b->length || (length + position) < length)
		return HTTPC_ERROR;
	memcpy(&b->buffer[position], buf, length);
	b->used = position + length;
	assert(b->used <= b->length);
	return HTTPC_OK;
}

static int httpc_put_buffer_cb(void *param, unsigned char *buf, size_t length, size_t position, size_t content_length) {
	assert(param);
	assert(buf);
	httpc_buffer_cb_t *b = param;
	assert(b->used <= b->length);
	if (position > b->used)
		return HTTPC_ERROR;
	const size_t copy = MIN(length, b->used - position);
	memcpy(buf, &b->buffer[position], copy);
	assert(copy < INT_MAX);
	return copy;
}

static inline int httpc_buffer_unsupported(httpc_options_t *a) {
	assert(a);
	return !!(a->flags & HTTPC_OPT_NON_BLOCKING) || !!(a->flags & HTTPC_OPT_REUSE);
}

/* N.B. These could be made to be non-blocking as well, but it is too much effort */
int httpc_get_buffer(httpc_options_t *a, const char *url, char *buffer, size_t *length) {
	assert(url);
	assert(a);
	assert(buffer);
	assert(length);
	if (httpc_buffer_unsupported(a))
		return HTTPC_ERROR;
	httpc_buffer_cb_t param = { .buffer = buffer, .length = *length, };
	*length  = 0;
	const int r = httpc_op_stack(a, url, HTTPC_GET, httpc_get_buffer_cb, &param, NULL, NULL);
	if (r == HTTPC_OK)
		*length = param.used;
	return r;
}

int httpc_put_buffer(httpc_options_t *a, const char *url, char *buffer, size_t length) {
	assert(url);
	assert(a);
	assert(buffer);
	if (httpc_buffer_unsupported(a))
		return HTTPC_ERROR;
	httpc_buffer_cb_t param = { .buffer = buffer, .length = length, .used = length, };
	return httpc_op_stack(a, url, HTTPC_PUT, NULL, NULL, httpc_put_buffer_cb, &param);
}

int httpc_post_buffer(httpc_options_t *a, const char *url, char *buffer, size_t length) {
	assert(url);
	assert(a);
	assert(buffer);
	if (httpc_buffer_unsupported(a))
		return HTTPC_ERROR;
	httpc_buffer_cb_t param = { .buffer = buffer, .length = length, .used = length, };
	return httpc_op_stack(a, url, HTTPC_POST, NULL, NULL, httpc_put_buffer_cb, &param);
}

static inline int httpc_testing_sleep(httpc_options_t *os, unsigned long milliseconds) {
	assert(os);
	UNUSED(milliseconds);
	return HTTPC_OK;
}

static inline int httpc_testing_time(httpc_options_t *os, unsigned long *milliseconds) {
	assert(os);
	assert(milliseconds);
	*milliseconds = 1;
	return HTTPC_OK;
}

typedef struct {
	httpc_t *h;
	const char *buffer;
	size_t length, position;
} testing_t;

static inline int httpc_testing_open(httpc_options_t *a, void **socket, void *opts, const char *domain, unsigned short port, int use_ssl) {
	assert(socket);
	assert(a);
	assert(opts);
	assert(domain);
	assert(port != 0);
	UNUSED(use_ssl);
	httpc_t *h = opts;

	static const struct sites {
		char *domain;
		char *file;
	} files[] = {
		{
			.domain = "example.com",
			.file   = "HTTP/1.1 200 OK\r\n"
				"Content-Type: text/plain\r\n"
				"Transfer-Encoding: chunked\r\n"
				"\r\n"
				"7\r\nMozilla\r\n"
				"9\r\nDeveloper\r\n"
				"7\r\nNetwork\r\n"
				"4\r\nWiki\r\n"
				"5\r\npedia\r\n"
				"E\r\n in\r\n\r\nchunks.\r\n"
				"0\r\n",
		},
		{
			.domain = "404.com", /* a bit of a hack */
			.file = "HTTP/1.1 404 Not Found\r\n",
		},
		{
			.domain = "redirect.com",
			.file = "HTTP/1.1 301 Moved Permanently\r\nLocation: example.com\r\n\r\n",
		},
		{
			.domain = "identity.com",
			.file = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789",
		}
	};
	const size_t files_count = sizeof (files) / sizeof (files[0]);
	for (size_t i = 0; i < files_count; i++) {
		if (!strcmp(domain, files[i].domain)) {
			testing_t *t = httpc_malloc(h, sizeof *t);
			if (!t)
				return fatal(h, "unable to malloc");
			t->h = h;
			t->buffer = files[i].file;
			t->length = strlen(t->buffer);
			t->position = 0;
			*socket = t;
			return HTTPC_OK;
		}
	}

	return HTTPC_ERROR;
}

static inline int httpc_testing_close(httpc_options_t *a, void *socket) {
	assert(a);
	assert(socket);
	testing_t *t = socket;
	return httpc_free(t->h, t);
}

static inline int httpc_testing_read(httpc_options_t *os, void *socket, unsigned char *buf, size_t *length) {
	assert(os);
	assert(socket);
	assert(buf);
	assert(length);
	testing_t *t = socket;
	size_t requested = *length;
	*length = 0;
	if (t->position >= t->length)
		return HTTPC_OK;
	size_t copy = MIN(requested, t->length - t->position);
	memcpy(buf, &t->buffer[t->position], copy);
	t->position += copy;
	*length = copy;
	return HTTPC_OK;
}

static inline int httpc_testing_write(httpc_options_t *os, void *socket, const unsigned char *buf, size_t *length) {
	assert(os);
	assert(socket);
	assert(buf);
	assert(length);
	return HTTPC_OK; /* discard for now; better unit tests would compare what is written as well */
}

int httpc_tests(httpc_options_t *a) {
	assert(a);
	BUILD_BUG_ON(HTTPC_STACK_BUFFER_SIZE < 128ul);
	BUILD_BUG_ON(HTTPC_CONNECTION_ATTEMPTS < 1ul);
	BUILD_BUG_ON(HTTPC_MAX_HEADER < 1024ul && HTTPC_MAX_HEADER != 0ul);
	BUILD_BUG_ON(HTTPC_ERROR != -1);
	BUILD_BUG_ON(HTTPC_OK    !=  0);
	int r = HTTPC_OK;

	if (HTTPC_TESTS_ON == 0)
		return HTTPC_OK;

	/*if (localely_unit_tests() < 0)
		r = HTTPC_ERROR;*/

	a->open  = httpc_testing_open;
	a->close = httpc_testing_close;
	a->read  = httpc_testing_read;
	a->write = httpc_testing_write;
	a->sleep = httpc_testing_sleep;
	a->time  = httpc_testing_time;

	{
		httpc_t h = { .os = a, };
		a->socketopts = &h;
		char buf[128] = { 0, };
		size_t buflen = sizeof buf;
		if (httpc_get_buffer(a, "identity.com", buf, &buflen) != HTTPC_OK) {
			r = error(&h, "httpc_get_buffer retrieval failed");
		} else {
			if (buflen != 10)
				r = error(&h, "unexpected length %u", (unsigned)buflen);
			else if (memcmp(buf, "0123456789", 10))
				r = error(&h, "unexpected data");
			else
				info(&h, "httpc_get_buffer passed");
		}
		a->socketopts = NULL;
	}

	static const struct url_test {
		char *url;
		char *domain /* or IPv4/IPv6 */, *userpass, *path;
		unsigned short port;
		int use_ssl, error;
	} url_tests[] = {
		{ .url = "example.com",                                  .domain = "example.com",   .userpass = NULL,            .path = "/",           .port = 80,  .use_ssl = 0, .error = 0, },
		{ .url = "example.co.uk",                                .domain = "example.co.uk", .userpass = NULL,            .path = "/",           .port = 80,  .use_ssl = 0, .error = 0, },
		{ .url = "user:password@example.com",                    .domain = "example.com",   .userpass = "user:password", .path = "/",           .port = 80,  .use_ssl = 0, .error = 0, },
		{ .url = "user:password@example.com:666",                .domain = "example.com",   .userpass = "user:password", .path = "/",           .port = 666, .use_ssl = 0, .error = 0, },
		{ .url = "http://example.com",                           .domain = "example.com",   .userpass = NULL,            .path = "/",           .port = 80,  .use_ssl = 0, .error = 0, },
		{ .url = "https://example.com",                          .domain = "example.com",   .userpass = NULL,            .path = "/",           .port = 443, .use_ssl = 1, .error = 0, },
		{ .url = "https://example.com:666",                      .domain = "example.com",   .userpass = NULL,            .path = "/",           .port = 666, .use_ssl = 1, .error = 0, },
		{ .url = "https://example.com:666/",                     .domain = "example.com",   .userpass = NULL,            .path = "/",           .port = 666, .use_ssl = 1, .error = 0, },
		{ .url = "https://example.com:666/index.html",           .domain = "example.com",   .userpass = NULL,            .path = "/index.html", .port = 666, .use_ssl = 1, .error = 0, },
		{ .url = "https://example.com/",                         .domain = "example.com",   .userpass = NULL,            .path = "/",           .port = 443, .use_ssl = 1, .error = 0, },
		{ .url = "https://example.com/index.html",               .domain = "example.com",   .userpass = NULL,            .path = "/index.html", .port = 443, .use_ssl = 1, .error = 0, },
		{ .url = "https://user:password@example.com/index.html", .domain = "example.com",   .userpass = "user:password", .path = "/index.html", .port = 443, .use_ssl = 1, .error = 0, },
		{ .url = "",                                             .domain = "",              .userpass = "",              .path = "",            .port = 0,   .use_ssl = 0, .error = 1, },
		{ .url = "https://user@password:example.com/index.html", .domain = "",              .userpass = "",              .path = "",            .port = 0,   .use_ssl = 0, .error = 1, },
	};
	const size_t url_tests_count = sizeof (url_tests) / sizeof (url_tests[0]);
	for (size_t i = 0; i < url_tests_count; i++) {
		httpc_t h = { .os = a, };
		const struct url_test *u = &url_tests[i];
		info(&h, "URL:       %s", u->url);
		const int rp = httpc_parse_url(&h, u->url);
		if (rp < 0) {
			if (u->error == 0)
				r = error(&h, "HTTP URL parsing failed");
			else
				info(&h, "expected and got an error");
			continue;
		}
		if (u->error) {
			r = error(&h, "expected an error and got none");
			continue;
		}
		if (strcmp(u->path, h.path))
			r = error(&h, "path mismatch:   '%s' != '%s'", u->path, h.path);
		if (strcmp(u->domain, h.domain))
			r = error(&h, "domain mismatch: '%s' != '%s'", u->domain, h.domain);
		if (u->port != h.port)
			r = error(&h, "port mismatch:   '%u' != '%u'", (unsigned) u->port, (unsigned) h.port);
		if (u->use_ssl != h.use_ssl)
			r = error(&h, "SSL mismatch:    '%u' != '%u'", (unsigned) u->use_ssl, (unsigned) h.use_ssl);
		if (u->userpass) {
			if (h.userpass == NULL) {
				r = error(&h, "user-pass mismatch: '%s' != NULL", u->userpass);
			} else {
				if (strcmp(u->userpass, h.userpass))
					r = error(&h, "user-pass mismatch: '%s' != '%s'", u->userpass, h.userpass);
			}
		}
		if (buffer_free(&h, &h.burl) < 0)
			r = HTTPC_ERROR;
		h.url = NULL;
	}

	static const struct file_test {
		const char *file;
		int error;
	} file_tests[] = {
		{ "example.com",          0, },
		{ "identity.com",         0, },
		{ "redirect.com",         0, },
		{ "not-found.com",       -1, },
		{ "404.com",           -404, },
	};
	const size_t file_tests_count = sizeof (file_tests) / sizeof (file_tests[0]);
  {
    for (size_t i = 0; i < file_tests_count; i++) {
      const struct file_test *ft = &file_tests[i];
      httpc_t h = { .os = a, };
      a->socketopts = &h;
      info(&h, "Test GET on URL '%s'", ft->file);
      const int code = httpc_get(a, ft->file, NULL, NULL);
      if (code != ft->error)
        r = error(&h, "Test GET on URL '%s' failed: got %d and expected %d", ft->file, code, ft->error);
      else
        info(&h, "Test GET on URL '%s' passed", ft->file);
      a->socketopts = NULL;
    }
  }
	return r;
}
