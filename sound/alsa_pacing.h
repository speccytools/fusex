/* alsa_pacing.h: producer-owned ALSA admission observations
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

#ifndef FUSE_ALSA_PACING_H
#define FUSE_ALSA_PACING_H

#include <stdbool.h>
#include <stdint.h>
#include "audio_controller.h"

enum alsa_pacing_mode {
  ALSA_PACING_ADAPTIVE, ALSA_PACING_FIXED, ALSA_PACING_CAPACITY
};

struct alsa_pacing {
  struct adaptive_controller controller;
  enum alsa_pacing_mode mode;
  unsigned int startup_bound, fixed_target, batch, last_q;
  uint64_t accepted, transaction_epoch;
  uint32_t progress, baseline;
  bool have_queue, normal, ordinary, reserved, quarantine;
};

/* period is an empirical reporting/service quantum, not a callback bound. */
static inline int
alsa_pacing_init( struct alsa_pacing *p, unsigned int capacity,
                  unsigned int allowance, unsigned int period,
                  unsigned int startup_bound, enum alsa_pacing_mode mode,
                  unsigned int target )
{
  memset( p, 0, sizeof( *p ) );
  if( !startup_bound || startup_bound > capacity ||
      adaptive_init( &p->controller, capacity, allowance, period ) ||
      ( mode == ALSA_PACING_FIXED &&
        ( target < allowance || target > capacity ) ) ) return -1;
  p->startup_bound = startup_bound;
  p->mode = mode;
  p->fixed_target = target;
  p->quarantine = true;
  return 0;
}

/* Only actual accepted frames are credited. Recovery starts a new epoch. */
static inline void
alsa_pacing_accept( struct alsa_pacing *p, unsigned int frames )
{
  p->accepted += frames;
}

static inline int
alsa_pacing_snapshot( struct alsa_pacing *p, unsigned int q )
{
  if( q > p->controller.capacity ) return -1;
  if( p->have_queue ) {
    uint64_t funded = (uint64_t)p->last_q + p->accepted;
    if( funded < q ) return -1;
    if( funded > q ) p->progress++;
  }
  p->have_queue = true;
  p->last_q = q;
  p->accepted = 0;
  return 0;
}

static inline void
alsa_pacing_restart( struct alsa_pacing *p )
{
  adaptive_protect( &p->controller, p->controller.capacity, ADAPTIVE_STARTUP );
  p->normal = p->have_queue = p->reserved = p->ordinary = false;
  p->progress = p->baseline = 0;
  p->accepted = 0;
  p->quarantine = true;
}

static inline unsigned int
alsa_pacing_limit( const struct alsa_pacing *p )
{
  if( !p->ordinary || p->mode == ALSA_PACING_CAPACITY )
    return p->controller.capacity;
  return p->mode == ALSA_PACING_FIXED ? p->fixed_target :
         p->controller.target;
}

static inline int
alsa_pacing_pending( struct alsa_pacing *p, unsigned int frames,
                     bool compatible, bool running )
{
  if( p->reserved || !frames || frames > p->controller.capacity ) return -1;
  p->ordinary = p->normal && compatible && running &&
                frames <= p->controller.allowance;
  if( !p->ordinary ) {
    if( !compatible && p->controller.state != ADAPTIVE_STARTUP )
      adaptive_fallback( &p->controller );
    else if( p->normal ) adaptive_exceptional( &p->controller );
    p->quarantine = true;
  }
  p->batch = frames;
  p->transaction_epoch = p->controller.epoch;
  return 0;
}

/* Maximum reported availability is negative evidence only. Not a continuous
   physical low water and never evidence that permits a downward probe. */
static inline void
alsa_pacing_low_water( struct alsa_pacing *p, unsigned int minimum,
                       bool valid )
{
  struct adaptive_sample sample = { 0 };
  if( !valid || !p->normal || p->mode != ALSA_PACING_ADAPTIVE ) return;
  if( minimum > p->controller.demand ) return;
  sample.kind = PUBLICATION_WARNING;
  sample.epoch = p->controller.epoch;
  sample.q = minimum;
  sample.ordinary = p->ordinary;
  /* An interval minimum is not a qualified instantaneous refill. */
  adaptive_observe( &p->controller, sample );
  p->quarantine = true;
}

/* Charge all write-interval consumption BEFORE an instantaneous refill.
   q_effective=after-B is a conservative pre-refill equivalent, not the literal
   pre-write occupancy. Qualified accounting replaces callback non-overlap. */
static inline int
alsa_pacing_publication( struct alsa_pacing *p, unsigned int before,
                         unsigned int after, bool running, bool compatible )
{
  struct adaptive_controller *s = &p->controller;
  struct adaptive_sample sample = { 0 };
  uint64_t structure = (uint64_t)( p->normal ? s->allowance :
                                   p->startup_bound ) + s->demand - 1;
  unsigned int old_target = s->target;
  uint64_t old_epoch = s->epoch;
  unsigned int q;
  if( !p->reserved || before > s->capacity || after > s->capacity ||
      (uint64_t)before + p->batch < after ) return -1;
  p->reserved = false;
  if( !running || p->transaction_epoch != s->epoch ) {
    p->quarantine = true;
    return 0;
  }
  if( !p->normal && compatible && p->progress &&
      ( p->mode != ALSA_PACING_ADAPTIVE || adaptive_limited( s ) ) ) {
    p->normal = true;
    p->quarantine = true;
    adaptive_activity( s );
    return 0;
  }
  if( after < p->batch ) {
    /* No nonnegative equivalent refill: protect, never clamp into CLEAN. */
    sample.kind = PUBLICATION_WARNING;
    sample.epoch = s->epoch;
    adaptive_observe( s, sample );
    p->quarantine = true;
    return 0;
  }
  q = after - p->batch;
  sample.q = q;
  sample.after = after;
  sample.epoch = p->transaction_epoch;
  sample.progress = p->progress;
  sample.ordinary = p->ordinary;
  sample.complete = sample.positive = sample.uncontaminated = true;
  if( q <= s->demand ) sample.kind = PUBLICATION_WARNING;
  else if( structure + s->demand > s->target )
    sample.kind = PUBLICATION_NONE;
  else if( !p->ordinary && p->normal ) sample.kind = PUBLICATION_NONE;
  else if( after > s->target ||
           (uint64_t)after + s->allowance <= s->target ||
           (uint64_t)q < (uint64_t)s->demand + s->step )
    sample.kind = PUBLICATION_NONE;
  else if( p->quarantine ) sample.kind = PUBLICATION_BASELINE;
  else if( p->progress != p->baseline ) sample.kind = PUBLICATION_CLEAN;
  if( !p->normal && compatible &&
      sample.kind == PUBLICATION_BASELINE && p->progress ) {
    p->normal = true;
    p->quarantine = true;
    adaptive_activity( s );
    return 0;
  }
  if( !p->normal || p->mode != ALSA_PACING_ADAPTIVE ) return 0;
  if( sample.kind == PUBLICATION_BASELINE ||
      sample.kind == PUBLICATION_CLEAN ) {
    p->baseline = p->progress;
    p->quarantine = false;
    if( sample.kind == PUBLICATION_BASELINE ) {
      adaptive_activity( s );
      sample.epoch = s->epoch;
    }
  } else if( sample.kind != PUBLICATION_NONE ) p->quarantine = true;
  adaptive_observe( s, sample );
  if( old_target != s->target || old_epoch != s->epoch ) p->quarantine = true;
  if( adaptive_probe( s ) ) p->quarantine = true;
  return 0;
}

#endif
