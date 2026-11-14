/* fusex_debugger.h: Internal debugger access for libfusex embedders
   Copyright (c) 2026 The FuseX authors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef FUSEX_DEBUGGER_H
#define FUSEX_DEBUGGER_H

int fusex_debugger_command( const char *command, char *output,
                            int output_size );

#endif
