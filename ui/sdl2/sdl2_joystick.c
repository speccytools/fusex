/* sdl2_joystick.c: routines for dealing with SDL 2 joysticks
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
 */

#include "config.h"

#ifndef USE_JOYSTICK
#include "../uijoystick.c"

#else

#include <SDL.h>

#include "input.h"
#include "settings.h"
#include "ui/ui.h"
#include "ui/uijoystick.h"
#include "sdl2_joystick.h"
#include "sdl2_joystick_internal.h"

static SDL_Joystick *joystick1;
static SDL_Joystick *joystick2;
static SDL_JoystickID joystick1_id = -1;
static SDL_JoystickID joystick2_id = -1;

int
ui_joystick_init( void )
{
  int error;
  int retval;

  error = SDL_InitSubSystem( SDL_INIT_JOYSTICK );
  if( error ) {
    ui_error( UI_ERROR_ERROR, "failed to initialise joystick subsystem" );
    return 0;
  }

  retval = SDL_NumJoysticks();

  if( retval >= 2 ) {
    retval = 2;

    joystick2 = SDL_JoystickOpen( 1 );
    if( !joystick2 ) {
      ui_error( UI_ERROR_ERROR, "failed to initialise joystick 2" );
      return 0;
    }

    if( SDL_JoystickNumAxes( joystick2 ) < 2 ||
        SDL_JoystickNumButtons( joystick2 ) < 1 ) {
      ui_error( UI_ERROR_ERROR, "sorry, joystick 2 is inadequate!" );
      return 0;
    }

    joystick2_id = SDL_JoystickInstanceID( joystick2 );
  }

  if( retval > 0 ) {
    joystick1 = SDL_JoystickOpen( 0 );
    if( !joystick1 ) {
      ui_error( UI_ERROR_ERROR, "failed to initialise joystick 1" );
      return 0;
    }

    if( SDL_JoystickNumAxes( joystick1 ) < 2 ||
        SDL_JoystickNumButtons( joystick1 ) < 1 ) {
      ui_error( UI_ERROR_ERROR, "sorry, joystick 1 is inadequate!" );
      return 0;
    }

    joystick1_id = SDL_JoystickInstanceID( joystick1 );
  }

  SDL_JoystickEventState( SDL_ENABLE );

  return retval;
}

void
ui_joystick_poll( void )
{
#ifndef UI_SDL2
  /* GTK and Xlib have no SDL UI event loop. */
  SDL_Event event;

  while( SDL_PollEvent( &event ) ) {
    switch( event.type ) {
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
    default:
      break;
    }
  }
#endif
}

static void
button_action( SDL_JoyButtonEvent *buttonevent, input_event_type type )
{
  input_event_t event;
  int which;

  which = sdl2joystick_lookup_id( buttonevent->which, joystick1_id,
                                  joystick2_id );
  if( which < 0 ) return;

  if( !sdl2joystick_button_event( which, buttonevent->button, type, &event ) )
    return;

  input_event( &event );
}

void
sdl2joystick_buttonpress( SDL_JoyButtonEvent *buttonevent )
{
  button_action( buttonevent, INPUT_EVENT_JOYSTICK_PRESS );
}

void
sdl2joystick_buttonrelease( SDL_JoyButtonEvent *buttonevent )
{
  button_action( buttonevent, INPUT_EVENT_JOYSTICK_RELEASE );
}

void
sdl2joystick_axismove( SDL_JoyAxisEvent *axisevent )
{
  int which;
  input_event_t event1, event2;

  which = sdl2joystick_lookup_id( axisevent->which, joystick1_id,
                                  joystick2_id );
  if( which < 0 ) return;

  if( axisevent->axis == 0 ) {
    sdl2joystick_axis_events( which, axisevent->value,
                              INPUT_JOYSTICK_LEFT, INPUT_JOYSTICK_RIGHT,
                              &event1, &event2 );
    input_event( &event1 );
    input_event( &event2 );
  } else if( axisevent->axis == 1 ) {
    sdl2joystick_axis_events( which, axisevent->value,
                              INPUT_JOYSTICK_UP, INPUT_JOYSTICK_DOWN,
                              &event1, &event2 );
    input_event( &event1 );
    input_event( &event2 );
  }
}

void
sdl2joystick_hatmove( SDL_JoyHatEvent *hatevent )
{
  int which;
  input_event_t event;

  which = sdl2joystick_lookup_id( hatevent->which, joystick1_id,
                                  joystick2_id );
  if( which < 0 ) return;

  if( hatevent->hat != 0 ) return;

  sdl2joystick_hat_event( which, hatevent->value, SDL_HAT_UP,
                          INPUT_JOYSTICK_UP, &event );
  input_event( &event );

  sdl2joystick_hat_event( which, hatevent->value, SDL_HAT_DOWN,
                          INPUT_JOYSTICK_DOWN, &event );
  input_event( &event );

  sdl2joystick_hat_event( which, hatevent->value, SDL_HAT_RIGHT,
                          INPUT_JOYSTICK_RIGHT, &event );
  input_event( &event );

  sdl2joystick_hat_event( which, hatevent->value, SDL_HAT_LEFT,
                          INPUT_JOYSTICK_LEFT, &event );
  input_event( &event );
}

void
ui_joystick_end( void )
{
  if( joystick1 || joystick2 ) {
    SDL_JoystickEventState( SDL_IGNORE );

    if( joystick1 ) SDL_JoystickClose( joystick1 );
    if( joystick2 ) SDL_JoystickClose( joystick2 );

    joystick1 = NULL;
    joystick2 = NULL;
    joystick1_id = -1;
    joystick2_id = -1;
  }

  if( SDL_WasInit( SDL_INIT_JOYSTICK ) ) SDL_QuitSubSystem( SDL_INIT_JOYSTICK );
}

#endif
