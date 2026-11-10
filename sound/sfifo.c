/* sfifo.c: byte-oriented single-producer, single-consumer FIFO
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
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "sfifo.h"

int
sfifo_init( sfifo_t *f, int size )
{
  f->buffer = NULL;
  f->size = 0;
  atomic_init( &f->readpos, 0 );
  atomic_init( &f->writepos, 0 );

  if( size <= 0 || size > SFIFO_MAX_BUFFER_SIZE ) return -EINVAL;
  if( !atomic_is_lock_free( &f->readpos ) ||
      !atomic_is_lock_free( &f->writepos ) ) return -ENOTSUP;

  /* Reserve one byte to distinguish full from empty. */
  f->size = 1;
  while( f->size <= size ) f->size *= 2;
  f->buffer = malloc( f->size );
  if( !f->buffer ) {
    f->size = 0;
    return -ENOMEM;
  }
  return 0;
}

void
sfifo_flush( sfifo_t *f )
{
  /* Quiescent only: no payload publication is needed. */
  atomic_store_explicit( &f->readpos, 0, memory_order_relaxed );
  atomic_store_explicit( &f->writepos, 0, memory_order_relaxed );
}

void
sfifo_close( sfifo_t *f )
{
  free( f->buffer );
  f->buffer = NULL;
  f->size = 0;
  sfifo_flush( f );
}

int
sfifo_producer_space( const sfifo_t *f )
{
  unsigned int writepos, readpos;
  if( !f->buffer ) return -ENODEV;
  writepos = atomic_load_explicit( &f->writepos, memory_order_relaxed );
  readpos = atomic_load_explicit( &f->readpos, memory_order_acquire );
  return f->size - 1 - ( ( writepos - readpos ) & ( f->size - 1 ) );
}

int
sfifo_consumer_used( const sfifo_t *f )
{
  unsigned int readpos, writepos;
  if( !f->buffer ) return -ENODEV;
  readpos = atomic_load_explicit( &f->readpos, memory_order_relaxed );
  writepos = atomic_load_explicit( &f->writepos, memory_order_acquire );
  return ( writepos - readpos ) & ( f->size - 1 );
}

int
sfifo_write( sfifo_t *f, const void *buf, int len )
{
  unsigned int pos;
  int available, first;
  if( len < 0 || ( len && !buf ) ) return -EINVAL;
  available = sfifo_producer_space( f );
  if( available < 0 ) return available;
  if( len > available ) len = available;
  if( !len ) return 0;

  pos = atomic_load_explicit( &f->writepos, memory_order_relaxed );
  first = f->size - pos;
  if( first > len ) first = len;
  memcpy( f->buffer + pos, buf, first );
  if( len > first )
    memcpy( f->buffer, (const char *)buf + first, len - first );
  /* Publish the payload only after both copies have completed. */
  atomic_store_explicit( &f->writepos, ( pos + len ) & ( f->size - 1 ),
                         memory_order_release );
  return len;
}

int
sfifo_read( sfifo_t *f, void *buf, int len )
{
  unsigned int pos;
  int available, first;
  if( len < 0 || ( len && !buf ) ) return -EINVAL;
  available = sfifo_consumer_used( f );
  if( available < 0 ) return available;
  if( len > available ) len = available;
  if( !len ) return 0;

  pos = atomic_load_explicit( &f->readpos, memory_order_relaxed );
  first = f->size - pos;
  if( first > len ) first = len;
  memcpy( buf, f->buffer + pos, first );
  if( len > first )
    memcpy( (char *)buf + first, f->buffer, len - first );
  /* Release storage only after the consumer has finished reading it. */
  atomic_store_explicit( &f->readpos, ( pos + len ) & ( f->size - 1 ),
                         memory_order_release );
  return len;
}
