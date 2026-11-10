/* utf8.h: UTF-8 helpers for widget text (not Spectrum screen bytes)
   Copyright (c) 2005 Darren Salt

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version. */

#ifndef FUSE_WIDGET_UTF8_H
#define FUSE_WIDGET_UTF8_H

#include <stddef.h>

/* remaining is a byte limit. Return zero at the end or an incomplete
   byte-limited substring, -1 for invalid input (consuming one byte),
   or a Unicode scalar value. No locale-dependent interpretation. */
int widget_utf8_next( const char **text, size_t *remaining );

/* Return the start of the last character in a byte-limited string.
   Invalid trailing bytes are removed one at a time. */
size_t widget_utf8_previous( const char *text, size_t length );

#endif
