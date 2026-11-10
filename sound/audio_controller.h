/* Producer-owned preventive audio target controller. GPL-2.0-or-later.
   Apply target changes only between completed production transactions. */
#ifndef FUSE_AUDIO_CONTROLLER_H
#define FUSE_AUDIO_CONTROLLER_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum publication_evidence {
  PUBLICATION_NONE, PUBLICATION_CLEAN, PUBLICATION_WARNING,
  PUBLICATION_AMBIGUOUS, PUBLICATION_UNDERRUN, PUBLICATION_INVALID,
  PUBLICATION_BASELINE
};
enum adaptive_state {
  ADAPTIVE_STARTUP, ADAPTIVE_QUALIFY, ADAPTIVE_NORMAL, ADAPTIVE_PROBE,
  ADAPTIVE_HOLD, ADAPTIVE_EMERGENCY, ADAPTIVE_FALLBACK, ADAPTIVE_INVALID,
  ADAPTIVE_LIMITED
};
struct adaptive_controller {
  unsigned int capacity, allowance, demand, step;
  uint64_t structure, excess, floor, epoch;
  unsigned int target, accepted, recovery, candidate, clean;
  uint32_t baseline;
  bool funded;
  enum adaptive_state state;
};
struct adaptive_sample {
  enum publication_evidence kind;
  unsigned int q, after;
  uint32_t progress;
  uint64_t epoch;
  bool ordinary, complete, positive, uncontaminated, terminal_invalid;
};
static inline uint64_t
adaptive_max( uint64_t a, uint64_t b ) { return a > b ? a : b; }
static inline uint64_t
adaptive_minimum( const struct adaptive_controller *s )
{
  return s->structure + adaptive_max( s->floor,
                                      (uint64_t)s->demand + s->step );
}
static inline bool
adaptive_limited( const struct adaptive_controller *s )
{ return adaptive_minimum( s ) > s->capacity; }
static inline void
adaptive_invalidate( struct adaptive_controller *s )
{ s->epoch++; s->funded = false; s->baseline = 0; s->clean = 0; }
static inline int
adaptive_init( struct adaptive_controller *s, unsigned int capacity,
               unsigned int allowance, unsigned int demand )
{
  memset( s, 0, sizeof( *s ) );
  if( !capacity || !allowance || !demand || allowance > capacity ) return -1;
  s->capacity = capacity; s->allowance = allowance; s->demand = demand;
  s->structure = (uint64_t)allowance + demand - 1;
  s->step = adaptive_max( 2, ((uint64_t)demand + 7) / 8 );
  s->floor = demand;
  s->target = s->accepted = s->recovery = capacity;
  s->state = ADAPTIVE_STARTUP;
  adaptive_invalidate( s );
  return 0;
}
static inline void
adaptive_cancel( struct adaptive_controller *s )
{
  s->target = adaptive_max( s->target, s->accepted );
  s->candidate = 0;
  adaptive_invalidate( s );
}
static inline void
adaptive_protect( struct adaptive_controller *s, uint64_t target,
                  enum adaptive_state state )
{
  adaptive_cancel( s );
  target = adaptive_max( s->target, target );
  s->target = s->accepted = target > s->capacity ? s->capacity : target;
  s->recovery = s->accepted;
  s->state = state == ADAPTIVE_QUALIFY && adaptive_limited( s ) ?
             ADAPTIVE_LIMITED : state;
  adaptive_invalidate( s );
}
static inline void
adaptive_activity( struct adaptive_controller *s )
{
  if( s->state == ADAPTIVE_STARTUP || s->state == ADAPTIVE_FALLBACK ||
      s->state == ADAPTIVE_EMERGENCY || s->state == ADAPTIVE_HOLD ) {
    s->state = adaptive_limited( s ) ? ADAPTIVE_LIMITED : ADAPTIVE_QUALIFY;
    adaptive_invalidate( s );
  }
}
static inline void
adaptive_fallback( struct adaptive_controller *s )
{ adaptive_protect( s, s->capacity, ADAPTIVE_FALLBACK ); }
static inline void
adaptive_exceptional( struct adaptive_controller *s )
{
  adaptive_cancel( s );
  if( s->state == ADAPTIVE_NORMAL || s->state == ADAPTIVE_QUALIFY ||
      s->state == ADAPTIVE_PROBE ) s->state = ADAPTIVE_QUALIFY;
}
static inline bool
adaptive_probe( struct adaptive_controller *s )
{
  uint64_t candidate = adaptive_max( adaptive_minimum( s ),
                                     s->target > s->step ?
                                     s->target - s->step : 0 );
  if( s->state != ADAPTIVE_NORMAL || !s->funded || adaptive_limited( s ) ||
      s->clean < 128 || candidate >= s->target ) return false;
  s->candidate = s->target = candidate;
  s->state = ADAPTIVE_PROBE;
  adaptive_invalidate( s );
  return true;
}
static inline void
adaptive_observe( struct adaptive_controller *s, struct adaptive_sample o )
{
  uint64_t e;
  bool valid;
  if( o.kind == PUBLICATION_UNDERRUN ) {
    if( s->state != ADAPTIVE_STARTUP && s->state != ADAPTIVE_FALLBACK &&
        s->state != ADAPTIVE_INVALID )
      adaptive_protect( s, s->capacity, ADAPTIVE_EMERGENCY );
    adaptive_invalidate( s );
    return;
  }
  if( o.kind == PUBLICATION_INVALID ) {
    if( !o.terminal_invalid && s->state == ADAPTIVE_STARTUP ) {
      adaptive_invalidate( s ); return;
    }
    adaptive_protect( s, s->capacity, ADAPTIVE_INVALID );
    return;
  }
  if( (s->state != ADAPTIVE_NORMAL && s->state != ADAPTIVE_PROBE &&
       s->state != ADAPTIVE_QUALIFY) || o.epoch != s->epoch ) return;
  if( o.kind == PUBLICATION_WARNING ) {
    if( s->funded && o.ordinary && o.uncontaminated && o.q <= s->target ) {
      e = (uint64_t)o.q + s->structure < s->target ?
          s->target - o.q - s->structure : 0;
      s->excess = adaptive_max( s->excess, e );
      s->floor = adaptive_max( s->floor, s->excess + s->demand + s->step );
      s->floor = adaptive_max( s->floor, s->recovery > s->structure ?
                                         s->recovery - s->structure : 0 );
      s->floor = adaptive_max( s->floor, s->accepted > s->structure ?
                                         s->accepted - s->structure : 0 );
      adaptive_protect( s, s->structure + s->floor, ADAPTIVE_QUALIFY );
    } else adaptive_protect( s, s->capacity, ADAPTIVE_HOLD );
    return;
  }
  if( o.kind == PUBLICATION_AMBIGUOUS || !o.uncontaminated ) {
    adaptive_cancel( s ); s->state = ADAPTIVE_QUALIFY; return;
  }
  if( s->funded && o.ordinary && o.q <= s->target ) {
    e = (uint64_t)o.q + s->structure < s->target ?
        s->target - o.q - s->structure : 0;
    s->excess = adaptive_max( s->excess, e );
    if( s->excess ) s->floor = adaptive_max( s->floor,
                                   s->excess + s->demand + s->step );
    if( s->structure + s->floor > s->target ) {
      adaptive_protect( s, s->structure + s->floor, ADAPTIVE_QUALIFY );
      return;
    }
  }
  valid = o.ordinary && o.complete && o.positive &&
          (uint64_t)o.after + s->allowance > s->target &&
          o.after <= s->target &&
          (uint64_t)o.q >= (uint64_t)s->demand + s->step &&
          s->target >= adaptive_minimum( s );
  if( (o.kind != PUBLICATION_BASELINE && o.kind != PUBLICATION_CLEAN) ||
      !valid ) return;
  if( !s->funded ) {
    s->funded = true; s->baseline = o.progress; s->clean = 0;
    if( s->state == ADAPTIVE_QUALIFY ) s->state = ADAPTIVE_NORMAL;
    return;
  }
  if( o.kind == PUBLICATION_CLEAN && o.progress != s->baseline ) {
    s->baseline = o.progress;
    if( s->clean < 128 ) s->clean++;
    if( s->candidate && s->clean >= 128 ) {
      s->recovery = s->accepted; s->accepted = s->target;
      s->candidate = 0; s->state = ADAPTIVE_NORMAL; s->clean = 0;
    }
  }
}
#endif
