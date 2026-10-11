/* null_ui.c: Routines for dealing with the null user interface
   Copyright (c) 2017 Philip Kendall

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

#include "config.h"

#ifdef ENABLE_AUTOMATION
#include "automation/automation.h"
#endif
#include <stdint.h>
#include <stdlib.h>

#include "display.h"
#include "keyboard.h"
#include "machine.h"
#include "spectrum.h"
#include "timer/timer.h"
#include "ui/scaler/scaler.h"
#include "ui/ui.h"
#include "ui/ui_internals.h"

#include "fusex_display.h"

#include "../uijoystick.c"

keysyms_map_t keysyms_map[] = {
  { 0xff, 0 } /* End marker */
};

static uint8_t *null_display_pixels;
static fusex_display_info_t null_display_info;
static fusex_display_frame_t null_display_frame;
static uint64_t null_display_generation;

static void
null_ui_schedule_timer( libspectrum_dword last_tstates )
{
  event_add( last_tstates + machine_current->timings.tstates_per_frame,
             timer_event );
}

static void
null_display_pixel( int x, int y, int colour )
{
  if( x >= 0 && x < null_display_info.width &&
      y >= 0 && y < null_display_info.height )
    null_display_pixels[ y * null_display_info.width + x ] = (uint8_t)colour;
}

static void
null_display_plot8( int x, int y, libspectrum_byte data,
                    libspectrum_word ink, libspectrum_word paper )
{
  int i;
  int pixel_x = machine_current->timex ? x << 4 : x << 3;
  int pixel_y = machine_current->timex ? y << 1 : y;
  int repeat_x = machine_current->timex ? 2 : 1;
  int repeat_y = machine_current->timex ? 2 : 1;

  for( i = 0; i < 8; i++ ) {
    /* The buffer holds one of the standard colours per pixel */
    int colour = display_nearest_standard_colour(
                   ( data & ( 0x80 >> i ) ) ? ink : paper );
    int rx, ry;
    for( ry = 0; ry < repeat_y; ry++ )
      for( rx = 0; rx < repeat_x; rx++ )
        null_display_pixel( pixel_x + repeat_x * i + rx,
                             pixel_y + ry, colour );
  }
}

static void
null_display_plot16( int x, int y, libspectrum_word data,
                     libspectrum_byte ink, libspectrum_byte paper )
{
  int row, i;
  int pixel_x = x << 4;
  int pixel_y = y << 1;

  for( row = 0; row < 2; row++ )
    for( i = 0; i < 16; i++ )
      null_display_pixel( pixel_x + i, pixel_y + row,
                          ( data & ( 0x8000 >> i ) ) ? ink : paper );
}

scaler_type
menu_get_scaler( scaler_available_fn selector )
{
  /* No scaler selected */
  return SCALER_NUM;
}

int
menu_select_roms_with_title( const char *title, size_t start, size_t count,
                             int is_peripheral )
{
  /* No error */
  return 0;
}

void
ui_breakpoints_updated( void )
{
  /* Do nothing */
}

ui_confirm_save_t
ui_confirm_save_specific( const char *message )
{
  return UI_CONFIRM_SAVE_DONTSAVE;
}

ui_confirm_joystick_t
ui_confirm_joystick( libspectrum_joystick libspectrum_type, int inputs )
{
  return UI_CONFIRM_JOYSTICK_NONE;
}

int
ui_debugger_activate( void )
{
  /* No error */
  return 0;
}

int
ui_debugger_deactivate( int interruptable )
{
  /* No error */
  return 0;
}

int
ui_debugger_disassemble( libspectrum_word addr )
{
  /* No error */
  return 0;
}

int
ui_debugger_update( void )
{
  /* No error */
  return 0;
}

int
ui_end( void )
{
  /* No error */
  return 0;
}

int
ui_error_specific( ui_error_level severity, const char *message )
{
#ifdef ENABLE_AUTOMATION
  automation_diagnostic( severity, message );
#endif
  return 0;
}

int
ui_event( void )
{
  /* No error */
  return 0;
}

char *
ui_get_open_filename( const char *title )
{
  /* No filename */
  return NULL;
}

int
ui_get_rollback_point( GSList *points )
{
  /* No rollback point */
  return -1;
}

char *
ui_get_save_filename( const char *title )
{
  /* No filename */
  return NULL;
}

int
ui_init( int *argc, char ***argv )
{
  timer_set_pacer( null_ui_schedule_timer );

  /* No error */
  return 0;
}

int
ui_menu_item_set_active( const char *path, int active )
{
  /* No error */
  return 0;
}

int
ui_mouse_grab( int startup )
{
  /* Successful grab */
  return 1;
}

int
ui_mouse_release( int suspend )
{
  /* No error */
  return 0;
}

void
ui_pokemem_selector( const char *filename )
{
  /* Do nothing */
}

int
ui_query_message( const char *message )
{
  /* Query confirmed */
  return 1;
}

int
ui_statusbar_update( ui_statusbar_item item, ui_statusbar_state state )
{
#ifdef ENABLE_AUTOMATION
  if( automation_active() && item == UI_STATUSBAR_ITEM_DISK &&
      ( state == UI_STATUSBAR_STATE_ACTIVE ||
        state == UI_STATUSBAR_STATE_INACTIVE ) )
    automation_disk_motor_changed( state == UI_STATUSBAR_STATE_ACTIVE,
                                   spectrum_get_frame_count() );
#endif
  /* No error */
  return 0;
}

int
ui_statusbar_update_speed( float speed )
{
  /* No error */
  return 0;
}

int
ui_tape_browser_update( ui_tape_browser_update_type change,
                        libspectrum_tape_block *block )
{
  /* No error */
  return 0;
}

int
ui_widgets_reset( void )
{
  /* No error */
  return 0;
}

void
uidisplay_area( int x, int y, int w, int h )
{
  /* Do nothing */
}

int
uidisplay_end( void )
{
  free( null_display_pixels );
  null_display_pixels = NULL;
  null_display_info = (fusex_display_info_t){ 0 };
  null_display_frame = (fusex_display_frame_t){ 0 };
  /* No error */
  return 0;
}

void
uidisplay_frame_end( void )
{
#ifdef ENABLE_AUTOMATION
  if( automation_capture_screen_enabled() )
    automation_capture_screen( null_display_pixels,
                               null_display_info.width,
                               null_display_info.height );
#endif
}

int
uidisplay_hotswap_gfx_mode( void )
{
  /* No error */
  return 0;
}

int
uidisplay_init( int width, int height )
{
#ifdef ENABLE_AUTOMATION
  if( automation_capture_screen_enabled() ) {
    for( scaler_type scaler = 0; scaler < SCALER_NUM; scaler++ )
      scaler_register( scaler );
  }
#endif

  null_display_pixels = calloc( (size_t)width * (size_t)height,
                                sizeof( *null_display_pixels ) );
  if( !null_display_pixels ) return 1;

  null_display_info = (fusex_display_info_t){
    .width = width,
    .height = height,
    .border_width = width == DISPLAY_ASPECT_WIDTH
      ? DISPLAY_BORDER_ASPECT_WIDTH : DISPLAY_BORDER_WIDTH,
    .border_height = height == DISPLAY_SCREEN_HEIGHT
      ? DISPLAY_BORDER_HEIGHT : DISPLAY_BORDER_HEIGHT * 2,
  };
  null_display_generation++;
  null_display_frame = (fusex_display_frame_t){
    .pixels = null_display_pixels,
    .generation = null_display_generation,
  };
  return 0;
}

void
uidisplay_plot16( int x, int y, libspectrum_word data,
                  libspectrum_byte ink, libspectrum_byte paper )
{
  null_display_plot16( x, y, data, ink, paper );
}

void
uidisplay_plot8( int x, int y, libspectrum_byte data,
                 libspectrum_word ink, libspectrum_word paper )
{
  null_display_plot8( x, y, data, ink, paper );
}

void
uidisplay_putpixel( int x, int y, int colour )
{
  int pixel_x = machine_current->timex ? x << 1 : x;
  int pixel_y = machine_current->timex ? y << 1 : y;
  int repeat_x = machine_current->timex ? 2 : 1;
  int repeat_y = machine_current->timex ? 2 : 1;
  int rx, ry;

  for( ry = 0; ry < repeat_y; ry++ )
    for( rx = 0; rx < repeat_x; rx++ )
      null_display_pixel( pixel_x + rx, pixel_y + ry, colour );
}

const fusex_display_info_t *
null_ui_display_info( void )
{
  return &null_display_info;
}

const fusex_display_frame_t *
null_ui_display_frame( void )
{
  return &null_display_frame;
}

int
ui_menu_activate( ui_menu_item item, int active )
{
  return 0;
}

int
ui_tape_write( void )
{
  return 0;
}

int
ui_disk_write( int which, int saveas )
{
  return 0;
}

int
ui_mdr_write( int which, int saveas )
{
  return 0;
}
