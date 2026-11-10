/* pcm_fifo.h: policy-neutral whole-PCM-frame access to sfifo

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#ifndef FUSE_PCM_FIFO_H
#define FUSE_PCM_FIFO_H

#include "sfifo.h"

/* A PCM frame contains all channels for one sample instant. bytes_per_frame
   must be nonzero, fit in int, and remain identical at both endpoints for the
   FIFO's active lifetime. Use only these transfers on a PCM FIFO: mixing byte
   transfers or changing geometry can destroy frame alignment.

   Ownership, publication and quiescent lifecycle rules are those of sfifo.
   Space is a producer observation; used is a consumer observation. Capacity
   is immutable while active. Counts and successful transfers are in frames;
   failures are negative errno values. Unusable trailing bytes are not frames.
   These helpers neither wait nor silence output nor choose a buffering policy.
   Whole-frame partial transfers are allowed, including across byte-ring wrap.
 */
static inline int
pcm_fifo_valid_width( unsigned int bytes_per_frame )
{
  return bytes_per_frame && bytes_per_frame <= INT_MAX;
}

static inline int
pcm_fifo_capacity( const sfifo_t *fifo, unsigned int bytes_per_frame )
{
  if( !pcm_fifo_valid_width( bytes_per_frame ) ) return -EINVAL;
  if( !fifo->buffer ) return -ENODEV;
  return ( fifo->size - 1 ) / (int)bytes_per_frame;
}

static inline int
pcm_fifo_producer_space( const sfifo_t *fifo, unsigned int bytes_per_frame )
{
  int bytes;
  if( !pcm_fifo_valid_width( bytes_per_frame ) ) return -EINVAL;
  bytes = sfifo_producer_space( fifo );
  return bytes < 0 ? bytes : bytes / (int)bytes_per_frame;
}

/* Sole-producer physical fit query: 1 if frames fit now, 0 if a valid
   request needs more space, negative errno if invalid or larger than total
   PCM-frame capacity. This is an SPSC geometry query, not a reservation
   object or latency/pacing policy; it neither waits nor changes FIFO state.

   Storage and geometry must remain unchanged through subsequent publication,
   with only the checked batch written by the sole producer. A stale consumer
   position can only understate free space. After a successful B-frame fit
   check, concurrent consumption can only increase space. Once the same
   producer has published k frames of that batch, at least B-k frames remain
   physically fundable; split whole-frame writes are therefore safe. */
static inline int
pcm_fifo_producer_can_write( const sfifo_t *fifo, unsigned int bytes_per_frame,
                             unsigned int frames )
{
  int capacity, space;
  if( !fifo ) return -EINVAL;
  capacity = pcm_fifo_capacity( fifo, bytes_per_frame );
  if( capacity < 0 ) return capacity;
  if( frames > (unsigned int)capacity ) return -EINVAL;
  space = pcm_fifo_producer_space( fifo, bytes_per_frame );
  return space < 0 ? space : (unsigned int)space >= frames;
}

static inline int
pcm_fifo_consumer_used( const sfifo_t *fifo, unsigned int bytes_per_frame )
{
  int bytes;
  if( !pcm_fifo_valid_width( bytes_per_frame ) ) return -EINVAL;
  bytes = sfifo_consumer_used( fifo );
  return bytes < 0 ? bytes : bytes / (int)bytes_per_frame;
}

static inline int
pcm_fifo_write( sfifo_t *fifo, unsigned int bytes_per_frame,
                const void *data, unsigned int frames )
{
  int available, bytes;
  if( frames && !data ) return -EINVAL;
  available = pcm_fifo_producer_space( fifo, bytes_per_frame );
  if( available < 0 ) return available;
  if( frames > INT_MAX / bytes_per_frame ) return -EINVAL;
  if( frames > (unsigned int)available ) frames = available;
  /* Clamping in frames precedes the byte transfer. Only the consumer can
     change space in between, and it can only increase it. */
  bytes = sfifo_write( fifo, data, frames * bytes_per_frame );
  return bytes < 0 ? bytes : bytes / (int)bytes_per_frame;
}

static inline int
pcm_fifo_read( sfifo_t *fifo, unsigned int bytes_per_frame,
               void *data, unsigned int frames )
{
  int available, bytes;
  if( frames && !data ) return -EINVAL;
  available = pcm_fifo_consumer_used( fifo, bytes_per_frame );
  if( available < 0 ) return available;
  if( frames > INT_MAX / bytes_per_frame ) return -EINVAL;
  if( frames > (unsigned int)available ) frames = available;
  /* Only the producer can change occupancy in between, and it can only
     increase it. sfifo's release store publishes consumption after copying. */
  bytes = sfifo_read( fifo, data, frames * bytes_per_frame );
  return bytes < 0 ? bytes : bytes / (int)bytes_per_frame;
}

#endif
