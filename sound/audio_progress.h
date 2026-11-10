/* audio_progress.h: advisory callback consumer progress
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* Permission to produce always comes from the FIFO/pacing predicate,
   never from a notification. */
#ifndef FUSE_AUDIO_PROGRESS_H
#define FUSE_AUDIO_PROGRESS_H
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "compat/wait.h"

typedef compat_wait_deadline audio_progress_deadline;
struct audio_progress {
  _Alignas( 4 ) _Atomic uint32_t generation;
  _Atomic int error;
  _Atomic bool armed;
  compat_wait_t *wait;
};
_Static_assert( _Alignof( struct audio_progress ) >= 4,
                "Progress generation requires four-byte alignment" );
/* Generation equality is safe while the sole producer is parked: positive
   reads are bounded by the finite FIFO, plus at most two first sticky faults.
   Empty callbacks cannot advance it indefinitely. All accesses are SC. */

static inline int
audio_progress_init( struct audio_progress *p )
{
  atomic_init( &p->generation, 0 );
  atomic_init( &p->error, 0 );
  atomic_init( &p->armed, false );
  p->wait = NULL;
  if( !atomic_is_lock_free( &p->generation ) ||
      !atomic_is_lock_free( &p->error ) ||
      !atomic_is_lock_free( &p->armed ) ) return 1;
  p->wait = compat_wait_alloc();
  return !p->wait;
}

/* Only the producer owns lifecycle. End/init cannot overlap a reservation or
   publication. Stop and join callbacks before closing/resetting this state;
   failed Core Audio disposal retains it, just like the FIFO. */
static inline void
audio_progress_close( struct audio_progress *p )
{
  compat_wait_free( p->wait );
  p->wait = NULL;
}

static inline uint32_t
audio_progress_snapshot( struct audio_progress *p )
{
  return atomic_load( &p->generation );
}

static inline void
audio_progress_publish( struct audio_progress *p )
{
  int error = 0;
  atomic_fetch_add( &p->generation, 1 );
  /* Address comparison retains earlier progress without a notification.
     Token-based primitives need one retained signal after arming, not an
     ever-growing bank of permits from idle callbacks. */
  if( compat_wait_uses_address || atomic_exchange( &p->armed, false ) )
    error = compat_wait_wake( p->wait, &p->generation );
  if( error ) atomic_store( &p->error, error );
}

static inline audio_progress_deadline
audio_progress_until( void )
{
  return compat_wait_until( 10 );
}

/* Single producer, serialized callbacks. Keep the same absolute deadline
   across insufficient/spurious returns; the caller always rechecks admission. */
static inline int
audio_progress_wait( struct audio_progress *p, uint32_t expected,
                     audio_progress_deadline end )
{
  int result;
  if( !compat_wait_uses_address ) {
    /* Drain BEFORE arming. A callback that claimed an earlier arm may still
       deliver one late token: harmless spurious success. Never drain after
       the final generation check, which could discard a needed signal. */
    result = compat_wait_discard( p->wait );
    if( result ) return result;
  }
  result = compat_wait_expired( end );
  if( result ) return result < 0 ? -result : ETIMEDOUT;
  if( compat_wait_uses_address )
    return compat_wait_park( p->wait, &p->generation, expected, end );

  atomic_store( &p->armed, true );
  result = audio_progress_snapshot( p ) == expected ?
           compat_wait_park( p->wait, &p->generation, expected, end ) : 0;
  atomic_store( &p->armed, false );
  return result;
}

#endif
