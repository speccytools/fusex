/* wait.h: single-waiter notification and deadline compatibility
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
#ifndef FUSE_COMPAT_WAIT_H
#define FUSE_COMPAT_WAIT_H
#include <stdint.h>
#include <stdatomic.h>

typedef struct compat_wait_t compat_wait_t;
/* Opaque clock value; use only with this API's deadline operations. */
typedef uint64_t compat_wait_deadline;

/* When nonzero, park must not miss progress published between the caller's
   final generation check and waiting on the address. When zero, wake retains
   a notification until consumed or discarded; the caller must arrange its own
   final predicate/generation check after preparing to receive notifications.
   One waiter and serialized notifications are required. The address must be
   four-byte aligned and remain alive until waiters/notifiers are quiescent. */
extern const int compat_wait_uses_address;
/* Allocate/free on the owner thread, never from a notifier or live waiter. */
compat_wait_t *compat_wait_alloc( void );
void compat_wait_free( compat_wait_t *wait );
compat_wait_deadline compat_wait_until( unsigned int milliseconds );
/* 0/1 for not expired/expired; negative errno on clock failure. */
int compat_wait_expired( compat_wait_deadline end );
/* Drain obsolete tokens before arranging a new notification. No-op for
   address waits. Never call after the final predicate check. */
int compat_wait_discard( compat_wait_t *wait );
/* Return 0 or positive errno. Wake does not allocate, park, or acquire an
   application mutex; it is not required to provide hard realtime guarantees.
   Park may return spuriously. Callers must recheck their progress/admission
   predicate after every return; a timeout does not grant admission. */
int compat_wait_wake( compat_wait_t *wait, _Atomic uint32_t *address );
int compat_wait_park( compat_wait_t *wait, _Atomic uint32_t *address,
                      uint32_t expected, compat_wait_deadline end );
#endif
