/* cocoascreenshot.m: Routines for handling external format screenshots
   Copyright (c) 2003,2005-2006 Fredrick Meunier
   Copyright (C) 1997-2003 Sam Lantinga

   $Id$

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

#include <config.h>

#include <Cocoa/Cocoa.h>

#include <libspectrum.h>

#include "cocoascreenshot.h"
#include "display.h"
#include "settings.h"
#include "ui/ui.h"

int
get_rgb24_data( libspectrum_byte *rgb24_data, size_t stride,
		size_t height, size_t width, size_t height_offset,
                size_t width_offset )
{
  size_t x, y;

  for( y = 0; y < height; y++ ) {
    for( x = 0; x < width; x++ ) {

      libspectrum_byte red, green, blue;

      display_colour_to_tv_rgb(
        display_getpixel( x + width_offset, y + height_offset ),
        &red, &green, &blue );

      rgb24_data[ y * stride + 3 * x     ] = red;
      rgb24_data[ y * stride + 3 * x + 1 ] = green;
      rgb24_data[ y * stride + 3 * x + 2 ] = blue;

    }
  }

  return 0;
}

int
screenshot_write( const char *filename )
{
  size_t icon_stride;
  size_t base_height, base_width;
  size_t base_height_offset, base_width_offset;
  int error = 0;
  size_t len = strlen(filename);
  const char* extension;
  NSBitmapImageRep *bits;
  NSAutoreleasePool *pool;
  NSBitmapImageFileType type = NSTIFFFileType;

  if( len < 4 ) {
    return 1;
  }

  extension = filename + len - 4;

  if( !strcmp( extension, ".png" ) ) {
    type = NSPNGFileType;
  } else if( !strcmp( extension, ".gif" ) ) {
    type = NSGIFFileType;
  } else if( !strcmp( extension, ".jpg" ) ) {
    type = NSJPEGFileType;
  } else if( !strcmp( extension, ".bmp" ) ) {
    type = NSBMPFileType;
  }

  if( machine_current->timex ) {
    base_height = 2 * DISPLAY_SCREEN_HEIGHT;
    base_width = DISPLAY_SCREEN_WIDTH; 
    base_height_offset = 0; 
    base_width_offset = 0; 
    icon_stride = DISPLAY_SCREEN_WIDTH * 3;
  } else {
    base_height = DISPLAY_SCREEN_HEIGHT;
    base_width = DISPLAY_SCREEN_WIDTH/2;
    base_height_offset = 0; 
    base_width_offset = 0 ; 
    icon_stride = DISPLAY_ASPECT_WIDTH * 3;
  }

  pool = [ [ NSAutoreleasePool alloc ] init ];

  bits = [ [ NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
            pixelsWide:base_width pixelsHigh:base_height
            bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO
            colorSpaceName:NSCalibratedRGBColorSpace
            bytesPerRow:icon_stride bitsPerPixel:24 ];

  /* Change from paletted data to RGB data */
  error = get_rgb24_data( [bits bitmapData], icon_stride, base_height,
                          base_width, base_height_offset, base_width_offset );
  if( error ) goto freePool;

  NSData *data = [ bits representationUsingType:type properties:[NSDictionary dictionary] ];
  [ data writeToFile:@(filename) atomically:NO ];

freePool:

  [ bits release ];

  [ pool release ];

  return error;
}
