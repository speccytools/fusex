/* main.m: The Free Unix Spectrum Emulator
   Copyright (c) 2006-2010 Fredrick Meunier

   $Id$

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

   Author contact information:

   E-mail: pak21-fuse@srcf.ucam.org

*/

#import <Cocoa/Cocoa.h>

#include <stdlib.h>

#include "fuse.h"
#include "main.h"
#include "settings.h"

int ac;
char **av;

int main(int argc, char *argv[])
{
  int retval, i;
  
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    
  settings_defaults( &settings_current );

  /* This is passed if we are launched by double-clicking */
  if ( argc >= 2 && strncmp( argv[1], "-psn", 4 ) == 0 ) {
    ac = 1;
  } else {
    ac = argc;
  }
  av = (char**) malloc( sizeof(*av) * (ac+1) );
  for( i = 0; i < ac; i++ )
    av[i] = argv[i];
  av[i] = NULL;

  if( settings_current.show_help ||
      settings_current.show_version ) return 0;

  /* fuse_init owns command-line file arguments; keep AppKit from also
     delivering them to application:openFile:. */
  [[NSUserDefaults standardUserDefaults]
    registerDefaults:@{ @"NSTreatUnknownArgumentsAsOpen": @"NO" }];

  retval = NSApplicationMain( argc, (const char**)argv );
  
  fuse_end();
  
  [pool release];

  return retval;
}
