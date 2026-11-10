/* automation.h: development-only one-shot automation interface
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
#ifndef FUSE_AUTOMATION_H
#define FUSE_AUTOMATION_H
#include <stddef.h>
#include "libspectrum.h"
#include "ui/ui.h"
#include "utils.h"

/* CRC32 checksum of a byte buffer, shared by automation.c and artifacts.c */
unsigned long automation_checksum( const unsigned char *data, size_t length );

typedef struct automation_condition { int present; libspectrum_word address;
                                      unsigned long ignore, hits;
} automation_condition;

typedef struct automation_scenario { char *output_directory;
                                     unsigned long maximum_frames,
                                                   disk_idle_frames,
                                                   sample_tstates;
                                     int until_rzx_end, until_disk_idle,
                                         capture_screen, capture_audio;
                                     automation_condition success, failure;
} automation_scenario;

typedef enum automation_termination_type { AUTOMATION_TERMINATION_FRAMES,
                                           AUTOMATION_TERMINATION_SUCCESS,
                                           AUTOMATION_TERMINATION_FAILURE,
                                           AUTOMATION_TERMINATION_RZX_END,
                                           AUTOMATION_TERMINATION_RZX_DESYNC,
                                           AUTOMATION_TERMINATION_RZX_PARSE_ERROR,
                                           AUTOMATION_TERMINATION_RZX_SNAPSHOT_ERROR,
                                           AUTOMATION_TERMINATION_RZX_ABORTED,
                                           AUTOMATION_TERMINATION_DISK_IDLE,
                                           AUTOMATION_TERMINATION_DEADLINE,
                                           AUTOMATION_TERMINATION_ERROR }
automation_termination_type;

typedef enum automation_rzx_snapshot_source {
  AUTOMATION_RZX_SNAPSHOT_NONE, AUTOMATION_RZX_SNAPSHOT_EMBEDDED,
  AUTOMATION_RZX_SNAPSHOT_EXTERNAL
} automation_rzx_snapshot_source;

typedef struct automation_result { unsigned long frames_completed;
                                   automation_termination_type termination;
                                   libspectrum_word pc;
                                   int rzx_cpu_mode_recorded, rzx_cpu_cmos;
                                   automation_rzx_snapshot_source
                                     snapshot_source;} automation_result;

int automation_options_present( int argc, char **argv );
int automation_set_output_directory( const char *directory );
int automation_set_frame_limit( const char *frames );
void automation_set_until_rzx_end( void );
void automation_set_until_disk_idle( void );
int automation_set_disk_idle_frames( const char *frames );
int automation_set_sample_tstates( const char *tstates );
void automation_set_capture_screen( void );
void automation_set_capture_audio( void );
int automation_capture_screen_enabled( void );
int automation_capture_audio_enabled( void );
int automation_set_success_pc( const char *text );
int automation_set_failure_pc( const char *text );
int automation_set_failure_pc_ignore( const char *text );

int automation_validate_scenario( void );
int automation_active( void );
int automation_pc_check_active( void );
void automation_arm( unsigned long frame_count );
int automation_check_pc( libspectrum_word pc );
int automation_frame_limit_reached( unsigned long frame_count );
int automation_exit_status( void );

void automation_record_tape( const utils_file *file );
void automation_record_disk( const utils_file *file, const char *controller,
                             unsigned int drive );
void automation_record_rzx( const utils_file *file );
void automation_record_external_snapshot( const utils_file *file );
void automation_disk_motor_changed( int on, unsigned long frame_count );

void automation_rzx_started( int embedded_snapshot );
void automation_rzx_completed( void );
void automation_rzx_desynchronised( void );
void automation_rzx_parse_error( void );
void automation_rzx_snapshot_error( void );
void automation_rzx_aborted( void );

void automation_diagnostic( ui_error_level severity, const char *message );
void automation_capture_screen( const libspectrum_byte *pixels,
                                size_t width, size_t height );
void automation_capture_pcm( const libspectrum_signed_word *samples, int count,
                             int sample_rate, int channels );
int automation_write_result( void );

void automation_end( void );

#endif
