/* sdl2_ui.c: Routines for dealing with the SDL 2 user interface
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "config.h"

#include <SDL.h>

#include "display.h"
#include "fuse.h"
#include "menu.h"
#include "settings.h"
#include "ui/display_timing.h"
#include "ui/ui.h"
#include "ui/uidisplay.h"
#include "sdl2_display.h"
#include "sdl2_joystick.h"
#include "sdl2_keyboard.h"
#include "sdl2_mouse_internal.h"
#include "sdl2_ui.h"

static SDL_Cursor *sdl2ui_blank_cursor;
static SDL_Cursor *sdl2ui_default_cursor;

static int
sdl2ui_init_blank_cursor( void )
{
  static const Uint8 data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  static const Uint8 mask[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

  if( sdl2ui_blank_cursor ) return 0;

  sdl2ui_blank_cursor = SDL_CreateCursor( data, mask, 8, 8, 0, 0 );
  if( !sdl2ui_blank_cursor ) {
    ui_error( UI_ERROR_WARNING, "Couldn't create blank mouse cursor: %s",
              SDL_GetError() );
    return 1;
  }

  return 0;
}

void
sdl2ui_set_cursor_visibility( int show_cursor )
{
  if( show_cursor ) {
    SDL_SetCursor( sdl2ui_default_cursor ? sdl2ui_default_cursor :
                   SDL_GetDefaultCursor() );
    SDL_ShowCursor( SDL_ENABLE );
  } else {
    SDL_SetCursor( sdl2ui_default_cursor ? sdl2ui_default_cursor :
                   SDL_GetDefaultCursor() );
    if( !sdl2ui_init_blank_cursor() ) SDL_SetCursor( sdl2ui_blank_cursor );
    SDL_ShowCursor( SDL_ENABLE );
  }
}

static void
sdl2ui_handle_window_event( const SDL_WindowEvent *event )
{
  switch( event->event ) {
  case SDL_WINDOWEVENT_EXPOSED:
  case SDL_WINDOWEVENT_SHOWN:
  case SDL_WINDOWEVENT_SIZE_CHANGED:
    display_refresh_all();
    break;
  case SDL_WINDOWEVENT_FOCUS_GAINED:
    ui_mouse_resume();
    break;
  case SDL_WINDOWEVENT_FOCUS_LOST:
    sdl2keyboard_release_all();
    ui_mouse_suspend();
    break;
  default:
    break;
  }
}

int
ui_init( int *argc, char ***argv )
{
  int error;

  if( ui_widget_init() ) return 1;

  error = SDL_Init( SDL_INIT_VIDEO );
  if( error ) {
    ui_widget_end();
    return error;
  }

  sdl2ui_default_cursor = SDL_GetDefaultCursor();

  sdl2keyboard_init();
  ui_mouse_present = 1;

  return 0;
}

int
ui_event( void )
{
  SDL_Event event;

  display_timing_input_begin();
  while( SDL_PollEvent( &event ) ) {
    switch( event.type ) {
    case SDL_KEYDOWN:
      if( !event.key.repeat || ui_widget_level >= 0 )
        sdl2keyboard_keypress( &event.key );
      break;
    case SDL_KEYUP:
      sdl2keyboard_keyrelease( &event.key );
      break;

    case SDL_MOUSEBUTTONDOWN:
      ui_mouse_button( event.button.button, 1 );
      break;
    case SDL_MOUSEBUTTONUP:
      ui_mouse_button( event.button.button, 0 );
      break;
    case SDL_MOUSEMOTION:
      if( ui_mouse_grabbed ) {
        ui_mouse_motion( event.motion.xrel, event.motion.yrel );
      }
      break;

#ifdef USE_JOYSTICK
    case SDL_JOYBUTTONDOWN:
      sdl2joystick_buttonpress( &event.jbutton );
      break;
    case SDL_JOYBUTTONUP:
      sdl2joystick_buttonrelease( &event.jbutton );
      break;
    case SDL_JOYAXISMOTION:
      sdl2joystick_axismove( &event.jaxis );
      break;
    case SDL_JOYHATMOTION:
      sdl2joystick_hatmove( &event.jhat );
      break;
#endif

    case SDL_QUIT:
      fuse_emulation_pause();
      menu_file_exit( 0 );
      fuse_emulation_unpause();
      break;

    case SDL_WINDOWEVENT:
      sdl2ui_handle_window_event( &event.window );
      break;

    default:
      break;
    }
  }

  display_timing_input_end();

  return 0;
}

int
ui_end( void )
{
  int error;

  error = uidisplay_end();
  if( error ) return error;

  if( sdl2ui_blank_cursor ) {
    SDL_FreeCursor( sdl2ui_blank_cursor );
    sdl2ui_blank_cursor = NULL;
  }

  sdl2ui_default_cursor = NULL;

  sdl2keyboard_end();
  SDL_Quit();
  ui_widget_end();

  return 0;
}

int
ui_statusbar_update_speed( float speed )
{
  char buffer[ 32 ];

  snprintf( buffer, sizeof( buffer ), "Fuse - %3.0f%%", speed );
  sdl2display_set_title( buffer );

  return 0;
}

int
ui_mouse_grab( int startup )
{
  sdl2_mouse_grab_plan plan;
  SDL_Window *window;

  window = sdl2display_get_window();
  sdl2mouse_get_grab_plan( startup, settings_current.full_screen,
                           window != NULL, &plan );

  if( !plan.should_grab ) return 0;

  if( SDL_SetRelativeMouseMode( plan.enable_relative_mode ? SDL_TRUE :
                                SDL_FALSE ) ) {
    ui_error( UI_ERROR_WARNING, "Mouse grab failed" );
    return 0;
  }

  SDL_GetRelativeMouseState( NULL, NULL );

  if( plan.enable_window_grab >= 0 )
    SDL_SetWindowGrab( window, plan.enable_window_grab ? SDL_TRUE : SDL_FALSE );
  sdl2ui_set_cursor_visibility( plan.show_cursor );
  SDL_PumpEvents();

  return 1;
}

int
ui_mouse_release( int suspend GCC_UNUSED )
{
  sdl2_mouse_grab_plan plan;
  SDL_Window *window;

  window = sdl2display_get_window();
  sdl2mouse_get_release_plan( window != NULL, &plan );

  SDL_SetRelativeMouseMode( plan.enable_relative_mode ? SDL_TRUE : SDL_FALSE );
  if( plan.enable_window_grab >= 0 && window )
    SDL_SetWindowGrab( window, plan.enable_window_grab ? SDL_TRUE : SDL_FALSE );
  sdl2ui_set_cursor_visibility( plan.show_cursor );
  SDL_PumpEvents();

  return 0;
}
