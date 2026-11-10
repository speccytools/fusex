/* utils_string.c: string helper functions
   Copyright (c) 1999-2018 Philip Kendall
   Copyright (c) 2015 Stuart Brady
   Copyright (c) 2015 Gergely Szasz
   Copyright (c) 2015-2021 Fredrick Meunier
   Copyright (c) 2016 BogDan Vatra
   Copyright (c) 2016-2017 Sergio Baldoví

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

#include <string.h>

#include "utils.h"

char*
utils_safe_strdup( const char *src )
{
  char *dest = NULL;
  if( src ) {
    size_t length = strlen( src ) + 1;
    dest = libspectrum_new( char, length );
    memcpy( dest, src, length );
  }
  return dest;
}
