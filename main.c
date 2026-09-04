/* main.c: Fuse executable entry point
   Copyright (c) 1999-2026 Philip Kendall and others

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

#include <stdio.h>

#ifdef WIN32
#include <windows.h>
#endif

/* We need to include SDL.h on Mac OS X and Windows to do some magic
   bootstrapping by redefining main. As we now allow SDL joystick code to be
   used in the GTK and Xlib UIs we need to also do the magic when that code is
   in use, feel free to look away for the next line */
#if defined UI_SDL || defined UI_SDL2 || (defined USE_JOYSTICK && (defined UI_X || defined UI_GTK) )
#include <SDL.h>		/* Needed on MacOS X and Windows */
#endif /* #if defined UI_SDL || defined UI_SDL2 || (defined USE_JOYSTICK && (defined UI_X || defined UI_GTK) ) */

#include "debugger/debugger.h"
#include "fuse.h"
#include "settings.h"
#include "spectrum.h"
#include "unittests/unittests.h"

#ifdef UI_WIN32
/* The Win32 UI supplies WinMain(), which calls this. */
int
fuse_main( int argc, char **argv )
#elif defined UI_COCOA
/* The Cocoa app supplies its own main() in main.m. */
int
old_main( int argc, char **argv )
#else
int
main( int argc, char **argv )
#endif
{
  int r = 0;

#ifdef WIN32
  SetErrorMode( SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX );
#endif

#ifdef GEKKO
  fatInitDefault();
#endif

  if( fuse_init( argc, argv ) ) {
    fprintf( stderr, "%s: error initialising -- giving up!\n", fuse_progname );
    return 1;
  }

  if( settings_current.show_help ||
      settings_current.show_version ) return 0;

  if( settings_current.unittests ) {
    r = unittests_run();
  } else {
    while( !fuse_exiting ) {
      spectrum_do_frame();
    }
    r = debugger_get_exit_code();
  }

  fuse_end();

  return r;
}
