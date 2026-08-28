/* sdljoystick.c: routines for dealing with the SDL joystick
   Copyright (c) 2003-2026 Darren Salt, Fredrick Meunier, Philip Kendall
   Copyright (c) 2015 UB880D

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


#include <SDL.h>
#include <libspectrum.h>

#include "compat.h"
#include "input.h"
#include "sdljoystick.h"
#include "settings.h"
#include "ui/ui.h"
#include "ui/uijoystick.h"

static SDL_Joystick *joystick1 = NULL;
int joy1_num_axes = 0;
static SDL_Joystick *joystick2 = NULL;
int joy2_num_axes = 0;

static void do_axis( int which, Sint16 value, input_key negative,
		     input_key positive );
static void do_hat( int which, Uint8 value, Uint8 mask, input_key direction );

int
ui_joystick_init( void )
{
  int error, retval;

#ifdef UI_SDL
  error = SDL_InitSubSystem( SDL_INIT_JOYSTICK );
#else
  /* Other UIs could handle joysticks by the SDL library */
  error = SDL_Init( SDL_INIT_JOYSTICK );
#endif

  if ( error ) {
    ui_error( UI_ERROR_ERROR, "failed to initialise joystick subsystem" );
    return 0;
  }

  if( settings_current.joy1_number ) {
    if( ( joystick1 =
            SDL_JoystickOpen( settings_current.joy1_number - 1) ) == NULL ) {
      settings_current.joy1_number = 0;
      return 0;
    }

    joy1_num_axes = SDL_JoystickNumAxes(joystick1);
  }

  if( settings_current.joy2_number ) {
    if( ( joystick2 =
            SDL_JoystickOpen( settings_current.joy2_number - 1) ) == NULL ) {
      settings_current.joy2_number = 0;
      return 0;
    }

    joy2_num_axes = SDL_JoystickNumAxes(joystick2);
  }

  if( settings_current.joy1_number || settings_current.joy2_number ) {
    SDL_JoystickEventState( SDL_ENABLE );
  }

  return SDL_NumJoysticks();
}

void
ui_joystick_poll( void )
{
  /* No action needed in SDL UI; joysticks already handled by the SDL events
     system */

#ifndef UI_SDL
  SDL_Event event;

  while( SDL_PollEvent( &event ) ) {
    switch( event.type ) {
    case SDL_JOYBUTTONDOWN:
      sdljoystick_buttonpress( &(event.jbutton) );
      break;
    case SDL_JOYBUTTONUP:
      sdljoystick_buttonrelease( &(event.jbutton) );
      break;
    case SDL_JOYAXISMOTION:
      sdljoystick_axismove( &(event.jaxis) );
      break;
    case SDL_JOYHATMOTION:
      sdljoystick_hatmove( &(event.jhat) );
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
  int button;
  input_event_t event;
  
  button = buttonevent->button;
  if( button >= NUM_JOY_BUTTONS ) return;	/* We support 'only' NUM_JOY_BUTTONS (15 as defined in ui/uijoystick.h) fire buttons */
  
  if( ( settings_current.joy1_number - 1) == buttonevent->which ) {
    event.types.joystick.which = 0;
  } else if( ( settings_current.joy2_number - 1) == buttonevent->which ) {
    event.types.joystick.which = 1;
  } else {
    return;
  }

  event.type = type;
  event.types.joystick.button = INPUT_JOYSTICK_FIRE_1 + button;

  input_event( &event );
}

void
sdljoystick_buttonpress( SDL_JoyButtonEvent *buttonevent )
{
  button_action( buttonevent, INPUT_EVENT_JOYSTICK_PRESS );
}

void
sdljoystick_buttonrelease( SDL_JoyButtonEvent *buttonevent )
{
  button_action( buttonevent, INPUT_EVENT_JOYSTICK_RELEASE );
}

void
sdljoystick_axismove( SDL_JoyAxisEvent *axisevent )
{
  int which, xaxis, yaxis;

  if( ( settings_current.joy1_number - 1 ) == axisevent->which ) {
    which = 0;
    if( axisevent->axis == settings_current.joy1_xaxis ) {
      xaxis = 0;
    } else if( axisevent->axis == settings_current.joy1_yaxis ) {
      yaxis = 1;
    } else {
      return;
    }
  } else if( ( settings_current.joy2_number - 1 ) == axisevent->which ) {
    which = 1;
    if( axisevent->axis == settings_current.joy2_xaxis ) {
      xaxis = 0;
    } else if( axisevent->axis == settings_current.joy2_yaxis ) {
      yaxis = 1;
    } else {
      return;
    }
  } else {
    return;
  }

  if( axisevent->axis == 0 ) {
    do_axis( which, axisevent->value,
	     INPUT_JOYSTICK_LEFT, INPUT_JOYSTICK_RIGHT );
  } else if( axisevent->axis == 1 ) {
    do_axis( which, axisevent->value,
	     INPUT_JOYSTICK_UP,   INPUT_JOYSTICK_DOWN  );
  }
}

static void
do_axis( int which, Sint16 value, input_key negative, input_key positive )
{
  input_event_t event1, event2;

  event1.types.joystick.which = event2.types.joystick.which = which;

  event1.types.joystick.button = negative;
  event2.types.joystick.button = positive;

  if( value > 16384 ) {
    event1.type = INPUT_EVENT_JOYSTICK_RELEASE;
    event2.type = INPUT_EVENT_JOYSTICK_PRESS;
  } else if( value < -16384 ) {
    event1.type = INPUT_EVENT_JOYSTICK_PRESS;
    event2.type = INPUT_EVENT_JOYSTICK_RELEASE;
  } else {
    event1.type = INPUT_EVENT_JOYSTICK_RELEASE;
    event2.type = INPUT_EVENT_JOYSTICK_RELEASE;
  }

  input_event( &event1 );
  input_event( &event2 );
}

void
sdljoystick_hatmove( SDL_JoyHatEvent *hatevent )
{
  int which = hatevent->which;
  Uint8 value = hatevent->value;

  if( hatevent->hat != 0 ) {
    return;
  }

  do_hat( which, value, SDL_HAT_UP, INPUT_JOYSTICK_UP );
  do_hat( which, value, SDL_HAT_DOWN, INPUT_JOYSTICK_DOWN );
  do_hat( which, value, SDL_HAT_RIGHT, INPUT_JOYSTICK_RIGHT );
  do_hat( which, value, SDL_HAT_LEFT, INPUT_JOYSTICK_LEFT );
}

static void
do_hat( int which, Uint8 value, Uint8 mask, input_key direction )
{
  input_event_t event;

  event.types.joystick.which = which;
  event.types.joystick.button = direction;

  event.type = INPUT_EVENT_JOYSTICK_RELEASE;
  input_event( &event );

  if( value & mask ) {
    event.type = INPUT_EVENT_JOYSTICK_PRESS;
    input_event( &event );
  }
}

void
ui_joystick_end( void )
{
  if( joystick1 != NULL || joystick2 != NULL ) {

    SDL_JoystickEventState( SDL_IGNORE );
    if( joystick1 != NULL ) SDL_JoystickClose( joystick1 );
    if( joystick2 != NULL ) SDL_JoystickClose( joystick2 );
    joystick1 = NULL;
    joystick2 = NULL;

  }

#ifdef UI_SDL
  SDL_QuitSubSystem( SDL_INIT_JOYSTICK );
#else
  SDL_Quit();
#endif
}
