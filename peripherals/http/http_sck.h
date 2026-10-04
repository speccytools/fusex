#pragma once

#include "httpc.h"

extern httpc_options_t tls_sck;
void sck_http_options_init(httpc_options_t *options);
int sck_http_open(httpc_options_t *options, void **socket, void *opts,
                  const char *domain, unsigned short port, int use_ssl);
int sck_http_close(httpc_options_t *options, void *socket);

/* Wake a file download blocked in recv without freeing its socket. */
void sck_http_abort(void *socket);
