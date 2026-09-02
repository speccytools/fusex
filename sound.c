/* sound.c: Sound support
   Copyright (c) 2000-2026 Russell Marks, Matan Ziv-Av, Philip Kendall,
                           Fredrick Meunier, Patrik Rak

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

   Author contact information:

   E-mail: philip-fuse@shadowmagic.org.uk

*/

/* The AY white noise RNG algorithm is based on info from MAME's ay8910.c -
 * MAME's licence explicitly permits free use of info (even encourages it).
 */

#include "config.h"

#include "fuse.h"
#include "infrastructure/startup_manager.h"
#include "machine.h"
#include "movie.h"
#include "options.h"
#include "settings.h"
#include "sound.h"
#include "tape.h"
#include "timer/timer.h"
#include "ui/ui.h"
#include "peripherals/sound/sp0256.h"
#include "sound/blipbuffer.h"
#include "sound/speaker_filter.h"
#include "sound/ula_filter.h"

/* Do we have any of our sound devices available? */

/* configuration */
int sound_enabled = 0;		/* Are we currently using the sound card */

static int sound_enabled_ever = 0; /* whether sound has *ever* been in use; see
				      sound_ay_write() and sound_ay_reset() */
int sound_stereo_ay = SOUND_STEREO_AY_NONE; /* local copy of settings_current.stereo_ay */

enum sound_speaker_type {
  SOUND_SPEAKER_TYPE_AUTOMATIC,
  SOUND_SPEAKER_TYPE_TV,
  SOUND_SPEAKER_TYPE_BEEPER,
  SOUND_SPEAKER_TYPE_UNFILTERED
};

/* assume all three tone channels together match the beeper volume (ish).
 * Must be <=127 for all channels; 50+2+(24*3) = 124.
 * (Now scaled up for 16-bit.)
 */
#define AMPL_AY_TONE		( 24 * 256 )	/* three of these */

/* max. number of sub-frame AY port writes allowed;
 * given the number of port writes theoretically possible in a
 * 50th I think this should be plenty.
 */
#define AY_CHANGE_MAX		8000
#define AY_CHANNELS		3
/* the AY envelope generator cycles through 16 amplitude steps per
   envelope period; the chip also has 16 distinct output volume levels */
#define AY_ENV_STEPS		16

int sound_framesiz;

static int sound_channels;

static unsigned int ay_tone_levels[AY_ENV_STEPS];

static unsigned int ay_tone_tick[AY_CHANNELS], ay_tone_high[AY_CHANNELS], ay_noise_tick;
static unsigned int ay_tone_cycles, ay_env_cycles;
static unsigned int ay_env_internal_tick, ay_env_tick;
static unsigned int ay_tone_period[AY_CHANNELS], ay_noise_period, ay_env_period;

/* Local copy of the AY registers */
static libspectrum_byte sound_ay_registers[AY_REGISTERS];

struct ay_change_tag
{
  libspectrum_dword tstates;
  unsigned char reg, val;
};

static struct ay_change_tag ay_change[ AY_CHANGE_MAX ];
static int ay_change_count;

/* The main buffers contain AY and peripheral sources. The selected ULA path
 * has its own mono buffer, so it can acquire independent processing before it
 * is mixed into the final PCM frame. */
Blip_Buffer *left_buf = NULL;
Blip_Buffer *right_buf = NULL;
static Blip_Buffer *ula_buf = NULL;
blip_sample_t *samples = NULL;
static blip_sample_t *ula_samples = NULL;
static int ula_output_count;
static speaker_filter_t ula_beeper_speaker_filter;
static ula_filter_t ula_filter;
static int ula_beeper_speaker_filter_active;
static int ula_filter_speaker_type = -1;
static int ula_synth_speaker_type = -1;

static Blip_Synth *ula_synth = NULL;

/* The ULA MIC output is active low. Tape input is combined at the ULA node,
 * as it was by the old sound_beeper() state encoding. */
static int ula_mic_on;
static int ula_beeper_on;

Blip_Synth *ay_a_synth = NULL, *ay_b_synth = NULL, *ay_c_synth = NULL;
Blip_Synth *ay_a_synth_r = NULL, *ay_b_synth_r = NULL, *ay_c_synth_r = NULL;

Blip_Synth *left_specdrum_synth = NULL, *right_specdrum_synth = NULL;

Blip_Synth *left_covox_synth = NULL, *right_covox_synth = NULL;

Blip_Synth *left_sp0256_synth = NULL, *right_sp0256_synth = NULL;

static double
sound_get_volume( int volume )
{
  if( volume < 0 ) volume = 0;
  else if( volume > 100 ) volume = 100;

  return volume / 100.0;
}

/* Returns the emulation speed adjusted processor speed */
libspectrum_dword
sound_get_effective_processor_speed( void )
{
  return machine_current->timings.processor_speed / 100 * 
           settings_current.emulation_speed;
}

static int
sound_init_buffer( Blip_Buffer **buf )
{
  *buf = new_Blip_Buffer();
  blip_buffer_set_clock_rate( *buf, sound_get_effective_processor_speed() );
  /* Allow up to 1s of playback buffer - this allows us to cope with slowing
     down to 2% of speed where a single Speccy frame generates just under 1s
     of sound */
  if ( blip_buffer_set_sample_rate( *buf, settings_current.sound_freq, 1000 ) ) {
    sound_end();
    ui_error( UI_ERROR_ERROR, "out of memory at %s:%d", __FILE__, __LINE__ );
    return 0;
  }

  blip_buffer_set_bass_freq( *buf, 0 );
  return 1;
}

static Blip_Synth *
sound_init_synth( Blip_Buffer *buf, int volume )
{
  Blip_Synth *synth = new_Blip_Synth();

  blip_synth_set_volume( synth, sound_get_volume( volume ) );
  blip_synth_set_output( synth, buf );
  blip_synth_set_treble_eq( synth, 0.0 );

  return synth;
}

static void
sound_ay_init( void )
{
  /* AY output doesn't match the claimed levels; these levels are based
   * on the measurements posted to comp.sys.sinclair in Dec 2001 by
   * Matthew Westcott, adjusted as I described in a followup to his post,
   * then scaled to 0..0xffff.
   */
  static const int levels[AY_ENV_STEPS] = {
    0x0000, 0x0385, 0x053D, 0x0770,
    0x0AD7, 0x0FD5, 0x15B0, 0x230C,
    0x2B4C, 0x43C1, 0x5A4B, 0x732F,
    0x9204, 0xAFF1, 0xD921, 0xFFFF
  };
  int f;

  /* scale the values down to fit */
  for( f = 0; f < AY_ENV_STEPS; f++ )
    ay_tone_levels[f] = ( levels[f] * AMPL_AY_TONE + 0x8000 ) / 0xffff;

  ay_noise_tick = ay_noise_period = 0;
  ay_env_internal_tick = ay_env_tick = ay_env_period = 0;
  ay_tone_cycles = ay_env_cycles = 0;
  for( f = 0; f < AY_CHANNELS; f++ )
    ay_tone_tick[f] = ay_tone_high[f] = 0, ay_tone_period[f] = 1;

  ay_change_count = 0;
}

#ifndef UI_WIN32
#define MIN_SPEED_PERCENTAGE 2
#define MAX_SPEED_PERCENTAGE 500
#else                        /* #ifndef UI_WIN32 */
/* We are limiting speed until bugs in the DirectSound driver are resolved, see
   [bugs:#364] for more details */
#define MIN_SPEED_PERCENTAGE 50
#define MAX_SPEED_PERCENTAGE 300
#endif                       /* #ifndef UI_WIN32 */

static int
is_in_sound_enabled_range( void )
{
  return settings_current.emulation_speed >= MIN_SPEED_PERCENTAGE &&
    settings_current.emulation_speed <= MAX_SPEED_PERCENTAGE;
}

void
sound_init( const char *device )
{
  float hz;
  Blip_Synth **ay_left_synth;
  Blip_Synth **ay_mid_synth;
  Blip_Synth **ay_mid_synth_r;
  Blip_Synth **ay_right_synth;

  /* Allow sound as long as emulation speed is greater than 2%
     (less than that and a single Speccy frame generates more
     than a seconds worth of sound which is bigger than the
     maximum Blip_Buffer of 1 second) */
  if( !( !sound_enabled && settings_current.sound &&
         is_in_sound_enabled_range() ) )
    return;

  /* only try for stereo if we need it */
  sound_stereo_ay = option_enumerate_sound_stereo_ay();

  if( settings_current.sound &&
      sound_lowlevel_init( device, &settings_current.sound_freq,
                           &sound_stereo_ay ) )
    return;

  if( !sound_init_buffer( &left_buf ) ) return;
  if( sound_stereo_ay != SOUND_STEREO_AY_NONE &&
      !sound_init_buffer( &right_buf ) )
    return;
  if( !sound_init_buffer( &ula_buf ) ) return;

  ula_synth = sound_init_synth( ula_buf, settings_current.volume_beeper );

  ay_a_synth = new_Blip_Synth();
  blip_synth_set_volume( ay_a_synth,
                         sound_get_volume( settings_current.volume_ay) );
  blip_synth_set_treble_eq( ay_a_synth, 0.0 );

  ay_b_synth = new_Blip_Synth();
  blip_synth_set_volume( ay_b_synth,
                         sound_get_volume( settings_current.volume_ay) );
  blip_synth_set_treble_eq( ay_b_synth, 0.0 );

  ay_c_synth = new_Blip_Synth();
  blip_synth_set_volume( ay_c_synth,
                         sound_get_volume( settings_current.volume_ay) );
  blip_synth_set_treble_eq( ay_c_synth, 0.0 );

  left_specdrum_synth = new_Blip_Synth();
  blip_synth_set_volume( left_specdrum_synth,
                         sound_get_volume( settings_current.volume_specdrum ) );
  blip_synth_set_output( left_specdrum_synth, left_buf );
  blip_synth_set_treble_eq( left_specdrum_synth, 0.0 );

  left_covox_synth = new_Blip_Synth();
  blip_synth_set_volume( left_covox_synth,
                         sound_get_volume( settings_current.volume_covox ) );
  blip_synth_set_output( left_covox_synth, left_buf );
  blip_synth_set_treble_eq( left_covox_synth, 0.0 );
  
  left_sp0256_synth = new_Blip_Synth();
  blip_synth_set_volume( left_sp0256_synth,
                         sound_get_volume( settings_current.volume_uspeech ) );
  blip_synth_set_output( left_sp0256_synth, left_buf );
  blip_synth_set_treble_eq( left_sp0256_synth, 0.0 );

  /* important to override these settings if not using stereo
   * (it would probably be confusing to mess with the stereo
   * settings in settings_current though, which is why we make copies
   * rather than using the real ones).
   */

  ay_a_synth_r = NULL;
  ay_b_synth_r = NULL;
  ay_c_synth_r = NULL;

  if( sound_stereo_ay != SOUND_STEREO_AY_NONE ) {
    /* Attach the Blip_Synth's we've already created as appropriate, and
     * create one more Blip_Synth for the middle channel's right buffer. */
    if( sound_stereo_ay == SOUND_STEREO_AY_ACB ) {
      ay_left_synth = &ay_a_synth;
      ay_mid_synth = &ay_c_synth;
      ay_mid_synth_r = &ay_c_synth_r;
      ay_right_synth = &ay_b_synth;
    } else if ( sound_stereo_ay == SOUND_STEREO_AY_ABC ) {
      ay_left_synth = &ay_a_synth;
      ay_mid_synth = &ay_b_synth;
      ay_mid_synth_r = &ay_b_synth_r;
      ay_right_synth = &ay_c_synth;
    } else {
      ui_error( UI_ERROR_ERROR, "unknown AY stereo separation type: %d", sound_stereo_ay );
      fuse_abort();
    }

    blip_synth_set_output( *ay_left_synth, left_buf );
    blip_synth_set_output( *ay_mid_synth, left_buf );
    blip_synth_set_output( *ay_right_synth, right_buf );

    *ay_mid_synth_r = new_Blip_Synth();
    blip_synth_set_volume( *ay_mid_synth_r,
                           sound_get_volume( settings_current.volume_ay ) );
    blip_synth_set_output( *ay_mid_synth_r, right_buf );
    blip_synth_set_treble_eq( *ay_mid_synth_r, 0.0 );

    right_specdrum_synth = new_Blip_Synth();
    blip_synth_set_volume( right_specdrum_synth,
                           sound_get_volume( settings_current.volume_specdrum ) );
    blip_synth_set_output( right_specdrum_synth, right_buf );
    blip_synth_set_treble_eq( right_specdrum_synth, 0.0 );

    right_covox_synth = new_Blip_Synth();
    blip_synth_set_volume( right_covox_synth,
                           sound_get_volume( settings_current.volume_covox ) );
    blip_synth_set_output( right_covox_synth, right_buf );
    blip_synth_set_treble_eq( right_covox_synth, 0.0 );

    right_sp0256_synth = new_Blip_Synth();
    blip_synth_set_volume( right_sp0256_synth,
                           sound_get_volume( settings_current.volume_uspeech ) );
    blip_synth_set_output( right_sp0256_synth, right_buf );
    blip_synth_set_treble_eq( right_sp0256_synth, 0.0 );
  } else {
    blip_synth_set_output( ay_a_synth, left_buf );
    blip_synth_set_output( ay_b_synth, left_buf );
    blip_synth_set_output( ay_c_synth, left_buf );
  }

  sound_enabled = sound_enabled_ever = 1;

  if( speaker_filter_configure( &ula_beeper_speaker_filter,
                                settings_current.sound_freq,
                                SPEAKER_FILTER_DEFAULT_FREQUENCY,
                                SPEAKER_FILTER_DEFAULT_Q ) ||
      ula_filter_configure( &ula_filter, settings_current.sound_freq ) ) {
    ui_error( UI_ERROR_ERROR, "could not configure Spectrum ULA filter" );
    sound_end();
    return;
  }
  ula_filter_speaker_type = -1;
  ula_synth_speaker_type = -1;

  sound_channels = ( sound_stereo_ay != SOUND_STEREO_AY_NONE ? 2 : 1 );

  /* Adjust relative processor speed to deal with adjusting sound generation
     frequency against emulation speed (more flexible than adjusting generated
     sample rate) */
  hz = ( float )sound_get_effective_processor_speed() /
                machine_current->timings.tstates_per_frame;

  /* Size of audio data we will get from running a single Spectrum frame */
  sound_framesiz = ( float )settings_current.sound_freq / hz;
  sound_framesiz++;

  samples = libspectrum_new0( blip_sample_t, sound_framesiz * sound_channels );
  ula_samples = libspectrum_new0( blip_sample_t, sound_framesiz );
  /* initialize movie settings... */
  movie_init_sound( settings_current.sound_freq, sound_stereo_ay );

}

void
sound_pause( void )
{
  if( sound_enabled )
    sound_end();
}

void
sound_unpause( void )
{
  /* No sound if fastloading in progress */
  if( settings_current.fastload && timer_fastloading_active() )
    return;

  sound_init( settings_current.sound_device );
}

void
sound_end( void )
{
  if( sound_enabled ) {
    delete_Blip_Synth( &ula_synth );

    delete_Blip_Synth( &ay_a_synth );
    delete_Blip_Synth( &ay_b_synth );
    delete_Blip_Synth( &ay_c_synth );
    delete_Blip_Synth( &ay_a_synth_r );
    delete_Blip_Synth( &ay_b_synth_r );
    delete_Blip_Synth( &ay_c_synth_r );

    delete_Blip_Synth( &left_specdrum_synth );
    delete_Blip_Synth( &right_specdrum_synth );

    delete_Blip_Synth( &left_covox_synth );
    delete_Blip_Synth( &right_covox_synth );

    delete_Blip_Synth( &left_sp0256_synth );
    delete_Blip_Synth( &right_sp0256_synth );

    delete_Blip_Buffer( &left_buf );
    delete_Blip_Buffer( &right_buf );
    delete_Blip_Buffer( &ula_buf );

    if( settings_current.sound ) 
      sound_lowlevel_end();
    libspectrum_free( samples );
    libspectrum_free( ula_samples );
    sound_enabled = 0;
  }
}

void
sound_register_startup( void )
{
  startup_manager_module dependencies[] = { STARTUP_MANAGER_MODULE_SETUID };
  startup_manager_register( STARTUP_MANAGER_MODULE_SOUND, dependencies,
                            ARRAY_SIZE( dependencies ), NULL, NULL, sound_end );
}

static inline void
ay_do_tone( int level, unsigned int tone_count, int *var, int chan )
{
  *var = 0;

  ay_tone_tick[ chan ] += tone_count;

  if( ay_tone_tick[ chan ] >= ay_tone_period[ chan ] ) {
    ay_tone_tick[ chan ] -= ay_tone_period[ chan ];
    ay_tone_high[ chan ] = !ay_tone_high[ chan ];
  }

  if( level && ay_tone_high[ chan ] )
    *var = level;
}

/* bitmasks for envelope */
#define AY_ENV_CONT	8
#define AY_ENV_ATTACK	4
#define AY_ENV_ALT	2
#define AY_ENV_HOLD	1

/* the AY steps down the external clock by 16 for tone and noise
   generators */
#define AY_CLOCK_DIVISOR 16
/* all Spectrum models and clones with an AY seem to count down the
   master clock by 2 to drive the AY */
#define AY_CLOCK_RATIO 2

static void
sound_ay_overlay( void )
{
  static int rng = 1;
  static int noise_toggle = 0;
  static int env_first = 1, env_rev = 0, env_counter = AY_ENV_STEPS - 1;
  int tone_level[AY_CHANNELS];
  int mixer, envshape;
  int g, level;
  libspectrum_dword f;
  struct ay_change_tag *change_ptr = ay_change;
  int changes_left = ay_change_count;
  int reg, r, ch_vol;
  int chan1, chan2, chan3;
  int last_chan1 = 0, last_chan2 = 0, last_chan3 = 0;
  unsigned int tone_count, noise_count;

  /* If no AY chip, don't produce any AY sound (!) */
  if( !( periph_is_active( PERIPH_TYPE_FULLER) ||
         periph_is_active( PERIPH_TYPE_MELODIK ) ||
         machine_current->capabilities & LIBSPECTRUM_MACHINE_CAPABILITY_AY ) )
    return;

  for( f = 0; f < machine_current->timings.tstates_per_frame;
       f+= AY_CLOCK_DIVISOR * AY_CLOCK_RATIO ) {
    /* update ay registers. */
    while( changes_left && f >= change_ptr->tstates ) {
      sound_ay_registers[ reg = change_ptr->reg ] = change_ptr->val;
      change_ptr++;
      changes_left--;

      /* fix things as needed for some register changes */
      switch ( reg ) {
      case 0: case 1: case 2: case 3: case 4: case 5:
        r = reg >> 1;
        /* a zero-len period is the same as 1 */
        ay_tone_period[r] = ( sound_ay_registers[ reg & ~1 ] |
                              ( sound_ay_registers[ reg | 1 ] & 15 ) << 8 );
        if( !ay_tone_period[r] )
          ay_tone_period[r]++;

        /* important to get this right, otherwise e.g. Ghouls 'n' Ghosts
         * has really scratchy, horrible-sounding vibrato.
         */
        if( ay_tone_tick[r] >= ay_tone_period[r] * 2 )
          ay_tone_tick[r] %= ay_tone_period[r] * 2;
        break;
      case 6:
        ay_noise_tick = 0;
        ay_noise_period = ( sound_ay_registers[ reg ] & 31 );
        break;
      case 11: case 12:
        ay_env_period =
          sound_ay_registers[11] | ( sound_ay_registers[12] << 8 );
        break;
      case 13:
        ay_env_internal_tick = ay_env_tick = ay_env_cycles = 0;
        env_first = 1;
        env_rev = 0;
        env_counter = ( sound_ay_registers[13] & AY_ENV_ATTACK ) ? 0 : AY_ENV_STEPS - 1;
        break;
      }
    }

    /* Per-channel tone level: use the envelope output if bit 4 of the
       channel's volume register is set, otherwise use the volume table. */
    envshape = sound_ay_registers[13];
    level = ay_tone_levels[ env_counter ];

    for( g = 0; g < AY_CHANNELS; g++ ) {
      ch_vol = sound_ay_registers[ 8 + g ];
      tone_level[g] = ( ch_vol & 16 ) ? level : ay_tone_levels[ ch_vol & 15 ];
    }

    /* envelope output counter gets incr'd every 16 AY cycles. */
    ay_env_cycles += AY_CLOCK_DIVISOR;
    noise_count = 0;
    while( ay_env_cycles >= AY_CLOCK_DIVISOR ) {
      ay_env_cycles -= AY_CLOCK_DIVISOR;
      noise_count++;
      ay_env_tick++;
      while( ay_env_tick >= ay_env_period ) {
        ay_env_tick -= ay_env_period;

        /* do a 1/16th-of-period incr/decr if needed */
        if( env_first ||
            ( ( envshape & AY_ENV_CONT ) && !( envshape & AY_ENV_HOLD ) ) ) {
          if( env_rev )
            env_counter -= ( envshape & AY_ENV_ATTACK ) ? 1 : -1;
          else
            env_counter += ( envshape & AY_ENV_ATTACK ) ? 1 : -1;
          if( env_counter < 0 )
            env_counter = 0;
          if( env_counter > AY_ENV_STEPS - 1 )
            env_counter = AY_ENV_STEPS - 1;
        }

        ay_env_internal_tick++;
        while( ay_env_internal_tick >= AY_ENV_STEPS ) {
          ay_env_internal_tick -= AY_ENV_STEPS;

          /* end of cycle */
          if( !( envshape & AY_ENV_CONT ) )
            env_counter = 0;
          else {
            if( envshape & AY_ENV_HOLD ) {
              if( env_first && ( envshape & AY_ENV_ALT ) )
                env_counter = ( env_counter ? 0 : AY_ENV_STEPS - 1 );
            } else {
              /* non-hold */
              if( envshape & AY_ENV_ALT )
                env_rev = !env_rev;
              else
                env_counter = ( envshape & AY_ENV_ATTACK ) ? 0 : AY_ENV_STEPS - 1;
            }
          }

          env_first = 0;
        }

        /* don't keep trying if period is zero */
        if( !ay_env_period )
          break;
      }
    }

    /* generate tone+noise... or neither.
     * (if no tone/noise is selected, the chip just shoves the
     * level out unmodified. This is used by some sample-playing
     * stuff.)
     */
    chan1 = tone_level[0];
    chan2 = tone_level[1];
    chan3 = tone_level[2];
    mixer = sound_ay_registers[7];

    ay_tone_cycles += AY_CLOCK_DIVISOR;
    tone_count = ay_tone_cycles >> 3;
    ay_tone_cycles &= 7;

    if( ( mixer & 1 ) == 0 ) {
      level = chan1;
      ay_do_tone( level, tone_count, &chan1, 0 );
    }
    if( ( mixer & 0x08 ) == 0 && noise_toggle )
      chan1 = 0;

    if( ( mixer & 2 ) == 0 ) {
      level = chan2;
      ay_do_tone( level, tone_count, &chan2, 1 );
    }
    if( ( mixer & 0x10 ) == 0 && noise_toggle )
      chan2 = 0;

    if( ( mixer & 4 ) == 0 ) {
      level = chan3;
      ay_do_tone( level, tone_count, &chan3, 2 );
    }
    if( ( mixer & 0x20 ) == 0 && noise_toggle )
      chan3 = 0;

    if( last_chan1 != chan1 ) {
      blip_synth_update( ay_a_synth, f, chan1 );
      if( ay_a_synth_r ) blip_synth_update( ay_a_synth_r, f, chan1 );
      last_chan1 = chan1;
    }
    if( last_chan2 != chan2 ) {
      blip_synth_update( ay_b_synth, f, chan2 );
      if( ay_b_synth_r ) blip_synth_update( ay_b_synth_r, f, chan2 );
      last_chan2 = chan2;
    }
    if( last_chan3 != chan3 ) {
      blip_synth_update( ay_c_synth, f, chan3 );
      if( ay_c_synth_r ) blip_synth_update( ay_c_synth_r, f, chan3 );
      last_chan3 = chan3;
    }

    /* update noise RNG/filter */
    ay_noise_tick += noise_count;
    while( ay_noise_tick >= ay_noise_period ) {
      ay_noise_tick -= ay_noise_period;

      /* rng is 17-bit LFSR, bit 0 is output.
       * Feedback input is bit 0 XOR bit 1 (drives noise_toggle).
       * Shift taps at bits 14 and 17 produce the next state.
       * Rewritten branch-free: XOR/mask replaces two conditional branches
       * whose outcomes are pseudo-random and therefore poorly predicted.
       */
      noise_toggle ^= ( rng ^ ( rng >> 1 ) ) & 1;
      rng = ( rng >> 1 ) ^ ( 0x12000 & -( rng & 1 ) );

      /* don't keep trying if period is zero */
      if( !ay_noise_period )
        break;
    }
  }
}

/* don't make the change immediately; record it for later,
 * to be made by sound_frame() (via sound_ay_overlay()).
 */
void
sound_ay_write( int reg, int val, libspectrum_dword now )
{
  if( ay_change_count < AY_CHANGE_MAX ) {
    ay_change[ ay_change_count ].tstates = now;
    ay_change[ ay_change_count ].reg = ( reg & 15 );
    ay_change[ ay_change_count ].val = val;
    ay_change_count++;
  }
}

/* no need to call this initially, but should be called
 * on reset otherwise.
 */
void
sound_ay_reset( void )
{
  int f;

  /* recalculate timings based on new machines ay clock */
  sound_ay_init();

  ay_change_count = 0;
  for( f = 0; f < AY_REGISTERS; f++ )
    sound_ay_write( f, 0, 0 );
  for( f = 0; f < AY_CHANNELS; f++ )
    ay_tone_high[f] = 0;
  ay_tone_cycles = ay_env_cycles = 0;
  speaker_filter_reset( &ula_beeper_speaker_filter );
  ula_filter_reset( &ula_filter );
  ula_beeper_speaker_filter_active = 0;
  ula_filter_speaker_type = -1;
  ula_synth_speaker_type = -1;
}

/*
 * sound_specdrum_write - very simple routine
 * as the output is already a digitized waveform
 */
void
sound_specdrum_write( libspectrum_word port GCC_UNUSED, libspectrum_byte val )
{
  if( periph_is_active( PERIPH_TYPE_SPECDRUM ) ) {
    blip_synth_update( left_specdrum_synth, tstates, ( val - 128) * 128);
    if( right_specdrum_synth ) {
      blip_synth_update( right_specdrum_synth, tstates, ( val - 128) * 128);
    }
    machine_current->specdrum.specdrum_dac = val - 128;
  }
}

/*
 * sound_covox_write - very simple routine
 * as the output is already a digitized waveform
 */
void
sound_covox_write( libspectrum_word port GCC_UNUSED, libspectrum_byte val )
{
  if( periph_is_active( PERIPH_TYPE_COVOX_FB ) ||
      periph_is_active( PERIPH_TYPE_COVOX_DD ) ) {
    blip_synth_update( left_covox_synth, tstates, val * 128);
    if( right_covox_synth ) {
      blip_synth_update( right_covox_synth, tstates, val * 128);
    }
    machine_current->covox.covox_dac = val;
  }
}

/*
 * sound_sp0256_write - very simple routine
 * as the output is already a digitized waveform
 */
void
sound_sp0256_write( libspectrum_dword at_tstates, libspectrum_signed_word val )
{
  if( !sound_enabled )
    return;

  blip_synth_update( left_sp0256_synth, at_tstates, val );
  if( right_sp0256_synth ) {
    blip_synth_update( right_sp0256_synth, at_tstates, val );
  }
}

static int
sound_get_speaker_type( void )
{
  int speaker_type = option_enumerate_sound_speaker_type();

  if( speaker_type != SOUND_SPEAKER_TYPE_AUTOMATIC ) return speaker_type;

  return machine_current->capabilities &
           LIBSPECTRUM_MACHINE_CAPABILITY_BEEPER ?
           SOUND_SPEAKER_TYPE_BEEPER : SOUND_SPEAKER_TYPE_TV;
}

static void
sound_mix_ula_speaker( long count )
{
  int filter_speaker = 0;
  int filter_ula = 0;
  int speaker_type = sound_get_speaker_type();
  long i;
  long frames = sound_channels == 2 ? count / 2 : count;

  switch( speaker_type ) {
  case SOUND_SPEAKER_TYPE_TV:   /* TV speaker: ULA/MIC socket output */
    filter_ula = 1;
    break;
  case SOUND_SPEAKER_TYPE_BEEPER: /* Beeper: internal speaker response */
    filter_ula = 1;
    filter_speaker = 1;
    break;
  case SOUND_SPEAKER_TYPE_UNFILTERED: /* Unfiltered: raw ULA/MIC output */
    break;
  default:
    fuse_abort();
  }

  if( speaker_type != ula_filter_speaker_type ) {
    /* A newly selected stream follows the listening-test convention: its
     * first target initializes the pole, rather than inheriting an inactive
     * stream's old state. */
    if( filter_ula ) ula_filter_reset( &ula_filter );
    ula_filter_speaker_type = speaker_type;
  }

  if( filter_speaker != ula_beeper_speaker_filter_active ) {
    speaker_filter_reset( &ula_beeper_speaker_filter );
    ula_beeper_speaker_filter_active = filter_speaker;
  }

  for( i = 0; i < frames && i < ula_output_count; i++ ) {
    int channel;
    double ula_sample = ula_samples[i];

    if( filter_ula ) ula_sample = ula_filter_apply( &ula_filter, ula_sample );

    /* The acoustic speaker model follows the electrical ULA pole and never
     * affects the MIC socket path. */
    if( filter_speaker )
      ula_sample = speaker_filter_apply( &ula_beeper_speaker_filter,
                                         ula_sample );

    for( channel = 0; channel < sound_channels; channel++ ) {
      long sample = samples[ i * sound_channels + channel ] + ula_sample;

      if( sample > 0x7fff ) sample = 0x7fff;
      else if( sample < -0x8000 ) sample = -0x8000;
      samples[ i * sound_channels + channel ] = sample;
    }
  }
}

void
sound_frame( void )
{
  long count;

  if( !sound_enabled )
    return;

  sp0256_do_frame();

  /* overlay AY sound */
  sound_ay_overlay();

  blip_buffer_end_frame( left_buf, machine_current->timings.tstates_per_frame );
  blip_buffer_end_frame( ula_buf, machine_current->timings.tstates_per_frame );

  if( sound_stereo_ay != SOUND_STEREO_AY_NONE ) {
    blip_buffer_end_frame( right_buf, machine_current->timings.tstates_per_frame );

    /* Read left channel into even samples, right channel into odd samples:
       LRLRLRLRLR... */
    count = blip_buffer_read_samples( left_buf, samples, sound_framesiz, 1 );
    blip_buffer_read_samples( right_buf, samples + 1, count, 1 );
    count <<= 1;
  } else {
    count = blip_buffer_read_samples( left_buf, samples, sound_framesiz,
                                      BLIP_BUFFER_DEF_STEREO );
  }

  ula_output_count = blip_buffer_read_samples( ula_buf, ula_samples,
                                                sound_framesiz, 0 );
  sound_mix_ula_speaker( count );

  if( settings_current.sound )
    sound_lowlevel_frame( samples, count );

  if( movie_recording )
      movie_add_sound( samples, count );
  ay_change_count = 0;
}

void
sound_ula_levels( int mic_on, int beeper_on, int *mic_ampl,
                  int *beeper_ampl )
{
  *mic_ampl = ( mic_on ? SOUND_AMPL_TAPE : 0 ) +
              ( beeper_on ? SOUND_AMPL_BEEPER : 0 );
  *beeper_ampl = beeper_on ? SOUND_AMPL_BEEPER +
                  ( mic_on ? SOUND_AMPL_TAPE : 0 ) : 0;
}

static void
sound_ula_update( libspectrum_dword at_tstates )
{
  int mic_on = ula_mic_on || tape_microphone;
  int mic_ampl, beeper_ampl;
  int speaker_type = sound_get_speaker_type();

  /* This is the ULA output-node path. Unlike the speaker path, its MIC-only
   * state is retained for a future MIC output selection. */
  sound_ula_levels( mic_on, ula_beeper_on, &mic_ampl, &beeper_ampl );

  /* Preserve the legacy tape-loading policy on the speaker path. Timex
   * machines have no loading noise; disabling it also removed the MIC
   * contribution while a tape was playing. */
  if( tape_is_playing() &&
      ( !settings_current.sound_load || machine_current->timex ) ) {
    beeper_ampl = ula_beeper_on ? SOUND_AMPL_BEEPER : 0;
  }

  if( !sound_enabled ) return;

  if( speaker_type != ula_synth_speaker_type ) {
    /* The inactive stream is deliberately not rendered. Discard its old Blip
     * history before it becomes active, then begin the newly selected path at
     * the current ULA state. */
    blip_buffer_clear( ula_buf, BLIP_BUFFER_DEF_ENTIRE_BUFF );
    blip_synth_set_output( ula_synth, ula_buf );
    ula_synth_speaker_type = speaker_type;
  }

  if( speaker_type == SOUND_SPEAKER_TYPE_BEEPER )
    blip_synth_update( ula_synth, at_tstates, beeper_ampl );
  else
    blip_synth_update( ula_synth, at_tstates, mic_ampl );
}

void
sound_ula( libspectrum_dword at_tstates, int mic_on, int beeper_on )
{
  ula_mic_on = !!mic_on;
  ula_beeper_on = !!beeper_on;
  sound_ula_update( at_tstates );
}

void
sound_tape( libspectrum_dword at_tstates )
{
  sound_ula_update( at_tstates );
}

const libspectrum_signed_word *
sound_ula_mic_output( void )
{
  return ula_synth_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ? NULL :
         ula_samples;
}

const libspectrum_signed_word *
sound_ula_beeper_output( void )
{
  return ula_synth_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ? ula_samples :
         NULL;
}

int
sound_ula_mic_output_count( void )
{
  return ula_synth_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ? 0 :
         ula_output_count;
}

int
sound_ula_beeper_output_count( void )
{
  return ula_synth_speaker_type == SOUND_SPEAKER_TYPE_BEEPER ?
         ula_output_count : 0;
}
