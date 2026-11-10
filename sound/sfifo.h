/* sfifo.h: byte-oriented single-producer, single-consumer FIFO
 * Copyright (c) 2000-2002 David Olofson
 * Modifications by Philip Kendall (c) 2007
 * Originally released under the GNU LESSER GENERAL PUBLIC LICENSE Version 2.1.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifndef _SFIFO_H_
#define _SFIFO_H_

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>

#ifdef __KERNEL__
#error "This FIFO requires userspace C11 atomics"
#endif

/* Largest requested capacity whose next power of two fits in int. */
#define SFIFO_MAX_BUFFER_SIZE (INT_MAX / 2)

typedef struct sfifo_t
{
  char *buffer;
  int size;
  _Atomic unsigned int readpos;
  _Atomic unsigned int writepos;
} sfifo_t;

/* init, flush, close and reinitialization require both endpoints quiescent.
 * Close before reinitializing. Do not copy an initialized FIFO. Metadata is
 * immutable while active. Initialization rejects non-lock-free positions.
 */
int sfifo_init( sfifo_t *f, int size );
void sfifo_close( sfifo_t *f );
void sfifo_flush( sfifo_t *f );

/* Exactly one producer calls write/producer_space, and exactly one consumer
 * calls read/consumer_used. Queries are endpoint observations, not coherent
 * snapshots for third-party observers. Transfers are in bytes, may be partial,
 * and return a negative errno on error. Zero-length transfers permit NULL.
 */
int sfifo_write( sfifo_t *f, const void *buf, int len );
int sfifo_read( sfifo_t *f, void *buf, int len );
int sfifo_producer_space( const sfifo_t *f );
int sfifo_consumer_used( const sfifo_t *f );

#endif
