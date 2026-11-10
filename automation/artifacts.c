/* artifacts.c: automation screen and audio artifacts
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
#ifdef ENABLE_AUTOMATION

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "artifacts.h"
#include "automation.h"
#include "compat.h"
#include "screenshot.h"
#include "settings.h"
#include "ui/ui.h"
#include "utils.h"

static libspectrum_byte *screen_rgb;
static size_t screen_width, screen_height;
static size_t screen_file_width, screen_file_height;
static libspectrum_signed_word *pcm;
static size_t pcm_count, pcm_capacity;
static int pcm_rate, pcm_channels, audio_initialized;
static uLong screen_crc, screen_file_crc, pcm_crc, audio_file_crc;
static size_t screen_file_size, audio_file_size;
static int screen_written, audio_written;
static const char *screen_error, *audio_error;

void
automation_artifacts_capture_screen( const libspectrum_byte *pixels,
                                     size_t width, size_t height )
{
  static const libspectrum_byte palette[16][3] = {
    {0, 0, 0}, {0, 0, 192}, {192, 0, 0}, {192, 0, 192}, {0, 192, 0},
    {0, 192, 192}, {192, 192, 0}, {192, 192, 192}, {0, 0, 0},
    {0, 0, 255}, {255, 0, 0}, {255, 0, 255}, {0, 255, 0},
    {0, 255, 255}, {255, 255, 0}, {255, 255, 255}
  };
  size_t count = width * height;

  if( !pixels ) return;

  screen_rgb = libspectrum_renew( libspectrum_byte, screen_rgb, count * 3 );
  for( size_t i = 0; i < count; i++ ) {
    libspectrum_byte c = pixels[i] & 15;

    if( settings_current.bw_tv ) {
      int grey = ( 299 * palette[c][0] + 587 * palette[c][1] +
                   114 * palette[c][2] + 500 ) / 1000;
      screen_rgb[3 * i] = screen_rgb[3 * i + 1] = screen_rgb[3 * i + 2] = grey;
    } else {
      memcpy( screen_rgb + 3 * i, palette[c], 3 );
    }
  }

  screen_width = width;
  screen_height = height;
}

void
automation_artifacts_audio_initialized( int sample_rate, int channels )
{
  audio_initialized = 1;
  pcm_rate = sample_rate;
  pcm_channels = channels;
}

void
automation_artifacts_capture_pcm( const libspectrum_signed_word *samples,
                                  int count, int sample_rate, int channels )
{
  size_t needed;

  if( count <= 0 ) return;

  needed = pcm_count + count;
  if( needed > pcm_capacity ) {
    pcm_capacity = needed + needed / 2 + 1024;
    pcm = libspectrum_renew( libspectrum_signed_word, pcm, pcm_capacity );
  }

  memcpy( pcm + pcm_count, samples, count * sizeof( *samples ) );
  pcm_count = needed;
  pcm_rate = sample_rate;
  pcm_channels = channels;
}

static void
put_le16( libspectrum_byte *p, unsigned value )
{
  p[0] = value;
  p[1] = value >> 8;
}

static void
put_le32( libspectrum_byte *p, unsigned long value )
{
  p[0] = value;
  p[1] = value >> 8;
  p[2] = value >> 16;
  p[3] = value >> 24;
}

static int
file_identity( const char *path, uLong *crc, size_t *size )
{
  utils_file file;

  if( utils_read_file( path, &file ) ) return 1;
  *size = libspectrum_file_size( &file );
  *crc = automation_checksum( libspectrum_file_data( &file ), *size );
  utils_close_file( &file );

  return 0;
}

static int
write_screen( const char *directory )
{
  char *path;
  int error;
  scaler_type scaler = current_scaler == SCALER_NUM ? SCALER_NORMAL :
                       current_scaler;

  if( !screen_rgb ) {
    screen_error = "logical framebuffer unavailable";
    return 1;
  }

  path = libspectrum_new( char, strlen( directory ) + 12 );
  sprintf( path, "%s" FUSE_DIR_SEP_STR "screen.png", directory );
#ifdef USE_LIBPNG
  error = screenshot_write( path, scaler ) ||
          file_identity( path, &screen_file_crc, &screen_file_size );
  if( !error ) {
    float factor = scaler_get_scaling_factor( scaler );
    screen_crc = automation_checksum( screen_rgb, screen_width * screen_height * 3 );
    screen_file_width = screen_width * factor;
    screen_file_height = screen_height * factor;
    screen_written = 1;
  } else {
    screen_error = "could not encode or finalize screen.png";
    compat_file_unlink( path );
  }
#else
  ui_error( UI_ERROR_ERROR, "Screen capture requires PNG support" );
  screen_error = "screen capture requires PNG support";
  error = 1;
#endif
  libspectrum_free( path );

  return error;
}

static libspectrum_byte *
make_pcm_bytes( size_t *byte_count )
{
  libspectrum_byte *bytes;

  *byte_count = pcm_count * 2;
  bytes = libspectrum_new( libspectrum_byte,
                           *byte_count ? *byte_count : 1 );
  for( size_t i = 0; i < pcm_count; i++ )
    put_le16( bytes + 2 * i, (unsigned short)pcm[i] );

  return bytes;
}

static void
make_wav_header( libspectrum_byte header[44], size_t byte_count )
{
  memset( header, 0, 44 );
  memcpy( header, "RIFF", 4 );
  put_le32( header + 4, 36 + byte_count );
  memcpy( header + 8, "WAVEfmt ", 8 );
  put_le32( header + 16, 16 );
  put_le16( header + 20, 1 );
  put_le16( header + 22, pcm_channels );
  put_le32( header + 24, pcm_rate );
  put_le32( header + 28, pcm_rate * pcm_channels * 2 );
  put_le16( header + 32, pcm_channels * 2 );
  put_le16( header + 34, 16 );
  memcpy( header + 36, "data", 4 );
  put_le32( header + 40, byte_count );
}

static int
write_wav( const char *path, const libspectrum_byte header[44],
           const libspectrum_byte *bytes, size_t byte_count )
{
  compat_fd file = compat_file_open( path, 1 );

  if( file == COMPAT_FILE_OPEN_FAILED ) return 1;
  if( compat_file_write( file, header, 44 ) ||
      ( byte_count && compat_file_write( file, bytes, byte_count ) ) ) {
    compat_file_close( file );
    compat_file_unlink( path );
    return 1;
  }

  if( compat_file_close( file ) ) {
    compat_file_unlink( path );
    return 1;
  }

  return 0;
}

static int
write_audio( const char *directory )
{
  libspectrum_byte header[44], *bytes;
  size_t byte_count;
  char *path;
  int error;

  if( !audio_initialized ) {
    audio_error = "null sound capture device was not initialized";
    return 1;
  }

  bytes = make_pcm_bytes( &byte_count );
  make_wav_header( header, byte_count );
  path = libspectrum_new( char, strlen( directory ) + 11 );
  sprintf( path, "%s" FUSE_DIR_SEP_STR "audio.wav", directory );

  error = write_wav( path, header, bytes, byte_count ) ||
          file_identity( path, &audio_file_crc, &audio_file_size );
  if( !error ) {
    pcm_crc = automation_checksum( bytes, byte_count );
    audio_written = 1;
  } else {
    audio_error = "could not write or finalize audio.wav";
  }

  libspectrum_free( bytes );
  libspectrum_free( path );
  return error;
}

int
automation_artifacts_write( const char *directory, int capture_screen,
                            int capture_audio )
{
  int error = 0;

  if( capture_screen ) error |= write_screen( directory );
  if( capture_audio ) error |= write_audio( directory );
  return error;
}

static void
write_file_identity( automation_json *json, const char *path, size_t size,
                     uLong crc )
{
  char text[9];

  automation_json_string( json, "path", path );
  automation_json_ulong( json, "size", size );
  snprintf( text, sizeof( text ), "%08lx", crc );
  automation_json_string( json, "file_crc32", text );
}

static void
write_screen_json( automation_json *json )
{
  char text[9];

  if( !screen_written ) return;

  automation_json_object_begin( json, "screen" );
  automation_json_string( json, "status", "ok" );
  automation_json_string( json, "stage", AUTOMATION_ARTIFACT_STAGE_SCREEN );
  write_file_identity( json, "screen.png", screen_file_size, screen_file_crc );
  automation_json_ulong( json, "width", screen_file_width );
  automation_json_ulong( json, "height", screen_file_height );
  automation_json_string( json, "pixel_format", "rgb24" );
  snprintf( text, sizeof( text ), "%08lx", screen_crc );
  automation_json_string( json, "pixel_crc32", text );
  automation_json_end( json );
}

static void
write_audio_json( automation_json *json )
{
  char text[9];

  if( !audio_written ) return;

  automation_json_object_begin( json, "audio" );
  automation_json_string( json, "status", "ok" );
  automation_json_string( json, "stage", AUTOMATION_ARTIFACT_STAGE_AUDIO );
  write_file_identity( json, "audio.wav", audio_file_size, audio_file_crc );
  automation_json_ulong( json, "sample_rate", pcm_rate );
  automation_json_ulong( json, "channels", pcm_channels );
  automation_json_string( json, "format", "s16le" );
  automation_json_ulong( json, "frames",
                         pcm_channels ? pcm_count / pcm_channels : 0 );
  automation_json_ulong( json, "start_frame_tstate", 0 );
  snprintf( text, sizeof( text ), "%08lx", pcm_crc );
  automation_json_string( json, "pcm_crc32", text );
  automation_json_end( json );
}

static void
write_error_json( automation_json *json, const char *name, const char *stage,
                  const char *message )
{
  automation_json_object_begin( json, name );
  automation_json_string( json, "status", "error" );
  automation_json_string( json, "stage", stage );
  automation_json_string( json, "message", message );
  automation_json_end( json );
}

void
automation_artifacts_write_json( automation_json *json )
{
  write_screen_json( json );
  if( screen_error )
    write_error_json( json, "screen", AUTOMATION_ARTIFACT_STAGE_SCREEN,
                      screen_error );

  write_audio_json( json );
  if( audio_error )
    write_error_json( json, "audio", AUTOMATION_ARTIFACT_STAGE_AUDIO,
                      audio_error );
}

void
automation_artifacts_end( void )
{
  libspectrum_free( screen_rgb );
  libspectrum_free( pcm );
}

#endif
