/* coreaudiosound.c: Mac OS X CoreAudio sound I/O
   Copyright (c) 2006-2021 Fredrick Meunier

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

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <AvailabilityMacros.h>
#include <AudioToolbox/AudioToolbox.h>

#include "machine.h"
#include "pcm_fifo.h"
#include "audio_pacing.h"
#include "audio_progress.h"
#include "sound.h"
#include "ui/ui.h"

/* The element was renamed in the macOS 12 SDK; both names have value zero. */
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
#define COREAUDIO_PROPERTY_ELEMENT_MAIN kAudioObjectPropertyElementMain
#else
#define COREAUDIO_PROPERTY_ELEMENT_MAIN kAudioObjectPropertyElementMaster
#endif

sfifo_t sound_fifo;

/* Number of emulation frames of audio latency, not audio sample frames. */
#define NUM_EMULATION_FRAMES 2

/* Signed 16-bit interleaved format supplied to the output unit. */
static AudioStreamBasicDescription device_format;
static struct audio_pacing pacing;
/* Static storage survives failed disposal. Owner-thread end/init cannot
   overlap production; the callback is the only concurrent backend caller. */
static struct audio_progress progress;
_Static_assert( SFIFO_MAX_BUFFER_SIZE < UINT32_MAX - 2,
                "FIFO must bound generation wrap during a producer wait" );
static int audio_output_started;

/* Unsigned modular counters, observed with relaxed loads outside the callback.
   Snapshots need not be mutually consistent. Wider totals can be accumulated
   from modular deltas outside the callback. Occupancy has a single writer. */
static struct {
  _Atomic unsigned int demand_frames;
  _Atomic unsigned int delivered_frames;
  _Atomic unsigned int underrun_callbacks;
  _Atomic unsigned int missing_frames;
  _Atomic unsigned int occupancy_frames;
  _Atomic unsigned int maximum_occupancy_frames;
  _Atomic unsigned int invalid_callbacks;
} audio_stats;

static int
init_audio_stats( void )
{
#define INIT_COUNTER(name) \
  atomic_init( &audio_stats.name, 0 ); \
  if( !atomic_is_lock_free( &audio_stats.name ) ) return 1
  INIT_COUNTER( demand_frames );
  INIT_COUNTER( delivered_frames );
  INIT_COUNTER( underrun_callbacks );
  INIT_COUNTER( missing_frames );
  INIT_COUNTER( occupancy_frames );
  INIT_COUNTER( maximum_occupancy_frames );
  INIT_COUNTER( invalid_callbacks );
#undef INIT_COUNTER
  return 0;
}

/* Producer and consumer transfers must be whole audio frames even though the
   FIFO reserves a single byte and permits partial byte transfers. */
static int
write_audio_frames( const void *data, unsigned int frames )
{
  int written = audio_pacing_write( &pacing, &sound_fifo,
                                    device_format.mBytesPerFrame,
                                    data, frames, audio_output_started );
  return written < 0 ? written :
    written * (int)device_format.mBytesPerFrame;
}

static unsigned int
fill_audio_frames( void *output, unsigned int requested_frames )
{
  unsigned int available_frames = 0, delivered_frames = 0;
  unsigned int bytes_per_frame = device_format.mBytesPerFrame;
  int used = pcm_fifo_consumer_used( &sound_fifo, bytes_per_frame );
  int delivered;

  if( used > 0 ) available_frames = used;
  atomic_store_explicit( &audio_stats.occupancy_frames, available_frames,
                         memory_order_relaxed );
  if( available_frames > atomic_load_explicit(
        &audio_stats.maximum_occupancy_frames, memory_order_relaxed ) )
    atomic_store_explicit( &audio_stats.maximum_occupancy_frames,
                           available_frames, memory_order_relaxed );

  delivered_frames = available_frames;
  if( delivered_frames > requested_frames ) delivered_frames = requested_frames;
  if( delivered_frames ) {
    delivered = pcm_fifo_read( &sound_fifo, bytes_per_frame, output,
                               delivered_frames );
    delivered_frames = delivered > 0 ? delivered : 0;
  }
  if( delivered_frames < requested_frames )
    memset( (char *)output + (size_t)delivered_frames * bytes_per_frame, 0,
            (size_t)( requested_frames - delivered_frames ) * bytes_per_frame );
  return delivered_frames;
}

static OSStatus
coreaudiowrite( void *in_ref_con, AudioUnitRenderActionFlags *action_flags,
               const AudioTimeStamp *timestamp, UInt32 bus_number,
               UInt32 requested_frames, AudioBufferList *buffers )
{
  unsigned int bytes_per_frame = device_format.mBytesPerFrame;
  unsigned int delivered_frames;
  UInt32 i;
  (void)in_ref_con;
  (void)action_flags;
  (void)timestamp;
  (void)bus_number;

  audio_pacing_callback_begin( &pacing );
  atomic_fetch_add_explicit( &audio_stats.demand_frames, requested_frames,
                            memory_order_relaxed );
  /* Check capacity by division, without overflowing a frame-to-byte product.
     FIFO byte counts are int, so reject requests outside that API's range. */
  if( !buffers || buffers->mNumberBuffers != 1 || !bytes_per_frame ||
      requested_frames > INT_MAX / bytes_per_frame ||
      buffers->mBuffers[0].mNumberChannels != device_format.mChannelsPerFrame ||
      ( requested_frames && !buffers->mBuffers[0].mData ) ||
      requested_frames > buffers->mBuffers[0].mDataByteSize / bytes_per_frame ) {
    /* Only supplied writable storage can be silenced on an invalid contract.
       Never consume queued audio or pretend an undersized buffer was filled. */
    if( buffers ) {
      for( i = 0; i < buffers->mNumberBuffers; i++ )
        if( buffers->mBuffers[i].mData && buffers->mBuffers[i].mDataByteSize )
          memset( buffers->mBuffers[i].mData, 0,
                  buffers->mBuffers[i].mDataByteSize );
    }
    atomic_fetch_add_explicit( &audio_stats.invalid_callbacks, 1,
                              memory_order_relaxed );
    audio_pacing_callback_finish( &pacing, &progress, 0, 0, true );
    return kAudio_ParamError;
  }

  delivered_frames = fill_audio_frames( buffers->mBuffers[0].mData,
                                        requested_frames );
  atomic_fetch_add_explicit( &audio_stats.delivered_frames, delivered_frames,
                            memory_order_relaxed );
  if( delivered_frames < requested_frames ) {
    atomic_fetch_add_explicit( &audio_stats.underrun_callbacks, 1,
                              memory_order_relaxed );
    atomic_fetch_add_explicit( &audio_stats.missing_frames,
                              requested_frames - delivered_frames,
                              memory_order_relaxed );
  }
  audio_pacing_callback_finish( &pacing, &progress, requested_frames,
                                delivered_frames, false );
  return noErr;
}

/* The default output unit converts our PCM to the device's output format. */
static AudioUnit output_unit;
static int audio_unit_initialized;
/* A failed teardown must not permit reinitialization of live FIFO storage. */
static int audio_teardown_failed;

/* Get the default output device for the HAL. */
static int
get_default_output_device( AudioDeviceID *device )
{
  OSStatus err;
  UInt32 count = sizeof( *device );
  AudioObjectPropertyAddress address = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal,
    COREAUDIO_PROPERTY_ELEMENT_MAIN
  };

  err = AudioObjectGetPropertyData( kAudioObjectSystemObject, &address,
                                   0, NULL, &count, device );
  if( err != noErr || *device == kAudioObjectUnknown ) {
    ui_error( UI_ERROR_ERROR, "Default audio device unavailable: %ld", (long)err );
    return 1;
  }
  return 0;
}

/* Get the nominal sample rate used by the supplied device. */
static int
get_default_sample_rate( AudioDeviceID device, Float64 *rate )
{
  OSStatus err;
  UInt32 count = sizeof( *rate );
  AudioObjectPropertyAddress address = {
    kAudioDevicePropertyNominalSampleRate,
    kAudioObjectPropertyScopeGlobal,
    COREAUDIO_PROPERTY_ELEMENT_MAIN
  };

  err = AudioObjectGetPropertyData( device, &address, 0, NULL, &count, rate );
  if( err != noErr ) {
    ui_error( UI_ERROR_ERROR, "Default audio sample rate unavailable: %ld",
              (long)err );
    return 1;
  }
  return 0;
}

void
sound_lowlevel_end( void )
{
  OSStatus err;
  if( audio_output_started ) {
    err = AudioOutputUnitStop( output_unit );
    if( err ) {
      ui_error( UI_ERROR_ERROR, "AudioOutputUnitStop=%ld", (long)err );
      audio_teardown_failed = 1;
      return;
    }
    audio_output_started = 0;
  }
  if( output_unit ) {
    if( audio_unit_initialized ) {
      err = AudioUnitUninitialize( output_unit );
      if( err ) ui_error( UI_ERROR_ERROR, "AudioUnitUninitialize=%ld", (long)err );
    }
    err = AudioComponentInstanceDispose( output_unit );
    if( err ) {
      ui_error( UI_ERROR_ERROR, "AudioComponentInstanceDispose=%ld", (long)err );
      audio_teardown_failed = 1;
      return;
    }
    output_unit = NULL;
    audio_unit_initialized = 0;
  }
  if( atomic_load( &progress.error ) )
    ui_error( UI_ERROR_ERROR, "Core Audio progress wake: %s",
              strerror( atomic_load( &progress.error ) ) );
  audio_progress_close( &progress );
  if( sound_fifo.buffer ) sfifo_close( &sound_fifo );
  pacing.ready = false;
}

int
sound_lowlevel_init( const char *dev, int *freqptr, int *stereoptr )
{
  OSStatus err;
  AudioDeviceID device = kAudioObjectUnknown;
  AudioComponent component;
  AudioComponentDescription desc = {
    kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
    kAudioUnitManufacturer_Apple, 0, 0
  };
  AURenderCallbackStruct input = { coreaudiowrite, NULL };
  double emulation_hz, audio_frames_per_batch;
  unsigned int capacity_frames;
  UInt32 demand = 0, property_size;
  int envelope_ok = 0;
  AudioObjectPropertyAddress buffer_address = {
    kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
    COREAUDIO_PROPERTY_ELEMENT_MAIN
  };
  int error;
  (void)dev;

  if( audio_teardown_failed || output_unit || sound_fifo.buffer ) {
    ui_error( UI_ERROR_ERROR, "Previous audio output has not been disposed" );
    return 1;
  }
  if( get_default_output_device( &device ) ||
      get_default_sample_rate( device, &device_format.mSampleRate ) ) return 1;
  if( !isfinite( device_format.mSampleRate ) ||
      device_format.mSampleRate < 1 || device_format.mSampleRate > INT_MAX ) {
    ui_error( UI_ERROR_ERROR, "Invalid audio sample rate" );
    return 1;
  }
  *freqptr = device_format.mSampleRate;
  device_format.mFormatID = kAudioFormatLinearPCM;
  device_format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger |
                               kLinearPCMFormatFlagIsPacked;
#ifdef WORDS_BIGENDIAN
  device_format.mFormatFlags |= kLinearPCMFormatFlagIsBigEndian;
#endif
  device_format.mChannelsPerFrame = *stereoptr ? 2 : 1;
  device_format.mBytesPerFrame = device_format.mChannelsPerFrame * 2;
  device_format.mBytesPerPacket = device_format.mBytesPerFrame;
  device_format.mFramesPerPacket = 1;
  device_format.mBitsPerChannel = 16;

  /* Adjust relative processor speed to deal with sound generation frequency
     against emulation speed (more flexible than adjusting sample rate).
     Preserve the existing physical allocation geometry. */
  emulation_hz = (double)sound_get_effective_processor_speed() /
                machine_current->timings.tstates_per_frame;
  if( !isfinite( emulation_hz ) || emulation_hz <= 0 ) {
    ui_error( UI_ERROR_ERROR, "Invalid audio generation frequency" );
    return 1;
  }
  /* Amount of audio accumulated before yielding back to the OS. There is
     little point batching above 100 Hz: excessive wakeups can cause the OS
     to downgrade us as a hog. Historically, this cap improved accelerated
     playback from about 2000% to 5000% on the author's Mac. */
  if( emulation_hz > 100.0 ) emulation_hz = 100.0;
  audio_frames_per_batch = device_format.mSampleRate / emulation_hz;
  if( audio_frames_per_batch < 1 ||
      audio_frames_per_batch > SFIFO_MAX_BUFFER_SIZE /
        device_format.mBytesPerFrame / NUM_EMULATION_FRAMES ) {
    ui_error( UI_ERROR_ERROR, "Audio FIFO capacity out of range" );
    return 1;
  }
  capacity_frames = NUM_EMULATION_FRAMES * (unsigned int)audio_frames_per_batch;
  if( audio_progress_init( &progress ) || init_audio_stats() ) {
    ui_error( UI_ERROR_ERROR, "Core Audio counters must be lock-free" );
    return 1;
  }
  /* Bytes per frame already includes channels. sfifo reserves its own byte. */
  error = sfifo_init( &sound_fifo,
                      capacity_frames * device_format.mBytesPerFrame );
  if( error ) {
    ui_error( UI_ERROR_ERROR, "Problem initialising sound fifo: %s",
              strerror( -error ) );
    return 1;
  }
  component = AudioComponentFindNext( NULL, &desc );
  if( !component ) {
    ui_error( UI_ERROR_ERROR, "AudioComponentFindNext" );
    goto fail;
  }
  err = AudioComponentInstanceNew( component, &output_unit );
  if( err ) {
    ui_error( UI_ERROR_ERROR, "AudioComponentInstanceNew=%ld", (long)err );
    goto fail;
  }
  err = AudioUnitSetProperty( output_unit, kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, 0, &input, sizeof( input ) );
  if( err ) {
    ui_error( UI_ERROR_ERROR, "AudioUnitSetProperty-CB=%ld", (long)err );
    goto fail;
  }
  err = AudioUnitSetProperty( output_unit, kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, 0, &device_format,
                              sizeof( device_format ) );
  if( err ) {
    ui_error( UI_ERROR_ERROR, "AudioUnitSetProperty-SF=%ld", (long)err );
    goto fail;
  }
  /* Bound each client input pull (QA1533), not merely the largest request
     observed so far. Read back before and after initialization. */
  property_size = sizeof( demand );
  err = AudioObjectGetPropertyData( device, &buffer_address, 0, NULL,
                                    &property_size, &demand );
  if( !err && property_size == sizeof( demand ) && demand ) {
    err = AudioUnitSetProperty( output_unit,
            kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
            0, &demand, sizeof( demand ) );
    if( !err ) {
      property_size = sizeof( demand );
      err = AudioUnitGetProperty( output_unit,
              kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
              0, &demand, &property_size );
      envelope_ok = !err && property_size == sizeof( demand ) && demand;
    }
  }
  err = AudioUnitInitialize( output_unit );
  if( err ) {
    ui_error( UI_ERROR_ERROR, "AudioUnitInitialize=%ld", (long)err );
    goto fail;
  }
  audio_unit_initialized = 1;
  if( envelope_ok ) {
    property_size = sizeof( demand );
    err = AudioUnitGetProperty( output_unit,
            kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
            0, &demand, &property_size );
    envelope_ok = !err && property_size == sizeof( demand ) && demand &&
                 demand <= INT_MAX / device_format.mBytesPerFrame;
  }
  if( !envelope_ok ) {
    demand = 0;
    ui_error( UI_ERROR_WARNING,
              "Core Audio callback envelope unavailable; using FIFO capacity" );
  }
  if( audio_pacing_init_rate( &pacing, &sound_fifo,
        device_format.mBytesPerFrame, *freqptr,
        sound_get_effective_processor_speed(),
        machine_current->timings.tstates_per_frame, demand ) )
    goto pacing_fail;
  /* Wait to run sound until we have some sound to play. */
  audio_output_started = 0;
  return 0;

pacing_fail:
  ui_error( UI_ERROR_ERROR, "Cannot initialize Core Audio pacing geometry" );
fail:
  sound_lowlevel_end();
  return 1;
}

int
sound_lowlevel_reserve( unsigned int frames )
{
  return audio_pacing_reserve( &pacing, &progress, &sound_fifo,
                              device_format.mBytesPerFrame, frames,
                              sound_normal_producer_context(),
                              audio_output_started );
}

/* Copy the frame-batched sound data to the FIFO. */
void
sound_lowlevel_frame( libspectrum_signed_word *data, int sample_count )
{
  unsigned int remaining_frames;
  unsigned int channels = device_format.mChannelsPerFrame;
  unsigned int bytes_per_frame = device_format.mBytesPerFrame;
  const char *bytes = (const char *)data;
  int written_bytes;

  if( sample_count < 0 || !channels || sample_count % channels ) {
    ui_error( UI_ERROR_ERROR, "Invalid Core Audio sample count" );
    return;
  }
  remaining_frames = sample_count / channels;
  while( remaining_frames ) {
    written_bytes = write_audio_frames( bytes, remaining_frames );
    if( written_bytes < 0 ) {
      ui_error( UI_ERROR_ERROR, "Couldn't write sound fifo: %s",
                strerror( -written_bytes ) );
      return;
    }
    if( !written_bytes ) {
      int error = audio_pacing_wait( &pacing, &progress, &sound_fifo,
                                     bytes_per_frame, 1, false );
      if( error ) {
        ui_error( UI_ERROR_ERROR, "Core Audio progress wait: %s",
                  strerror( -error ) );
        return;
      }
    }
    bytes += written_bytes;
    remaining_frames -= written_bytes / bytes_per_frame;
  }
  if( !audio_output_started ) {
    /* Start rendering. DefaultOutputUnit performs any format conversions
       needed by the default device. */
    OSStatus err = AudioOutputUnitStart( output_unit );
    if( err ) {
      ui_error( UI_ERROR_ERROR, "AudioOutputUnitStart=%ld", (long)err );
      return;
    }
    audio_output_started = 1;
  }
}
