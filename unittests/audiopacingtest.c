/* Deterministic preventive controller and real FIFO publication tests.
   GPL-2.0-or-later. No device, wall-clock policy or injected production faults. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif
#include "sound/audio_pacing.h"
#define CHECK(x) do { if( !(x) ) { fprintf( stderr, "%d: %s\n", \
  __LINE__, #x ); exit( 1 ); } } while( 0 )
static unsigned char data[8192];

static void
observe( struct adaptive_controller *s, enum publication_evidence kind,
         unsigned int q, unsigned int after, unsigned int progress,
         bool uncontaminated )
{
  struct adaptive_sample o = { kind, q, after, progress, s->epoch,
                              true, true, true, uncontaminated, false };
  adaptive_observe( s, o );
}
static void
cadence_tests( void )
{
  static const struct {
    libspectrum_dword clock, interval;
    unsigned int allowance;
  } cases[] = {
    { 3500000, 17500, 241 },
    { 7000000, 35000, 241 },
    { 14000000, 70000, 241 },
    { 28000000, 140000, 241 },
    /* Fractional T-state cadence rounds down, so the explicit PCM formula
       is just below 240 rather than exactly 240 at this clock. */
    { 3500123, 17500, 240 },
    { UINT32_MAX, 21474836, 240 }
  };
  struct audio_pacing p;
  sfifo_t fifo;
  unsigned int i;
  CHECK( SOUND_AUDIO_CADENCE_MS == 5 );
  CHECK( !sfifo_init( &fifo, 4095 ) );
  for( i = 0; i < sizeof( cases ) / sizeof( cases[0] ); i++ ) {
    CHECK( sound_audio_interval_tstates( cases[i].clock ) == cases[i].interval );
    CHECK( !audio_pacing_init_rate( &p, &fifo, 2, 48000, cases[i].clock,
                                   69888, 512 ) );
    CHECK( p.controller.allowance == cases[i].allowance );
    CHECK( (libspectrum_qword)48000 * cases[i].interval / cases[i].clock + 1 ==
           p.controller.allowance );
  }
  CHECK( !sound_audio_interval_tstates( 0 ) );
  CHECK( !sound_audio_interval_tstates( 199 ) );
  CHECK( audio_pacing_init_rate( &p, &fifo, 2, 48000, 199, 69888, 512 ) < 0 );
  sfifo_close( &fifo );
}

static void
controller_tests( void )
{
  struct adaptive_controller s;
  unsigned int i, progress = 1, probes = 0;
  CHECK( adaptive_init( &s, 2047, 241, 512 ) == 0 );
  CHECK( s.target == 2047 && s.state == ADAPTIVE_STARTUP && !s.funded );
  CHECK( s.structure == 752 && s.step == 64 && s.floor == 512 );
  CHECK( !adaptive_probe( &s ) );
  observe( &s, PUBLICATION_WARNING, 237, 475, progress, true );
  CHECK( !s.excess && s.target == 2047 ); /* No startup evidence. */
  observe( &s, PUBLICATION_UNDERRUN, 0, 240, progress, true );
  CHECK( s.state == ADAPTIVE_STARTUP && !s.excess );
  adaptive_activity( &s );
  CHECK( s.state == ADAPTIVE_QUALIFY && !adaptive_probe( &s ) );
  while( s.target > 1328 ) {
    observe( &s, PUBLICATION_BASELINE, s.target - 240, s.target, progress++, true );
    CHECK( s.funded );
    for( i = 0; i < 128; i++ ) {
      observe( &s, PUBLICATION_CLEAN, s.target - 240, s.target, progress++, true );
      if( i < 127 ) CHECK( !adaptive_probe( &s ) );
    }
    CHECK( adaptive_probe( &s ) && !s.funded && !s.clean );
    CHECK( s.candidate == s.target && s.target >= 1328 );
    observe( &s, PUBLICATION_BASELINE, s.target - 240, s.target, progress++, true );
    CHECK( s.funded );
    /* Duplicate progress cannot multiply credit. */
    observe( &s, PUBLICATION_CLEAN, s.target - 240, s.target, s.baseline, true );
    CHECK( s.clean == 0 );
    for( i = 0; i < 128; i++ )
      observe( &s, PUBLICATION_CLEAN, s.target - 240, s.target, progress++, true );
    CHECK( !s.candidate && s.accepted == s.target && s.state == ADAPTIVE_NORMAL );
    probes++;
  }
  CHECK( probes == 12 && s.target == 1328 && s.recovery == 1343 );
  CHECK( s.floor == 512 && !s.excess && !adaptive_probe( &s ) );
  /* Absence with no successful excess invents no requirement. */
  observe( &s, PUBLICATION_NONE, 749, 989, progress++, true );
  CHECK( !s.excess && s.floor == 512 );
  observe( &s, PUBLICATION_WARNING, 237, 475, progress++, true );
  CHECK( s.excess == 339 && s.floor == 915 && s.target == 1667 );
  CHECK( s.state == ADAPTIVE_QUALIFY && !s.funded && !s.clean );
  observe( &s, PUBLICATION_WARNING, 475, 715, progress++, true );
  CHECK( s.target == 2047 && s.state == ADAPTIVE_HOLD );
  CHECK( s.excess == 339 && s.floor == 915 );
  adaptive_activity( &s );
  observe( &s, PUBLICATION_BASELINE, 1674, 1914, progress++, true );
  CHECK( s.funded && s.state == ADAPTIVE_NORMAL );
  for( i = 0; i < 128; i++ )
    observe( &s, PUBLICATION_CLEAN, 1807, 2047, progress++, true );
  CHECK( s.floor == 915 && s.excess == 339 && adaptive_probe( &s ) );
  observe( &s, PUBLICATION_AMBIGUOUS, 1700, 1940, progress++, false );
  CHECK( s.target == s.accepted && !s.candidate && !s.funded );
  CHECK( s.excess == 339 && s.floor == 915 );
  adaptive_activity( &s );
  observe( &s, PUBLICATION_BASELINE, 1807, 2047, progress++, true );
  for( i = 0; i < 128; i++ )
    observe( &s, PUBLICATION_CLEAN, 1807, 2047, progress++, true );
  CHECK( adaptive_probe( &s ) );
  adaptive_exceptional( &s );
  CHECK( !s.candidate && !s.funded );
  CHECK( s.target == 2047 && s.floor == 915 );
  observe( &s, PUBLICATION_WARNING, 237, 477, progress++, false );
  CHECK( s.state == ADAPTIVE_HOLD && s.excess == 339 );
  adaptive_activity( &s );
  observe( &s, PUBLICATION_UNDERRUN, 0, 240, progress++, true );
  CHECK( s.state == ADAPTIVE_EMERGENCY && s.target == 2047 );
  CHECK( s.excess == 339 );
  adaptive_fallback( &s );
  CHECK( s.state == ADAPTIVE_FALLBACK && s.floor == 915 );
  CHECK( !adaptive_init( &s, 2047, 241, 512 ) );
  CHECK( s.state == ADAPTIVE_STARTUP && !s.excess && s.floor == 512 );
  CHECK( adaptive_init( &s, 0, 241, 512 ) < 0 );
  CHECK( adaptive_init( &s, 2047, 0, 512 ) < 0 );
  CHECK( adaptive_init( &s, 2047, 241, 0 ) < 0 );
  CHECK( adaptive_init( &s, 2047, 2048, 512 ) < 0 );
  CHECK( !adaptive_init( &s, 2047, 1010, 512 ) );
  adaptive_activity( &s );
  CHECK( s.state == ADAPTIVE_LIMITED && !adaptive_probe( &s ) );
  CHECK( !adaptive_init( &s, 2096, 1010, 512 ) );
  adaptive_activity( &s ); CHECK( s.state == ADAPTIVE_LIMITED );
  CHECK( !adaptive_init( &s, 2097, 1010, 512 ) );
  adaptive_activity( &s ); CHECK( s.state == ADAPTIVE_QUALIFY );
  CHECK( !adaptive_init( &s, 2047, 241, 512 ) );
  adaptive_activity( &s );
  observe( &s, PUBLICATION_BASELINE, 1807, 2047, progress++, true );
  observe( &s, PUBLICATION_WARNING, 0, 240, progress++, true );
  CHECK( s.target == 2047 && s.state == ADAPTIVE_LIMITED );
  CHECK( s.excess == 1295 && s.floor == 1871 && adaptive_minimum( &s ) == 2623 );
  CHECK( !adaptive_init( &s, 2047, 241, UINT_MAX ) );
  adaptive_activity( &s );
  CHECK( s.state == ADAPTIVE_LIMITED && s.target == 2047 );
}

static void
consume( struct audio_pacing *p, sfifo_t *fifo, unsigned int count )
{
  int read;
  unsigned char output[8192];
  audio_pacing_callback_begin( p );
  read = pcm_fifo_read( fifo, 2, output, count );
  CHECK( read >= 0 );
  audio_pacing_callback_end( p, count, read, false );
}
static void
publish( struct audio_pacing *p, sfifo_t *fifo, unsigned int count,
         bool normal, bool started )
{
  CHECK( !audio_pacing_pending( p, fifo, 2, count, normal, started ) );
  CHECK( audio_pacing_can_admit( p, fifo, 2, count ) == 1 );
  p->reserved = true;
  CHECK( audio_pacing_write( p, fifo, 2, data, count, started ) == (int)count );
}
static void
adapter_tests( void )
{
  struct audio_pacing p;
  sfifo_t fifo;
  unsigned int i;
  CHECK( !sfifo_init( &fifo, 4095 ) );
  CHECK( !audio_pacing_init( &p, 2047, 241, 512, 961 ) );
  CHECK( p.controller.target == 2047 && !p.normal );
  publish( &p, &fifo, 960, false, false );
  CHECK( p.controller.state == ADAPTIVE_STARTUP && !p.controller.excess );
  publish( &p, &fifo, 240, true, true );
  consume( &p, &fifo, 512 );
  publish( &p, &fifo, 240, true, true );
  CHECK( p.normal && p.controller.state == ADAPTIVE_QUALIFY );
  publish( &p, &fifo, 240, true, true );
  publish( &p, &fifo, 240, true, true );
  publish( &p, &fifo, 240, true, true );
  publish( &p, &fifo, 240, true, true );
  CHECK( p.controller.funded && p.controller.state == ADAPTIVE_NORMAL );
  /* Seed the accepted working point, without borrowing startup evidence. */
  sfifo_flush( &fifo );
  CHECK( pcm_fifo_write( &fifo, 2, data, 1261 ) == 1261 );
  p.controller.target = p.controller.accepted = 1328;
  p.controller.recovery = 1343;
  p.controller.funded = true; p.controller.state = ADAPTIVE_NORMAL;
  p.quarantine = false;
  consume( &p, &fifo, 512 ); consume( &p, &fifo, 512 );
  publish( &p, &fifo, 238, true, true );
  CHECK( p.controller.target == 1667 && p.controller.excess == 339 );
  CHECK( p.controller.floor == 915 && !p.controller.funded );
  publish( &p, &fifo, 240, true, true );
  CHECK( p.controller.target == 2047 && p.controller.state == ADAPTIVE_HOLD );
  CHECK( p.controller.excess == 339 );
  for( i = 0; i < 5; i++ ) publish( &p, &fifo, 240, true, true );
  CHECK( p.controller.funded && p.controller.state == ADAPTIVE_NORMAL );
  CHECK( p.controller.excess == 339 && p.controller.floor == 915 );
  /* Ordinary target block, then real consumer progress makes it fundable. */
  sfifo_flush( &fifo );
  CHECK( pcm_fifo_write( &fifo, 2, data, 1200 ) == 1200 );
  CHECK( !adaptive_init( &p.controller, 2047, 241, 512 ) );
  p.controller.state = ADAPTIVE_NORMAL;
  p.controller.target = p.controller.accepted = 1328;
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 240, true, true ) );
  CHECK( !audio_pacing_can_admit( &p, &fifo, 2, 240 ) );
  consume( &p, &fifo, 512 );
  CHECK( audio_pacing_can_admit( &p, &fifo, 2, 240 ) == 1 );
  p.reserved = true;
  CHECK( audio_pacing_write( &p, &fifo, 2, data, 240, true ) == 240 );
  /* Exact full-frame/prefix batch exceeds ordinary target but fits C. */
  sfifo_flush( &fifo );
  CHECK( pcm_fifo_write( &fifo, 2, data, 765 ) == 765 );
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 959, true, true ) );
  CHECK( !p.ordinary && audio_pacing_can_admit( &p, &fifo, 2, 959 ) == 1 );
  CHECK( p.controller.target == 1328 );
  p.reserved = true;
  CHECK( audio_pacing_write( &p, &fifo, 2, data, 959, true ) == 959 );
  CHECK( audio_pacing_occupancy( &fifo, 2 ) == 1724 );
  CHECK( p.controller.target == 1328 && p.controller.excess == 0 );
  sfifo_flush( &fifo );
  CHECK( pcm_fifo_write( &fifo, 2, data, 600 ) == 600 );
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 1400, true, true ) );
  CHECK( !p.ordinary && audio_pacing_can_admit( &p, &fifo, 2, 1400 ) == 1 );
  p.reserved = true;
  CHECK( audio_pacing_write( &p, &fifo, 2, data, 1400, true ) == 1400 );
  CHECK( p.controller.target == 1328 && !p.controller.excess );
  CHECK( audio_pacing_pending( &p, &fifo, 2, 2048, true, true ) == -EINVAL );
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 959, true, true ) );
  CHECK( !audio_pacing_can_admit( &p, &fifo, 2, 959 ) );
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 0, true, true ) );
  CHECK( audio_pacing_can_admit( &p, &fifo, 2, 0 ) == 1 );
  /* RZX/frame-only context is capacity-paced, not ordinary. */
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 240, false, true ) );
  CHECK( !p.ordinary && p.controller.target == 2047 );
  CHECK( p.controller.state == ADAPTIVE_FALLBACK );
  sfifo_flush( &fifo );
  consume( &p, &fifo, 512 );
  publish( &p, &fifo, 240, true, true );
  CHECK( !p.controller.excess );
  audio_pacing_callback_begin( &p );
  audio_pacing_callback_end( &p, 513, 513, false );
  CHECK( atomic_load( &p.invalid ) );
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 240, true, true ) );
  CHECK( p.controller.state == ADAPTIVE_INVALID );
  sfifo_close( &fifo );
}

static void
convergence_tests( void )
{
  struct audio_pacing p;
  sfifo_t fifo;
  unsigned int transactions;
  CHECK( !sfifo_init( &fifo, 4095 ) );
  CHECK( !audio_pacing_init_rate( &p, &fifo, 2, 48000, 3500000, 69888, 512 ) );
  CHECK( p.controller.allowance == 241 && p.startup_bound == 959 );
  publish( &p, &fifo, 959, false, false );
  for( transactions = 0; transactions < 20000; transactions++ ) {
    CHECK( !audio_pacing_pending( &p, &fifo, 2, 240, true, true ) );
    if( !audio_pacing_can_admit( &p, &fifo, 2, 240 ) ) consume( &p, &fifo, 512 );
    CHECK( audio_pacing_can_admit( &p, &fifo, 2, 240 ) == 1 );
    p.reserved = true;
    CHECK( audio_pacing_write( &p, &fifo, 2, data, 240, true ) == 240 );
    if( p.controller.accepted == 1328 && !p.controller.candidate ) break;
  }
  CHECK( transactions < 20000 && p.controller.funded );
  CHECK( !p.controller.excess && p.controller.floor == 512 );
  CHECK( !atomic_load( &p.missing ) && !atomic_load( &p.invalid ) );
  /* Real publication-window overlap must cancel candidate credit. */
  p.controller.clean = 128;
  p.controller.target = p.controller.accepted = 1400;
  CHECK( adaptive_probe( &p.controller ) );
  p.quarantine = true;
  sfifo_flush( &fifo );
  CHECK( pcm_fifo_write( &fifo, 2, data, 900 ) == 900 );
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 240, true, true ) );
  p.reserved = true;
  atomic_store( &p.active, 1 );
  CHECK( audio_pacing_write( &p, &fifo, 2, data, 240, true ) == 240 );
  atomic_store( &p.active, 0 );
  CHECK( !p.controller.candidate && p.controller.target == 1400 );
  CHECK( !p.controller.funded && !p.controller.excess );
  /* A contaminated low-Q warning chooses HOLD without calibrating severity. */
  sfifo_flush( &fifo );
  p.controller.state = ADAPTIVE_NORMAL; p.controller.funded = true;
  CHECK( !audio_pacing_pending( &p, &fifo, 2, 240, true, true ) );
  p.reserved = true;
  atomic_store( &p.active, 1 );
  CHECK( audio_pacing_write( &p, &fifo, 2, data, 240, true ) == 240 );
  atomic_store( &p.active, 0 );
  CHECK( p.controller.state == ADAPTIVE_HOLD && p.controller.target == 2047 );
  CHECK( !p.controller.excess && p.controller.floor == 512 );
  CHECK( audio_pacing_init_rate( &p, &fifo, 2, 48000, 0, 69888, 512 ) < 0 );
  CHECK( audio_pacing_init_rate( &p, &fifo, 2, UINT_MAX, 1, UINT_MAX, 512 ) < 0 );
  CHECK( !audio_pacing_init_rate( &p, &fifo, 2, 48000, 3500000, 69888, 0 ) );
  CHECK( p.controller.state == ADAPTIVE_INVALID && p.controller.target == 2047 );
  CHECK( atomic_load( &p.invalid ) && !p.demand && !adaptive_probe( &p.controller ) );
  sfifo_flush( &fifo );
  publish( &p, &fifo, 959, true, true );
  CHECK( p.controller.state == ADAPTIVE_INVALID && p.controller.target == 2047 );
  sfifo_close( &fifo );
}

#ifdef HAVE_PTHREAD
struct threaded_context { struct audio_pacing p; sfifo_t fifo; };
static void *
consumer( void *arg )
{
  struct threaded_context *c = arg;
  unsigned int i = 0;
  unsigned char output[128];
  while( i < 20000 ) {
    int n;
    audio_pacing_callback_begin( &c->p );
    n = pcm_fifo_read( &c->fifo, 2, output, 64 );
    CHECK( n >= 0 );
    audio_pacing_callback_end( &c->p, n, n, false );
    i += n;
  }
  return NULL;
}
static void
thread_tests( void )
{
  struct threaded_context c;
  pthread_t thread;
  unsigned int i;
  CHECK( !sfifo_init( &c.fifo, 4095 ) );
  CHECK( !audio_pacing_init( &c.p, 2047, 241, 512, 961 ) );
  CHECK( !pthread_create( &thread, NULL, consumer, &c ) );
  for( i = 0; i < 20000; i += 40 ) {
    CHECK( !audio_pacing_pending( &c.p, &c.fifo, 2, 40, true, true ) );
    while( !audio_pacing_can_admit( &c.p, &c.fifo, 2, 40 ) ) {}
    c.p.reserved = true;
    CHECK( audio_pacing_write( &c.p, &c.fifo, 2, data, 40, true ) == 40 );
  }
  CHECK( !pthread_join( thread, NULL ) );
  sfifo_close( &c.fifo );
}
#endif
int main( void )
{
  cadence_tests(); controller_tests(); adapter_tests(); convergence_tests();
#ifdef HAVE_PTHREAD
  thread_tests();
#endif
  puts( "Audio pacing controller/publication/FIFO tests passed" );
  return 0;
}
