/* alsa_config.c: ALSA device configuration
   Copyright (c) 2006-2008 Gergely Szasz
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

/* ALSA's stack parameter allocation must not warn in debug builds. */
#ifndef NDEBUG
#define NDEBUG
#endif

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>

#include "alsa_config.h"
#include "settings.h"
#include "sound.h"
#include "ui/ui.h"

#define NUM_FRAMES 3

struct alsa_options {
  snd_pcm_uframes_t buffer, avail_min;
  int periods;
  bool explicit_geometry;
};

static int
alsa_parse_value( const char *option, const char *format, unsigned int *value,
                  unsigned int *length, char *separator )
{
  return sscanf( option, format, value, length, separator ) > 0 &&
         ( *separator == ',' || strlen( option ) == *length );
}

static int
alsa_parse_numeric_option( const char *option,
                           struct alsa_configuration *config,
                           struct alsa_options *options,
                           unsigned int *length, char *separator )
{
  unsigned int val;

  if( alsa_parse_value( option, " buffer=%u %n%c", &val, length, separator ) ) {
    if( val < 1 )
      fprintf( stderr, "Bad value for ALSA buffer size %u, using default\n",
               val );
    else {
      options->buffer = val;
      options->explicit_geometry = true;
    }
  } else if( alsa_parse_value( option, " frames=%u %n%c", &val,
                               length, separator ) ) {
    if( val < 1 || val > INT_MAX )
      fprintf( stderr, "Bad value for ALSA buffer size %u frames, "
               "using default (%d)\n", val, NUM_FRAMES );
    else {
      options->periods = val;
      options->explicit_geometry = true;
    }
  } else if( alsa_parse_value( option, " avail=%u %n%c", &val,
                               length, separator ) ) {
    if( val < 1 )
      fprintf( stderr, "Bad value for ALSA avail_min size %u frames, "
               "using default\n", val );
    else options->avail_min = val;
  } else if( alsa_parse_value( option, " target=%u %n%c", &val,
                               length, separator ) ) {
    config->target = val;
    config->mode = val ? ALSA_PACING_FIXED : ALSA_PACING_CAPACITY;
  } else {
    return 0;
  }
  return 1;
}

static void
alsa_parse_options( const char *device, struct alsa_configuration *config,
                    struct alsa_options *options )
{
  const char *option = device;
  unsigned int n;
  char tmp;

  while( option && *option ) {
    tmp = '*';
    if( alsa_parse_numeric_option( option, config, options, &n, &tmp ) ) {
      option += n + ( tmp == ',' );
      continue;
    }
    if( sscanf( option, " verbose %n%c", &n, &tmp ) == 1 &&
        ( tmp == ',' || strlen( option ) == n ) ) {
      config->verb = 1;
    } else {                                    /* try as device name */
      n = strlen( option );
      config->pcm_name = option;
      while( isspace( (unsigned char)*config->pcm_name ) ) config->pcm_name++;
      if( *config->pcm_name == '\'' ) config->pcm_name++;   /* force device */
    }
    option += n + ( tmp == ',' );
  }
}

static int
alsa_open_device( struct alsa_configuration *config, bool first_init )
{
  /* Open the sound device
   */
  if( config->pcm_name == NULL || *config->pcm_name == '\0' )
    config->pcm_name = "default";
  if( snd_pcm_open( &config->pcm_handle, config->pcm_name,
                    SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK ) < 0 ) {
    if( strcmp( config->pcm_name, "default" ) == 0 ) {
      /* we try a last one: plughw:0,0 but what a weired ALSA conf.... */
      if( snd_pcm_open( &config->pcm_handle, "plughw:0,0",
                        SND_PCM_STREAM_PLAYBACK,
                        SND_PCM_NONBLOCK ) < 0 ) {
        settings_current.sound = 0;
        ui_error( UI_ERROR_ERROR,
                  "couldn't open sound device 'default' and 'plughw:0,0' check ALSA configuration."
                  );
        return 1;
      } else {
        config->pcm_name = "plughw:0,0";
        if( first_init )
          fprintf( stderr,
                   "Couldn't open sound device 'default', using 'plughw:0,0' check ALSA configuration.\n"
                   );
      }
    }
    if( !config->pcm_handle ) {
      settings_current.sound = 0;
      ui_error( UI_ERROR_ERROR, "couldn't open sound device '%s'.",
                config->pcm_name );
      return 1;
    }
  }

  return 0;
}

static int
alsa_configure_format( struct alsa_configuration *config,
                       snd_pcm_hw_params_t *hw_params, int *freqptr,
                       int *stereoptr, bool first_init )
{
  unsigned int exact_rate;
#if defined WORDS_BIGENDIAN
  const snd_pcm_format_t format = SND_PCM_FORMAT_S16_BE;
#else
  const snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
#endif

  /* Init hw_params with full configuration space */
  if( snd_pcm_hw_params_any( config->pcm_handle, hw_params ) < 0 ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR,
              "couldn't get configuration space on sound device '%s'.",
              config->pcm_name );
    return 1;
  }

  if( snd_pcm_hw_params_set_access( config->pcm_handle, hw_params,
                                    SND_PCM_ACCESS_RW_INTERLEAVED ) < 0 ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "couldn't set access interleaved on '%s'.",
              config->pcm_name );
    return 1;
  }

  /* Set sample format */
  if( snd_pcm_hw_params_set_format( config->pcm_handle, hw_params,
                                    format ) < 0 ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "couldn't set format on '%s'.",
              config->pcm_name );
    return 1;
  }

  config->ch = *stereoptr ? 2 : 1;

  if( snd_pcm_hw_params_set_channels( config->pcm_handle, hw_params,
                                      config->ch )
      < 0 ) {
    fprintf( stderr, "Couldn't set %s to '%s'.\n", config->pcm_name,
             (*stereoptr ? "stereo" : "mono" ) );
    config->ch = *stereoptr ? 1 : 2;            /* try with opposite */
    if( snd_pcm_hw_params_set_channels( config->pcm_handle, hw_params,
                                        config->ch )
        < 0 ) {
      ui_error( UI_ERROR_ERROR, "couldn't set %s to '%s'.", config->pcm_name,
                (*stereoptr ? "stereo" : "mono" ) );
      settings_current.sound = 0;
      return 1;
    }
    *stereoptr = *stereoptr ? 0 : 1;            /* write back */
  }

  /* Set sample rate. If the exact rate is not supported */
  /* by the hardware, use nearest possible rate.         */
  exact_rate = *freqptr;
  if( snd_pcm_hw_params_set_rate_near( config->pcm_handle, hw_params,
                                       &exact_rate,
                                       NULL ) < 0 ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "couldn't set rate %d on '%s'.",
              *freqptr, config->pcm_name );
    return 1;
  }
  if( first_init && *freqptr != exact_rate ) {
    fprintf( stderr,
             "The rate %d Hz is not supported by your hardware. "
             "Using %d Hz instead.\n", *freqptr, exact_rate );
  }
  *freqptr = exact_rate;
  config->rate = exact_rate;

  return 0;
}

static int
alsa_request_geometry( struct alsa_configuration *config,
                       const struct alsa_options *options,
                       snd_pcm_uframes_t *buffer, snd_pcm_uframes_t *period )
{
  snd_pcm_uframes_t bsize = options->buffer, sound_periodsize;
  int nperiods = options->periods;
  bool explicit_geometry = options->explicit_geometry;
  libspectrum_dword processor_speed, interval;
  uint64_t requested_capacity;
  int frequency = config->rate;
  float hz;

  if( bsize != 0 ) sound_periodsize = bsize / nperiods;

  if( bsize == 0 ) {
    /* Adjust relative processor speed to deal with adjusting sound generation
       frequency against emulation speed (more flexible than adjusting generated
       sample rate) */
    hz = (float)sound_get_effective_processor_speed() /
         machine_current->timings.tstates_per_frame;
    /* Retain the legacy reference-frame storage size at high emulator speeds.
       This no longer determines the default service/wakeup period. */
    if( hz > 100.0 ) hz = 100.0;
    sound_periodsize = frequency / hz;
  }

  if( !explicit_geometry ) {
    /* Storage still covers legacy whole-frame/catch-up production. Only the
       service quantum follows the ordinary cadence; shrinking three periods
       to 5 ms each would make some exact whole-frame batches inadmissible. */
    requested_capacity = (uint64_t)sound_periodsize * nperiods;
    processor_speed = sound_get_effective_processor_speed();
    interval = sound_audio_interval_tstates( processor_speed );
    if( !processor_speed || !requested_capacity ||
        requested_capacity > UINT_MAX ) {
      ui_error( UI_ERROR_ERROR, "ALSA: invalid requested capacity on '%s'",
                config->pcm_name );
      settings_current.sound = 0;
      return 1;
    }
    bsize = requested_capacity;
    sound_periodsize = (uint64_t)frequency * interval / processor_speed;
    if( !sound_periodsize ) sound_periodsize = 1;
  }
  *buffer = bsize;
  *period = sound_periodsize;
  return 0;
}

static int
alsa_configure_geometry( struct alsa_configuration *config,
                         snd_pcm_hw_params_t *hw_params,
                         const struct alsa_options *options, bool first_init )
{
  snd_pcm_uframes_t bsize, sound_periodsize;
  unsigned int periods = options->periods;
  int dir, err;

  if( alsa_request_geometry( config, options, &bsize, &sound_periodsize ) )
    return 1;
  config->exact_periodsize = sound_periodsize;
  dir = -1;
  if( snd_pcm_hw_params_set_period_size_near( config->pcm_handle, hw_params,
                                              &config->exact_periodsize,
                                              &dir ) < 0 ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "couldn't set period size %d on '%s'.",
              (int)sound_periodsize, config->pcm_name );
    return 1;
  }

  if( first_init && ( config->exact_periodsize < sound_periodsize / 1.5 ||
                      config->exact_periodsize > sound_periodsize * 1.5    ) ) {
    fprintf( stderr,
             "The period size %d is not supported by your hardware. "
             "Using %d instead.\n", (int)sound_periodsize,
             (int)config->exact_periodsize );
  }

  if( options->explicit_geometry ) {
    /* Explicit buffer/frames retain their previous period-count semantics. */
    err = snd_pcm_hw_params_set_periods_near( config->pcm_handle, hw_params,
                                              &periods, NULL );
    if( !err && first_init && periods != (unsigned int)options->periods )
      fprintf( stderr, "%d periods is not supported by your hardware. "
               "Using %u instead.\n", options->periods, periods );
  } else {
    err = snd_pcm_hw_params_set_buffer_size_near( config->pcm_handle, hw_params,
                                                  &bsize );
  }
  if( err < 0 ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "couldn't set buffer geometry on '%s': %s",
              config->pcm_name, snd_strerror( err ) );
    return 1;
  }

  snd_pcm_hw_params_get_buffer_size( hw_params, &config->exact_bsize );

  /* Apply HW parameter settings to */
  /* PCM device and prepare device  */

  if( snd_pcm_hw_params( config->pcm_handle, hw_params ) < 0 ) {
    settings_current.sound = 0;
    ui_error( UI_ERROR_ERROR, "couldn't set hw_params on %s",
              config->pcm_name );
    return 1;
  }

  return 0;
}

static int
alsa_configure_software( struct alsa_configuration *config,
                         snd_pcm_uframes_t avail_min )
{
  snd_pcm_sw_params_t *sw_params;
  snd_pcm_uframes_t boundary;
  int err;

  snd_pcm_sw_params_alloca( &sw_params );
  if( ( err = snd_pcm_sw_params_current( config->pcm_handle,
                                         sw_params ) ) < 0 ) {
    ui_error( UI_ERROR_ERROR, "couldn't get sw_params from %s: %s",
              config->pcm_name,
              snd_strerror( err ) );
    return 1;
  }

  /* Explicit prefill/start: a shallow target must never wait for a prepared
     stream to consume. Use installed geometry, not requested period counts. */
  snd_pcm_sw_params_get_boundary( sw_params, &boundary );
  if( ( err = snd_pcm_sw_params_set_start_threshold( config->pcm_handle,
                                                     sw_params,
                                                     boundary ) ) < 0 ) {
    ui_error( UI_ERROR_ERROR, "couldn't set start_treshold on %s: %s",
              config->pcm_name,
              snd_strerror( err ) );
    return 1;
  }

  if( !avail_min )
    avail_min = config->exact_periodsize >> 1;
  if( ( err = snd_pcm_sw_params_set_avail_min( config->pcm_handle,
                                               sw_params, avail_min ) ) < 0 ) {
#if SND_LIB_VERSION < 0x10010
    if( ( err = snd_pcm_sw_params_set_sleep_min( config->pcm_handle,
                                                 sw_params, 1 ) ) < 0 ) {
      fprintf( stderr, "Unable to set minimal sleep 1 for %s: %s\n",
               config->pcm_name,
               snd_strerror( err ) );
    }
#else
    fprintf( stderr, "Unable to set avail min %s: %s\n", config->pcm_name,
             snd_strerror( err ) );
#endif
  }

#if SND_LIB_VERSION < 0x10010
  if( ( err = snd_pcm_sw_params_set_xfer_align( config->pcm_handle, sw_params,
                                                1 ) ) < 0 ) {
    ui_error( UI_ERROR_ERROR, "couldn't set xfer_allign on %s: %s",
              config->pcm_name,
              snd_strerror( err ) );
    return 1;
  }
#endif

  if( ( err = snd_pcm_sw_params( config->pcm_handle, sw_params ) ) < 0 ) {
    ui_error( UI_ERROR_ERROR, "couldn't set sw_params on %s: %s",
              config->pcm_name,
              snd_strerror( err ) );
    return 1;
  }

  return 0;
}

int
alsa_configure( const char *device, int *frequency, int *stereo,
                bool first_init, struct alsa_configuration *config )
{
  struct alsa_options options = { 0, 0, NUM_FRAMES, false };
  snd_pcm_hw_params_t *hw_params;
  int dir;

  memset( config, 0, sizeof( *config ) );
  config->mode = ALSA_PACING_ADAPTIVE;
  alsa_parse_options( device, config, &options );
  if( alsa_open_device( config, first_init ) ) goto failed;
  snd_pcm_hw_params_alloca( &hw_params );
  if( alsa_configure_format( config, hw_params, frequency, stereo,
                             first_init ) ||
      alsa_configure_geometry( config, hw_params, &options, first_init ) ||
      alsa_configure_software( config, options.avail_min ) ) goto failed;
  if( snd_pcm_hw_params_current( config->pcm_handle, hw_params ) < 0 ||
      snd_pcm_hw_params_get_buffer_size( hw_params,
                                         &config->exact_bsize ) < 0 ||
      snd_pcm_hw_params_get_period_size( hw_params, &config->exact_periodsize,
                                         &dir ) < 0 ) {
    ui_error( UI_ERROR_ERROR, "ALSA: querying installed geometry on '%s'",
              config->pcm_name );
    settings_current.sound = 0;
    goto failed;
  }
  config->batch = snd_pcm_hw_params_is_batch( hw_params );
  return 0;

failed:
  if( config->pcm_handle ) {
    snd_pcm_drop( config->pcm_handle );
    snd_pcm_close( config->pcm_handle );
    config->pcm_handle = NULL;
  }
  return 1;
}
