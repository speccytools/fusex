/* audio-wakeup-cases.h: shared backend reservation and native wait tests
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

/* Included after the backend under test. */
static int wake_steps[4], wake_step, wake_count, wake_spurious;
static _Atomic unsigned int wake_entered, wake_done;
static audio_progress_deadline wake_deadline;
static int wake_deadline_valid, wake_testing;

static int
wake_before_wait( audio_progress_deadline end )
{
  if( !wake_testing ) return 0;
  if( wake_deadline_valid == 1 ) {
    CHECK( end == wake_deadline );
  }
  wake_deadline = end;
  if( wake_deadline_valid != -1 ) wake_deadline_valid = 1;
  atomic_fetch_add( &wake_entered, 1 );
  if( wake_spurious ) {
    wake_spurious = 0; return 1;
  }
  if( wake_step < wake_count ) wake_consume( wake_steps[wake_step++] );
  return 0;
}

static void
wake_fixture( unsigned int queued, unsigned int target )
{
  libspectrum_signed_word samples[2048] = { 0 };
  int freq = 48000, stereo = 0;
  wake_init();
  CHECK( !sound_lowlevel_init( NULL, &freq, &stereo ) );
  CHECK( pacing.controller.capacity == 2047 );
  pacing.normal = true;
  pacing.controller.target = target;
  CHECK( pcm_fifo_write( &sound_fifo, 2, samples, queued ) == (int)queued );
  wake_step = wake_count = wake_spurious = wake_deadline_valid = 0;
  wake_testing = 1;
  atomic_store( &wake_entered, 0 ); atomic_store( &wake_done, 0 );
}

static void
wake_publish( unsigned int batch )
{
  libspectrum_signed_word samples[2048] = { 0 };
  CHECK( !sound_lowlevel_reserve( batch ) );
  sound_lowlevel_frame( samples, batch );
}

static void
wake_regressions( void )
{
  uint32_t generation;
  /* The observed one-frame deficit: one callback must suffice. Consumption
     occurs immediately before native wait, so compare-and-park must not sleep. */
  wake_fixture( 1680, 1919 );
  wake_steps[0] = 512; wake_count = 1;
  wake_publish( 240 );
  CHECK( wake_step == 1 && atomic_load( &wake_entered ) == 1 );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 1408 );
  sound_lowlevel_end();

  /* Two individually insufficient/sufficient pulls, plus a spurious return.
     All attempts must retain the original absolute deadline. */
  wake_fixture( 1800, 1919 );
  wake_spurious = 1;
  wake_steps[0] = 64; wake_steps[1] = 128; wake_count = 2;
  wake_publish( 240 );
  CHECK( wake_step == 2 && atomic_load( &wake_entered ) == 3 );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 1848 );
  sound_lowlevel_end();

  /* Exceptional batches use physical C, not ordinary N. */
  wake_fixture( 1680, 1919 );
  wake_steps[0] = 128; wake_count = 1;
  wake_publish( 400 );
  CHECK( wake_step == 1 );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 1952 );
  sound_lowlevel_end();

  /* The defensive writer path has no preventive reservation. A full FIFO
     must park on physical space and resume after one consumed frame. */
  wake_fixture( 2047, 1919 );
  wake_steps[0] = 1; wake_count = 1;
  libspectrum_signed_word sample = 0;
  sound_lowlevel_frame( &sample, 1 );
  CHECK( wake_step == 1 && atomic_load( &wake_entered ) == 1 );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 2047 );
  sound_lowlevel_end();

  /* Immediate ordinary admission; underrun publishes a sticky observation
     once, rather than endlessly advancing the generation on empty callbacks. */
  wake_fixture( 0, 1919 );
  wake_publish( 240 );
  CHECK( !atomic_load( &wake_entered ) );
  wake_consume( 512 );
  generation = audio_progress_snapshot( &progress );
  wake_consume( 512 );
  CHECK( audio_progress_snapshot( &progress ) == generation );
  CHECK( atomic_load( &pacing.missing ) );
  sound_lowlevel_end();

  /* Quiescent reinit resets notifications and keeps initial output paused.
     Real primitive tests: already-published progress, wrap, and bounded park
     without a callback (a timeout never grants an admission). */
  wake_fixture( 0, 2047 );
  CHECK( !audio_progress_snapshot( &progress ) && !audio_output_started );
  generation = audio_progress_snapshot( &progress );
  audio_progress_publish( &progress );
  CHECK( !audio_progress_wait( &progress, generation,
                               audio_progress_until() ) );
  atomic_store( &progress.generation, UINT32_MAX );
  audio_progress_publish( &progress );
  CHECK( !audio_progress_snapshot( &progress ) );
  CHECK( !audio_progress_wait( &progress, UINT32_MAX,
                               audio_progress_until() ) );
  CHECK( audio_progress_wait( &progress, 0,
                              audio_progress_until() ) == ETIMEDOUT );
  if( !compat_wait_uses_address ) {
    /* A callback claims an arm, but its generation is noticed before park.
       The retained token must be discarded before the next attempt. Multiple
       callbacks on that arm coalesce to one token, not one per callback. */
    atomic_store( &progress.armed, true );
    audio_progress_publish( &progress );
    audio_progress_publish( &progress );
    generation = audio_progress_snapshot( &progress );
    CHECK( !atomic_load( &progress.armed ) );
    CHECK( audio_progress_wait( &progress, generation,
                                audio_progress_until() ) == ETIMEDOUT );
    CHECK( !atomic_load( &progress.armed ) );
    /* Idle callbacks change generation without banking permits. */
    audio_progress_publish( &progress );
    generation = audio_progress_snapshot( &progress );
    CHECK( audio_progress_wait( &progress, generation,
                                audio_progress_until() ) == ETIMEDOUT );
  }
  /* An already-expired attempt must not be extended by spurious success. */
  {
    audio_progress_deadline expired = { 0 };
    CHECK( audio_progress_wait( &progress, audio_progress_snapshot( &progress ),
                                expired ) == ETIMEDOUT );
  }
  sound_lowlevel_end();
}

#ifdef AUDIO_WAKE_THREADS
static void *
wake_producer( void *unused )
{
  (void)unused;
  wake_publish( 240 );
  atomic_store( &wake_done, 1 );
  return NULL;
}

static void
wake_threaded( void )
{
  pthread_t producer;
  struct timespec pause = { 0, 2000000 };
  unsigned int tries;
  wake_fixture( 1680, 1919 );
  wake_deadline_valid = -1;
  /* No injected consumption. Run the actual native wait against a callback
     on another thread. Join production BEFORE owner-thread teardown/reinit. */
  CHECK( !pthread_create( &producer, NULL, wake_producer, NULL ) );
  for( tries = 0; !atomic_load( &wake_entered ) && tries < 1000; tries++ )
    nanosleep( &pause, NULL );
  CHECK( atomic_load( &wake_entered ) );
  nanosleep( &pause, NULL );
  CHECK( !atomic_load( &wake_done ) );
  wake_consume( 512 );
  for( tries = 0; !atomic_load( &wake_done ) && tries < 1000; tries++ )
    nanosleep( &pause, NULL );
  CHECK( atomic_load( &wake_done ) );
  CHECK( !pthread_join( producer, NULL ) );
  CHECK( pcm_fifo_consumer_used( &sound_fifo, 2 ) == 1408 );
  sound_lowlevel_end();
  wake_fixture( 0, 2047 );
  CHECK( !audio_progress_snapshot( &progress ) );
  sound_lowlevel_end();
}

#endif
