/* alsapacingtest.c: native PCM observation/controller regressions
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
#include <stdio.h>
#include <stdlib.h>
#include "sound/alsa_pacing.h"

#define CHECK( x ) do { if( !( x ) ) { \
                          fprintf( stderr, "alsapacingtest:%d: %s\n", __LINE__, \
                                   #x ); exit( 1 ); \
                        } } while( 0 )

static void
publication( struct alsa_pacing *p, unsigned int before, unsigned int after,
             unsigned int batch )
{
  CHECK( !alsa_pacing_snapshot( p, before ) );
  CHECK( !alsa_pacing_pending( p, batch, true, true ) );
  CHECK( before + batch <= alsa_pacing_limit( p ) );
  p->reserved = true;
  alsa_pacing_accept( p, batch );
  CHECK( !alsa_pacing_snapshot( p, after ) );
  CHECK( !alsa_pacing_publication( p, before, after, true, true ) );
}

int
main( void )
{
  struct alsa_pacing p;
  unsigned int i, target;
  CHECK( alsa_pacing_init( &p, 900, 241, 512, 960,
                           ALSA_PACING_ADAPTIVE, 0 ) );
  CHECK( alsa_pacing_init( &p, 2047, 241, 512, 960,
                           ALSA_PACING_FIXED, 240 ) );
  CHECK( !alsa_pacing_init( &p, 2047, 241, 512, 960,
                            ALSA_PACING_ADAPTIVE, 0 ) );
  /* No playback progress: startup cannot qualify. */
  publication( &p, 1750, 1991, 241 );
  CHECK( !p.normal );
  /* Real progress plus a near-target complete refill qualifies startup. */
  publication( &p, 1806, 2046, 241 );
  CHECK( p.normal );
  for( i = 0; i < 5000; i++ ) {
    target = p.controller.target;
    publication( &p, target - 241, target - 10, 241 );
    CHECK( p.controller.target <= 2047 );
  }
  CHECK( p.controller.target == adaptive_minimum( &p.controller ) );
  CHECK( p.controller.target < 2047 );
  CHECK( p.controller.state == ADAPTIVE_NORMAL );
  /* Reported interval low water protects, but cannot fund evidence. */
  alsa_pacing_low_water( &p, 0, false );
  CHECK( p.controller.target < 2047 );
  alsa_pacing_low_water( &p, 0, true );
  CHECK( p.controller.target == 2047 && !p.controller.funded );
  CHECK( !alsa_pacing_pending( &p, 960, true, true ) );
  CHECK( !p.ordinary && alsa_pacing_limit( &p ) == 2047 );
  alsa_pacing_restart( &p );
  CHECK( !p.normal && !p.reserved && !p.have_queue );
  CHECK( !alsa_pacing_snapshot( &p, 100 ) );
  CHECK( alsa_pacing_snapshot( &p, 101 ) );
  alsa_pacing_restart( &p );
  CHECK( !alsa_pacing_snapshot( &p, 100 ) );
  alsa_pacing_accept( &p, 40 );
  CHECK( !alsa_pacing_snapshot( &p, 130 ) && p.progress );
  alsa_pacing_accept( &p, 60 );
  CHECK( !alsa_pacing_snapshot( &p, 180 ) );
  /* Progress during the write is not automatically ambiguity. */
  p.normal = true;
  p.have_queue = false;
  adaptive_activity( &p.controller );
  publication( &p, 1806, 2037, 241 );
  CHECK( p.controller.funded );
  CHECK( !alsa_pacing_pending( &p, 241, true, true ) );
  p.reserved = true;
  CHECK( !alsa_pacing_publication( &p, 100, 200, true, true ) );
  CHECK( !p.controller.funded );
  CHECK( !alsa_pacing_pending( &p, 241, true, true ) );
  p.reserved = true;
  CHECK( alsa_pacing_publication( &p, 100, 342, true, true ) );
  alsa_pacing_restart( &p );
  CHECK( !alsa_pacing_init( &p, 2047, 241, 512, 960,
                            ALSA_PACING_FIXED, 1400 ) );
  p.normal = true;
  CHECK( !alsa_pacing_pending( &p, 241, true, true ) );
  CHECK( alsa_pacing_limit( &p ) == 1400 );
  CHECK( !alsa_pacing_pending( &p, 960, true, true ) );
  CHECK( alsa_pacing_limit( &p ) == 2047 );
  puts(
    "ALSA pacing accounting, qualification, convergence and protection passed" );
  return 0;
}
