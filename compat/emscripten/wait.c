/* wait.c: cooperative single-threaded browser audio waits
   Copyright (c) 2026 Fredrick Meunier

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
#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <emscripten.h>

#include "compat/wait.h"

struct compat_wait_t { int unused; };
/* Callbacks and the producer share one browser thread. They can interleave
   only when we yield, so comparison followed by cooperative sleep is safe.
   No retained semaphore tokens are needed. */
const int compat_wait_uses_address = 1;

static compat_wait_deadline
now_ns( void )
{
  return (compat_wait_deadline)( emscripten_get_now() * 1000000.0 );
}

compat_wait_t *
compat_wait_alloc( void )
{
  return malloc( sizeof( compat_wait_t ) );
}

void
compat_wait_free( compat_wait_t *wait )
{
  free( wait );
}

compat_wait_deadline
compat_wait_until( unsigned int milliseconds )
{
  return now_ns() + (uint64_t)milliseconds * 1000000;
}

int
compat_wait_expired( compat_wait_deadline end )
{
  return now_ns() >= end;
}

int
compat_wait_discard( compat_wait_t *wait )
{
  (void)wait;
  return 0;
}

int
compat_wait_wake( compat_wait_t *wait, _Atomic uint32_t *address )
{
  /* The callback publishes generation before calling us. Never suspend or
     re-enter the producer from the audio callback. */
  (void)wait;
  (void)address;
  return 0;
}

int
compat_wait_park( compat_wait_t *wait, _Atomic uint32_t *address,
                  uint32_t expected, compat_wait_deadline end )
{
  (void)wait;
  if( atomic_load( address ) != expected ) return 0;
  if( compat_wait_expired( end ) ) return ETIMEDOUT;

  /* Asyncify returns control to the browser so audio and input callbacks can
     run. Timers may be clamped, particularly in background tabs; the caller
     must recheck its predicate, never treat a timeout as admission. */
  emscripten_sleep( 1 );
  return compat_wait_expired( end ) ? ETIMEDOUT : 0;
}
