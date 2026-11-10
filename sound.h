/* sound.h: Sound support
   Copyright (c) 2000-2016 Russell Marks, Matan Ziv-Av, Philip Kendall
   Copyright (c) 2016-2021 Fredrick Meunier

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

#ifndef FUSE_SOUND_H
#define FUSE_SOUND_H

#include "libspectrum.h"

/* Internal normal production cadence, not a user preference. */
#define SOUND_AUDIO_CADENCE_MS 5

/* Round down to whole CPU T-states. Widen before multiplying so the clock
   conversion cannot overflow; the interval is at most the supplied clock. */
static inline libspectrum_dword
sound_audio_interval_tstates( libspectrum_dword processor_speed )
{
  return (libspectrum_qword)processor_speed * SOUND_AUDIO_CADENCE_MS / 1000;
}

enum sound_speaker_type {
  SOUND_SPEAKER_TYPE_AUTOMATIC,
  SOUND_SPEAKER_TYPE_TV,
  SOUND_SPEAKER_TYPE_BEEPER,
  SOUND_SPEAKER_TYPE_UNFILTERED
};

#define SOUND_ROUTE_ULA_MIC     0x01
#define SOUND_ROUTE_ULA_BEEPER  0x02
#define SOUND_ROUTE_BUILTIN_AY  0x04
#define SOUND_ROUTE_USPEECH     0x08

int sound_resolve_speaker_type( int selected_type, int machine_capabilities,
                                int uspeech_enabled );
unsigned int sound_tv_source_routes( int speaker_type,
                                     int machine_capabilities );

void sound_register_startup( void );

void sound_init( const char *device );
void sound_pause( void );
void sound_unpause( void );
void sound_end( void );
void sound_ay_write( int reg, int val, libspectrum_dword now );
void sound_ay_reset( void );
void sound_specdrum_write( libspectrum_word port, libspectrum_byte val );
void sound_covox_write( libspectrum_word port, libspectrum_byte val );
void sound_generalsound_write( libspectrum_dword at_tstates, int left,
                               int right );
void sound_sp0256_write( libspectrum_dword at_tstates,
                         libspectrum_signed_word val );
void sound_frame( void );
int sound_layout_stale( void );
/* Call before a clock discontinuity, an out-of-band source reset/restore,
 * or a transition to/from RZX's forced-event timing. Cancel interior cuts for
 * the rest of this frame; sound_frame() rearms the next eligible frame.
 * If this frame has already been partly consumed, reinitialize audio to avoid
 * reusing committed history. Published PCM cannot be retracted, so continuity
 * across the discontinuity is not guaranteed. This does not change tstates.
 */
void sound_suspend_subframe( void );
#define SOUND_AMPL_BEEPER ( 50 * 256 )
#define SOUND_AMPL_TAPE   ( 2 * 256 )

/* Derive the raw ULA output-node and internal-speaker levels. */
void sound_ula_levels( int mic_on, int beeper_on, int *mic_ampl,
                       int *beeper_ampl );
void sound_ula( libspectrum_dword at_tstates, int mic_on, int beeper_on );
void sound_tape( libspectrum_dword at_tstates );

/* Valid until the next sound_frame(). Only the selected ULA path is rendered:
 * the accessor for the inactive path returns NULL and its count is zero.
 */
const libspectrum_signed_word *sound_ula_mic_output( void );
const libspectrum_signed_word *sound_ula_beeper_output( void );
int sound_ula_mic_output_count( void );
int sound_ula_beeper_output_count( void );

libspectrum_dword sound_get_effective_processor_speed( void );

extern int sound_enabled;
extern int sound_framesiz;

/* Stereo separation types:
 *  * ACB is used in the Melodik interface.
 *  * ABC stereo is used in the Pentagon/Scorpion.
 *  * BAC stereo does seem to exist but is quite rare:
 *      Z80Stealth emulates BAC stereo but that's about all.
 *  * CAB, BCA and CBA don't get many search results.
 */

#define SOUND_STEREO_AY_NONE	0
#define SOUND_STEREO_AY_ACB	1
#define SOUND_STEREO_AY_ABC	2

extern int sound_stereo_ay;

/* Channels in the output stream: General Sound needs two whether or not
   the AY channels are being separated. */
extern int sound_channels;

/* The low-level sound interface */

int sound_lowlevel_init( const char *device, int *freqptr, int *stereoptr );
void sound_lowlevel_end( void );
void sound_lowlevel_frame( libspectrum_signed_word *data, int len );
#ifdef SOUND_PCM_ADMISSION
/* Sole producer: wait for a complete batch before synthesis mutation.
   Negative errno on failure; no producer writes may intervene before publish. */
int sound_lowlevel_reserve( unsigned int frames );
#endif
/* Ordinary subframe context; frame-only/RZX/discontinuities use capacity. */
int sound_normal_producer_context( void );

#endif				/* #ifndef FUSE_SOUND_H */
