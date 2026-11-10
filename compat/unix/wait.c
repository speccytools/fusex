/* wait.c: native POSIX semaphore notification
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
#include "config.h"

#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>
#include <time.h>

#include "compat/wait.h"

struct compat_wait_t { sem_t semaphore; };
const int compat_wait_uses_address = 0;

compat_wait_t *
compat_wait_alloc( void )
{
  compat_wait_t *wait = malloc( sizeof( *wait ) );
  if( !wait ) return NULL;
  if( sem_init( &wait->semaphore, 0, 0 ) ) {
    int error = errno;
    free( wait );
    errno = error;
    return NULL;
  }
  return wait;
}

void
compat_wait_free( compat_wait_t *wait )
{
  if( !wait ) return;
  sem_destroy( &wait->semaphore );
  free( wait );
}

compat_wait_deadline
compat_wait_until( unsigned int milliseconds )
{
  struct timespec now;
  clock_gettime( CLOCK_REALTIME, &now );
  return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec +
         (uint64_t)milliseconds * 1000000;
}

int
compat_wait_expired( compat_wait_deadline end )
{
  struct timespec now;
  if( clock_gettime( CLOCK_REALTIME, &now ) ) return -errno;
  return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec >= end;
}

int
compat_wait_discard( compat_wait_t *wait )
{
  for(;;) {
    if( !sem_trywait( &wait->semaphore ) || errno == EINTR ) continue;
    return errno == EAGAIN ? 0 : errno;
  }
}

int
compat_wait_wake( compat_wait_t *wait, _Atomic uint32_t *address )
{
  (void)address;
  return sem_post( &wait->semaphore ) ? errno : 0;
}

int
compat_wait_park( compat_wait_t *wait, _Atomic uint32_t *address,
                  uint32_t expected, compat_wait_deadline end )
{
  struct timespec deadline;
  (void)address;
  (void)expected;
  deadline.tv_sec = end / 1000000000;
  deadline.tv_nsec = end % 1000000000;
  return sem_timedwait( &wait->semaphore, &deadline ) ? errno : 0;
}
