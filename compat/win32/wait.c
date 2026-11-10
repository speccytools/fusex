/* wait.c: Windows semaphore notification
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
#include <limits.h>
#include <stdlib.h>
#include <windows.h>

#include "compat/wait.h"

struct compat_wait_t { HANDLE semaphore; };
const int compat_wait_uses_address = 0;

compat_wait_t *
compat_wait_alloc( void )
{
  compat_wait_t *wait = malloc( sizeof( *wait ) );
  if( !wait ) return NULL;
  wait->semaphore = CreateSemaphore( NULL, 0, LONG_MAX, NULL );
  if( !wait->semaphore ) {
    free( wait );
    errno = EIO;
    return NULL;
  }
  return wait;
}

void
compat_wait_free( compat_wait_t *wait )
{
  if( !wait ) return;
  CloseHandle( wait->semaphore );
  free( wait );
}

compat_wait_deadline
compat_wait_until( unsigned int milliseconds )
{
  return GetTickCount64() + milliseconds;
}

int
compat_wait_expired( compat_wait_deadline end )
{
  return GetTickCount64() >= end;
}

int
compat_wait_discard( compat_wait_t *wait )
{
  DWORD status;
  do {
    status = WaitForSingleObject( wait->semaphore, 0 );
  } while( status == WAIT_OBJECT_0 );
  return status == WAIT_TIMEOUT ? 0 : EIO;
}

int
compat_wait_wake( compat_wait_t *wait, _Atomic uint32_t *address )
{
  (void)address;
  return ReleaseSemaphore( wait->semaphore, 1, NULL ) ? 0 : EIO;
}

int
compat_wait_park( compat_wait_t *wait, _Atomic uint32_t *address,
                  uint32_t expected, compat_wait_deadline end )
{
  ULONGLONG now = GetTickCount64();
  DWORD status;
  (void)address;
  (void)expected;
  status = now < end ?
           WaitForSingleObject( wait->semaphore,
                                (DWORD)( end - now ) ) : WAIT_TIMEOUT;
  return status == WAIT_OBJECT_0 ? 0 :
         status == WAIT_TIMEOUT ? ETIMEDOUT : EIO;
}
