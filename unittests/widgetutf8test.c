/* widgetutf8test.c: Bounded widget UTF-8 decoding regression tests
   Copyright (c) 2026 Fredrick Meunier

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version. */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/widget/utf8.h"

static void
check_character( const char *text, int expected )
{
  const char *p = text;
  size_t remaining = strlen( text );

  assert( widget_utf8_next( &p, &remaining ) == expected );
  assert( remaining == 0 );
  assert( widget_utf8_next( &p, &remaining ) == 0 );
  assert( widget_utf8_previous( text, strlen( text ) ) == 0 );
}

int
main( void )
{
  const char *invalid[] = {
    "\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\x80",
    "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80",
    "\xf5\x80\x80\x80", "\xf8\x80\x80\x80\x80",
    "\xff", "\xc2", "\xe2\x82", "\xf0\x9f\x98"
  };
  const char sequence[] = "A\xc2\xa3\xe2\x82\xac\xf0\x9f\x98\x80";
  const int expected[] = { 'A', 0xa3, 0x20ac, 0x1f600 };
  const size_t ends[] = { 1, 3, 6, 10 };
  const char *p;
  size_t i, remaining;

  check_character( "A", 'A' );
  check_character( "\x01", 1 ); /* Colour controls stay byte-compatible. */
  check_character( "\x19", 25 ); /* Shadow control. */
  check_character( "\xc2\xa3", 0xa3 );
  check_character( "\xc4\x80", 0x100 );
  check_character( "\xe2\x80\x9d", 0x201d );
  check_character( "\xf0\x9f\x98\x80", 0x1f600 );
  check_character( "\xf4\x8f\xbf\xbf", 0x10ffff );

  for( i = 0; i < sizeof( invalid ) / sizeof( invalid[0] ); i++ ) {
    p = invalid[i];
    remaining = (size_t)-1;
    assert( widget_utf8_next( &p, &remaining ) == -1 );
    assert( p == invalid[i] + 1 );
  }

  /* Every possible byte limit, including one inside a multi-byte sequence. */
  for( i = 0; i <= strlen( sequence ); i++ ) {
    size_t j;
    p = sequence;
    remaining = i;
    for( j = 0; j < 4 && ends[j] <= i; j++ )
      assert( widget_utf8_next( &p, &remaining ) == expected[j] );
    assert( widget_utf8_next( &p, &remaining ) == 0 );
    assert( p == sequence + ( j ? ends[j - 1] : 0 ) );
  }

  assert( widget_utf8_previous( sequence, 10 ) == 6 );
  assert( widget_utf8_previous( sequence, 6 ) == 3 );
  assert( widget_utf8_previous( sequence, 3 ) == 1 );
  assert( widget_utf8_previous( "\x80\x80", 2 ) == 1 );
  assert( widget_utf8_previous( "", 0 ) == 0 );
  p = "";
  remaining = (size_t)-1;
  assert( widget_utf8_next( &p, &remaining ) == 0 );
  p = "\xc2";
  remaining = (size_t)-1;
  assert( widget_utf8_next( &p, &remaining ) == -1 );

  puts( "Widget UTF-8: scalars, controls, malformed input, byte limits and backspace passed" );
  return 0;
}
