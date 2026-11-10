/* Cooperative wait regression: compile with emcc -sASYNCIFY and run in Node.
   Copyright (c) 2026 Fredrick Meunier
   SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <emscripten.h>

#include "compat/wait.h"

static _Atomic uint32_t generation;
static compat_wait_t *waiter;
static int callback_ran;

static void
publish( void *unused )
{
  (void)unused;
  atomic_fetch_add( &generation, 1 );
  assert( !compat_wait_wake( waiter, &generation ) );
  callback_ran = 1;
}

int
main( void )
{
  compat_wait_deadline end;
  waiter = compat_wait_alloc();
  assert( waiter );
  assert( compat_wait_uses_address );
  atomic_init( &generation, 0 );
  assert( !compat_wait_discard( waiter ) );

  /* An expired deadline must not suspend. */
  end = compat_wait_until( 0 );
  assert( compat_wait_expired( end ) );
  assert( compat_wait_park( waiter, &generation, 0, end ) == ETIMEDOUT );
  /* Progress wins even when the deadline has expired. */
  atomic_store( &generation, 1 );
  assert( !compat_wait_park( waiter, &generation, 0, end ) );

  /* A JS event must be able to run while the producer is waiting. */
  emscripten_async_call( publish, NULL, 5 );
  end = compat_wait_until( 1000 );
  while( atomic_load( &generation ) == 1 ) {
    int result = compat_wait_park( waiter, &generation, 1, end );
    assert( !result );
  }
  assert( callback_ran );

  end = compat_wait_until( 5 );
  while( !compat_wait_expired( end ) ) {
    int result = compat_wait_park( waiter, &generation, 2, end );
    assert( !result || result == ETIMEDOUT );
  }
  compat_wait_free( waiter );
  return 0;
}
