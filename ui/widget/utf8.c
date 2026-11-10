/* utf8.c: UTF-8 helpers for widget text
   Copyright (c) 2005 Darren Salt

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version. */

#include "config.h"

#include "utf8.h"

int
widget_utf8_next( const char **text, size_t *remaining )
{
  const unsigned char *s = (const unsigned char *)*text;
  unsigned int value, minimum;
  size_t bytes, i;

  if( !*remaining || !*s ) return 0;
  value = *s;
  minimum = 0;
  if( value < 0x80 ) {
    bytes = 1;
  } else if( value >= 0xc2 && value <= 0xdf ) {
    bytes = 2; minimum = 0x80; value &= 0x1f;
  } else if( value >= 0xe0 && value <= 0xef ) {
    bytes = 3; minimum = 0x800; value &= 0x0f;
  } else if( value >= 0xf0 && value <= 0xf4 ) {
    bytes = 4; minimum = 0x10000; value &= 0x07;
  } else {
    goto invalid;
  }
  if( bytes > *remaining ) {
    *remaining = 0;
    return 0;
  }
  for( i = 1; i < bytes; i++ ) {
    if( ( s[i] & 0xc0 ) != 0x80 ) goto invalid;
    value = ( value << 6 ) | ( s[i] & 0x3f );
  }
  if( value < minimum || value > 0x10ffff ||
      ( value >= 0xd800 && value <= 0xdfff ) ) goto invalid;

  *text += bytes;
  *remaining -= bytes;
  return value;

invalid:
  (*text)++;
  (*remaining)--;
  return -1;
}

size_t
widget_utf8_previous( const char *text, size_t length )
{
  size_t start, remaining;
  const char *p;

  if( !length ) return 0;
  start = length - 1;
  while( start && ( (unsigned char)text[start] & 0xc0 ) == 0x80 ) start--;
  p = text + start;
  remaining = length - start;
  if( widget_utf8_next( &p, &remaining ) > 0 && !remaining ) return start;
  return length - 1;
}
