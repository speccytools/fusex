/* ssh.h: SSH socket offload for W5100 emulation (libssh2 + mbedTLS)
   
   ZX programs connect to TCP port 22 and see the Spectranext control-line
   protocol documented in docs/api/offload/ssh.md.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef FUSE_SSH_H
#define FUSE_SSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "compat.h"

#include <libssh2.h>

typedef struct ssh_socket_t ssh_socket_t;

typedef int (*ssh_zx_send_cb_t)( void *ctx, const uint8_t *data, size_t len );

ssh_socket_t *ssh_socket_allocate( const char *host, uint16_t port,
                                   const char *username );
void ssh_socket_free( ssh_socket_t *ssh );

int ssh_socket_init( void );

int ssh_socket_connect( ssh_socket_t *ssh, compat_socket_t socket_fd );
ssize_t ssh_socket_send( ssh_socket_t *ssh, const void *buf, size_t len );
ssize_t ssh_socket_recv( ssh_socket_t *ssh, void *buf, size_t len );
int ssh_socket_close( ssh_socket_t *ssh );

bool ssh_socket_connected( const ssh_socket_t *ssh );
bool ssh_socket_has_pending_control( const ssh_socket_t *ssh );
ssize_t ssh_socket_feed_control( ssh_socket_t *ssh, const uint8_t *buf,
                                 size_t len );
const char *ssh_socket_last_error( const ssh_socket_t *ssh );

/* Shared trust-on-first-use host-key check for other SSH-based services. */
int ssh_session_verify_or_trust_host( LIBSSH2_SESSION *session,
                                      const char *host, uint16_t port );

#endif /* FUSE_SSH_H */
