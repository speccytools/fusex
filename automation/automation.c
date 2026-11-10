/* automation.c: development-only one-shot automation coordinator
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

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "automation.h"
#include "artifacts.h"
#include "json.h"
#include "result_settings.h"
#include "state.h"
#include "compat.h"
#include "fuse.h"
#include "event.h"
#include "machine.h"
#include "memory_pages.h"
#include "settings.h"
#include "utils.h"
#include "z80/z80.h"

typedef struct diagnostic { ui_error_level severity;
                            char *message;} diagnostic;
static automation_scenario scenario;
static automation_result result;
static unsigned long first_frame;
static diagnostic *diagnostics;
static size_t diagnostics_count;
static uLong tape_crc32, disk_crc32;
static size_t tape_size, disk_size;
static int tape_recorded, disk_recorded;
static char *tape_name, *disk_name, *disk_controller;
static unsigned int disk_drive;
static uLong rzx_crc32, snapshot_crc32;
static size_t rzx_size, snapshot_size;
static char *rzx_name, *snapshot_name;
static int rzx_recorded, snapshot_recorded;
static int armed, disk_motor_on, disk_motor_observed;
static unsigned long disk_motor_off_frame;
static int sample_event_type, sample_event_scheduled;

static int
parse_count( const char *text, unsigned long *value, int allow_zero )
{
  char *end;
  errno = 0;
  *value = strtoul( text, &end, 0 );
  return errno || !text[0] || *end || ( !allow_zero && !*value );
}

static int
set_address( const char *text, automation_condition *condition )
{
  unsigned long value;

  if( parse_count( text, &value, 1 ) || value > 0xffff ) {
    fprintf( stderr, "invalid automation PC address: %s\n", text );
    return 1;
  }

  condition->present = 1;
  condition->address = value;

  return 0;
}

int
automation_options_present( int argc, char **argv )
{
  for( int i = 1; i < argc; i++ ) {
    if( !strncmp( argv[i], "--automation-", 13 ) ) return 1;
  }

  return 0;
}

int
automation_set_output_directory( const char *directory )
{
  char *copy = utils_safe_strdup( directory );
  libspectrum_free( scenario.output_directory );
  scenario.output_directory = copy;

  return 0;
}

int
automation_set_frame_limit( const char *frames )
{
  if( parse_count( frames, &scenario.maximum_frames, 0 ) ) {
    fprintf( stderr, "invalid automation frame count: %s\n", frames );
    return 1;
  }

  return 0;
}

void
automation_set_until_rzx_end( void )
{
  scenario.until_rzx_end = 1;
}

void
automation_set_until_disk_idle( void )
{
  scenario.until_disk_idle = 1;
  if( !scenario.disk_idle_frames ) scenario.disk_idle_frames = 50;
}

int
automation_set_disk_idle_frames( const char *frames )
{
  if( parse_count( frames, &scenario.disk_idle_frames, 0 ) ) {
    fprintf( stderr, "invalid automation disk idle frame count: %s\n", frames );
    return 1;
  }
  return 0;
}

int
automation_set_sample_tstates( const char *text )
{
  if( parse_count( text, &scenario.sample_tstates, 0 ) ||
      scenario.sample_tstates > 5000 ) {
    fprintf( stderr, "invalid automation sample T-state offset: %s\n", text );
    return 1;
  }
  return 0;
}

void
automation_set_capture_screen( void )
{
  scenario.capture_screen = 1;
}

void
automation_set_capture_audio( void )
{
  scenario.capture_audio = 1;
}

int
automation_capture_screen_enabled( void )
{
  return scenario.capture_screen;
}

int
automation_capture_audio_enabled( void )
{
  return scenario.capture_audio;
}

int
automation_set_success_pc( const char *text )
{
  return set_address( text, &scenario.success );
}

int
automation_set_failure_pc( const char *text )
{
  return set_address( text, &scenario.failure );
}

int
automation_set_failure_pc_ignore( const char *text )
{
  unsigned long value;

  if( parse_count( text, &value, 1 ) ) {
    fprintf( stderr, "invalid automation ignore count: %s\n", text );
    return 1;
  }

  scenario.failure.ignore = value;
  return 0;
}

int
automation_validate_scenario( void )
{
  int conditions = scenario.success.present || scenario.failure.present;

  if( !scenario.output_directory && !scenario.maximum_frames && !conditions &&
      !scenario.failure.ignore && !scenario.until_rzx_end &&
      !scenario.until_disk_idle && !scenario.disk_idle_frames &&
      !scenario.capture_screen && !scenario.capture_audio &&
      !scenario.sample_tstates )
    return 0;

  if( !scenario.output_directory || !scenario.maximum_frames ||
      ( conditions && !scenario.success.present ) ||
      ( scenario.failure.ignore && !scenario.failure.present ) ||
      ( scenario.disk_idle_frames && !scenario.until_disk_idle ) ||
      ( scenario.until_rzx_end && scenario.until_disk_idle ) ||
      ( scenario.sample_tstates && ( conditions || scenario.until_rzx_end ||
                                     scenario.until_disk_idle ) ) ) {
    fprintf( stderr,
             "automation requires --automation-output, a frame limit, and a success PC for condition runs\n" );
    return 1;
  }

  return 0;
}

int
automation_active( void )
{
  return scenario.output_directory != NULL;
}

/* Whether the PC-boundary automation check needs to run. When no success or
   failure PC is configured (e.g. a pure frame-count or RZX-completion run),
   automation_check_pc() would only be called per-opcode and immediately
   return, so gating it off avoids that per-opcode call overhead. */
int
automation_pc_check_active( void )
{
  return scenario.success.present || scenario.failure.present;
}

void
automation_arm( unsigned long frame_count )
{
  first_frame = frame_count;
  result.frames_completed = 0;
  armed = 1;
  sample_event_scheduled = 0;
  if( scenario.sample_tstates )
    sample_event_type = event_register( NULL, "Automation sample offset" );
  if( result.termination == AUTOMATION_TERMINATION_FRAMES )
    result.termination =
      ( scenario.success.present || scenario.until_rzx_end ||
        scenario.until_disk_idle ) ?
      AUTOMATION_TERMINATION_DEADLINE : AUTOMATION_TERMINATION_FRAMES;
}

int
automation_check_pc( libspectrum_word pc )
{
  if( !automation_active() || !scenario.success.present )
    return 0;

  if( scenario.failure.present && pc == scenario.failure.address &&
      scenario.failure.hits++ >= scenario.failure.ignore ) {
    result.termination = AUTOMATION_TERMINATION_FAILURE;
    result.pc = pc;
    fuse_exiting = 1;
    return 1;
  }

  if( pc == scenario.success.address ) {
    result.termination = AUTOMATION_TERMINATION_SUCCESS;
    result.pc = pc;
    fuse_exiting = 1;
    return 1;
  }

  return 0;
}

int
automation_frame_limit_reached( unsigned long frame_count )
{
  result.frames_completed = frame_count - first_frame;
  if( scenario.until_disk_idle && disk_motor_observed && !disk_motor_on &&
      frame_count - disk_motor_off_frame >= scenario.disk_idle_frames ) {
    result.termination = AUTOMATION_TERMINATION_DISK_IDLE;
    return 1;
  }
  if( result.frames_completed >= scenario.maximum_frames &&
      scenario.sample_tstates ) {
    if( !sample_event_scheduled ) {
      event_add( scenario.sample_tstates, sample_event_type );
      sample_event_scheduled = 1;
    }
    return tstates >= scenario.sample_tstates;
  }
  return result.frames_completed >= scenario.maximum_frames;
}

int
automation_exit_status( void )
{
  return result.termination == AUTOMATION_TERMINATION_DEADLINE ? 2 :
         result.termination == AUTOMATION_TERMINATION_FAILURE ||
         result.termination == AUTOMATION_TERMINATION_RZX_DESYNC ||
         result.termination == AUTOMATION_TERMINATION_RZX_PARSE_ERROR ||
         result.termination == AUTOMATION_TERMINATION_RZX_SNAPSHOT_ERROR ||
         result.termination == AUTOMATION_TERMINATION_RZX_ABORTED ||
         result.termination == AUTOMATION_TERMINATION_ERROR ? 1 : 0;
}

unsigned long
automation_checksum( const unsigned char *data, size_t length )
{
  uLong value = crc32( 0, Z_NULL, 0 );
  while( length ) {
    uInt part = length > UINT_MAX ? UINT_MAX : (uInt)length;
    value = crc32( value, data, part );
    data += part;
    length -= part;
  }
  return value;
}

void
automation_record_tape( const utils_file *file )
{
  const char *name;

  if( libspectrum_file_class( file ) != LIBSPECTRUM_CLASS_TAPE )
    return;

  tape_size = libspectrum_file_size( file );
  tape_crc32 = automation_checksum( libspectrum_file_data( file ), tape_size );
  tape_recorded = 1;
  name = libspectrum_file_name( file );
  libspectrum_free( tape_name );
  tape_name = utils_safe_strdup( name );
}

static void
record_file( const utils_file *file, uLong *crc, size_t *size, char **name )
{
  const char *source = libspectrum_file_name( file );
  *size = libspectrum_file_size( file );
  *crc = automation_checksum( libspectrum_file_data( file ), *size );
  libspectrum_free( *name );
  *name = utils_safe_strdup( source );
}

void
automation_record_disk( const utils_file *file, const char *controller,
                        unsigned int drive )
{
  record_file( file, &disk_crc32, &disk_size, &disk_name );
  libspectrum_free( disk_controller );
  disk_controller = utils_safe_strdup( controller );
  disk_drive = drive;
  disk_recorded = 1;
}

void
automation_disk_motor_changed( int on, unsigned long frame_count )
{
  if( !armed || !scenario.until_disk_idle ) return;

  on = !!on;
  if( on ) disk_motor_observed = 1;
  if( disk_motor_on && !on ) disk_motor_off_frame = frame_count;
  disk_motor_on = on;
}

void
automation_record_rzx( const utils_file *file )
{
  record_file( file, &rzx_crc32, &rzx_size, &rzx_name );
  rzx_recorded = 1;
}

void
automation_record_external_snapshot( const utils_file *file )
{
  record_file( file, &snapshot_crc32, &snapshot_size, &snapshot_name );
  snapshot_recorded = 1;
}

void
automation_rzx_started( int embedded_snapshot )
{
  result.snapshot_source =
    embedded_snapshot ? AUTOMATION_RZX_SNAPSHOT_EMBEDDED :
    AUTOMATION_RZX_SNAPSHOT_EXTERNAL;
  result.rzx_cpu_mode_recorded = 1;
  result.rzx_cpu_cmos = settings_current.z80_is_cmos;
}

static void
rzx_finish( automation_termination_type termination )
{
  if( !automation_active() || !scenario.until_rzx_end )
    return;

  result.termination = termination;
  fuse_exiting = 1;
}

void
automation_rzx_completed( void )
{
  rzx_finish( AUTOMATION_TERMINATION_RZX_END );
}

void
automation_rzx_desynchronised( void )
{
  rzx_finish( AUTOMATION_TERMINATION_RZX_DESYNC );
}

void
automation_rzx_parse_error( void )
{
  rzx_finish( AUTOMATION_TERMINATION_RZX_PARSE_ERROR );
}

void
automation_rzx_snapshot_error( void )
{
  rzx_finish( AUTOMATION_TERMINATION_RZX_SNAPSHOT_ERROR );
}

void
automation_rzx_aborted( void )
{
  if( result.termination == AUTOMATION_TERMINATION_DEADLINE )
    rzx_finish( AUTOMATION_TERMINATION_RZX_ABORTED );
}

void
automation_capture_screen( const libspectrum_byte *pixels, size_t width,
                           size_t height )
{
  if( armed && scenario.capture_screen )
    automation_artifacts_capture_screen( pixels, width, height );
}

void
automation_capture_pcm( const libspectrum_signed_word *samples, int count,
                        int sample_rate, int channels )
{
  if( armed && scenario.capture_audio )
    automation_artifacts_capture_pcm( samples, count, sample_rate, channels );
}

void
automation_diagnostic( ui_error_level severity, const char *message )
{
  diagnostic *p;

  if( !automation_active() )
    return;

  p = libspectrum_renew( diagnostic, diagnostics, diagnostics_count + 1 );
  diagnostics = p;
  diagnostics[diagnostics_count].severity = severity;
  diagnostics[diagnostics_count].message = utils_safe_strdup( message );
  diagnostics_count++;
}

static const char *
termination_name( void )
{
  switch( result.termination ) {
  case AUTOMATION_TERMINATION_SUCCESS: return "success";
  case AUTOMATION_TERMINATION_FAILURE: return "failure";
  case AUTOMATION_TERMINATION_RZX_END: return "rzx-end";
  case AUTOMATION_TERMINATION_RZX_DESYNC: return "rzx-desynchronisation";
  case AUTOMATION_TERMINATION_RZX_PARSE_ERROR: return "rzx-parse-error";
  case AUTOMATION_TERMINATION_RZX_SNAPSHOT_ERROR: return "rzx-snapshot-error";
  case AUTOMATION_TERMINATION_RZX_ABORTED: return "rzx-aborted";
  case AUTOMATION_TERMINATION_DISK_IDLE: return "disk-idle";
  case AUTOMATION_TERMINATION_DEADLINE: return "deadline";
  case AUTOMATION_TERMINATION_ERROR: return "error";
  default: return "frames";
  }
}

static void
write_crc32( automation_json *json, const unsigned char *data, size_t length )
{
  char text[9];
  snprintf( text, sizeof( text ), "%08lx", automation_checksum( data, length ) );
  automation_json_string( json, "crc32", text );
}

static void
write_scenario( automation_json *json )
{
  automation_json_object_begin( json, "scenario" );
  automation_json_ulong( json, "maximum_frames", scenario.maximum_frames );
  if( scenario.sample_tstates )
    automation_json_ulong( json, "sample_tstates", scenario.sample_tstates );
  automation_json_boolean( json, "until_rzx_end", scenario.until_rzx_end );
  automation_json_boolean( json, "until_disk_idle",
                           scenario.until_disk_idle );
  if( scenario.until_disk_idle )
    automation_json_ulong( json, "disk_idle_frames",
                           scenario.disk_idle_frames );
  automation_json_string( json, "requested_machine",
                          settings_current.start_machine );
  if( scenario.success.present )
    automation_json_ulong( json, "success_pc", scenario.success.address );
  if( scenario.failure.present ) {
    automation_json_ulong( json, "failure_pc", scenario.failure.address );
    automation_json_ulong( json, "failure_pc_ignore", scenario.failure.ignore );
  }
  automation_json_object_begin( json, "capture" );
  automation_json_boolean( json, "screen", scenario.capture_screen );
  automation_json_boolean( json, "audio", scenario.capture_audio );
  automation_json_end( json );
  automation_json_end( json );
}

static void
write_execution( automation_json *json )
{
  automation_json_object_begin( json, "execution" );
  automation_json_ulong( json, "frames_completed", result.frames_completed );
  automation_json_string( json, "actual_machine", machine_current->id );
  automation_json_string( json, "cpu_mode",
                          ( result.rzx_cpu_mode_recorded ? result.rzx_cpu_cmos :
                            settings_current.z80_is_cmos ) ? "cmos" : "nmos" );
  automation_json_object_begin( json, "termination" );
  automation_json_string( json, "type", termination_name() );
  if( result.termination == AUTOMATION_TERMINATION_DISK_IDLE ) {
    automation_json_boolean( json, "motor_activity_observed",
                             disk_motor_observed );
    automation_json_ulong( json, "idle_frames", scenario.disk_idle_frames );
  }
  if( result.termination == AUTOMATION_TERMINATION_SUCCESS ||
      result.termination == AUTOMATION_TERMINATION_FAILURE )
    automation_json_ulong( json, "pc", result.pc );
  automation_json_end( json );
  if( scenario.until_disk_idle ) {
    automation_json_object_begin( json, "disk" );
    automation_json_boolean( json, "motor_activity_observed",
                             disk_motor_observed );
    automation_json_boolean( json, "motor_on_at_end", disk_motor_on );
    automation_json_ulong( json, "required_idle_frames",
                           scenario.disk_idle_frames );
    automation_json_end( json );
  }
  automation_json_end( json );
}

static void
write_file_identity( automation_json *json, const char *key, const char *name,
                     size_t size, uLong crc32 )
{
  char crc[9];
  snprintf( crc, sizeof( crc ), "%08lx", crc32 );
  automation_json_object_begin( json, key );

  if( name )
    automation_json_string( json, "path", name );

  automation_json_ulong( json, "size", size );
  automation_json_string( json, "crc32", crc );
  automation_json_end( json );
}

static void
write_identity( automation_json *json )
{
  automation_json_object_begin( json, "identity" );

  if( rzx_recorded )
    write_file_identity( json, "rzx", rzx_name, rzx_size, rzx_crc32 );

  if( result.snapshot_source != AUTOMATION_RZX_SNAPSHOT_NONE )
    automation_json_string( json, "rzx_snapshot_source",
                            result.snapshot_source ==
                            AUTOMATION_RZX_SNAPSHOT_EMBEDDED ? "embedded" :
                            "external" );

  if( snapshot_recorded )
    write_file_identity( json, "external_snapshot", snapshot_name,
                         snapshot_size, snapshot_crc32 );

  if( tape_recorded )
    write_file_identity( json, "tape", tape_name, tape_size, tape_crc32 );

  if( disk_recorded ) {
    write_file_identity( json, "disk", disk_name, disk_size, disk_crc32 );
    automation_json_object_begin( json, "disk_location" );
    automation_json_string( json, "controller", disk_controller );
    automation_json_ulong( json, "drive", disk_drive );
    automation_json_end( json );
  }

  automation_json_array_begin( json, "active_roms" );
  for( int page = 0; page < SPECTRUM_ROM_PAGES; page++ ) {
    memory_page *p = &memory_map_rom[page * MEMORY_PAGES_IN_16K];
    if( !p->page ) continue;
    automation_json_object_begin( json, NULL );
    automation_json_ulong( json, "page", page );
    automation_json_ulong( json, "size", 0x4000 );
    write_crc32( json, p->page, 0x4000 );
    automation_json_end( json );
  }

  automation_json_end( json );
  automation_json_end( json );
}

static void
write_diagnostics( automation_json *json )
{
  automation_json_array_begin( json, "diagnostics" );
  for( size_t i = 0; i < diagnostics_count; i++ ) {
    automation_json_object_begin( json, NULL );
    automation_json_string( json, "severity",
                            diagnostics[i].severity ==
                            UI_ERROR_ERROR ? "error" :
                            diagnostics[i].severity == UI_ERROR_WARNING ?
                            "warning" : "info" );
    automation_json_string( json, "message", diagnostics[i].message );
    automation_json_end( json );
  }
  automation_json_end( json );
}

int
automation_write_result( void )
{
  char *path;
  compat_fd file;
  automation_json json;

  int artifact_error = automation_artifacts_write( scenario.output_directory,
                                                    scenario.capture_screen,
                                                    scenario.capture_audio );
  if( artifact_error ) result.termination = AUTOMATION_TERMINATION_ERROR;

  path = libspectrum_new( char, strlen( scenario.output_directory ) + 13 );
  sprintf( path, "%s" FUSE_DIR_SEP_STR "result.json",
           scenario.output_directory );
  file = compat_file_open( path, 1 );
  libspectrum_free( path );

  if( file == COMPAT_FILE_OPEN_FAILED )
    return 1;

  automation_json_init( &json, file );
  automation_json_object_begin( &json, NULL );
  automation_json_ulong( &json, "schema", 1 );
  write_scenario( &json );
  write_execution( &json );
  automation_state_write_json( &json );
  write_identity( &json );
  automation_result_settings_write_json( &json, scenario.capture_audio,
                                          scenario.capture_screen );
  write_diagnostics( &json );
  automation_json_object_begin( &json, "artifacts" );
  automation_artifacts_write_json( &json );
  automation_json_end( &json );
  automation_json_end( &json );
  fputc( '\n', file );
  int error = automation_json_error( &json );

  if( compat_file_close( file ) )
    error = 1;

  return error || artifact_error;
}

void
automation_end( void )
{
  for( size_t i = 0; i < diagnostics_count; i++ )
    libspectrum_free( diagnostics[i].message );

  libspectrum_free( diagnostics );
  libspectrum_free( scenario.output_directory );
  libspectrum_free( tape_name );
  libspectrum_free( disk_name );
  libspectrum_free( disk_controller );
  libspectrum_free( rzx_name );
  libspectrum_free( snapshot_name );
  automation_artifacts_end();
}

#endif
