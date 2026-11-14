/* fusex_registers.h: CPU register access for libfusex embedders
   Copyright (c) 2026 The FuseX authors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#ifndef FUSEX_REGISTERS_H
#define FUSEX_REGISTERS_H

#include <libspectrum.h>

#include "z80/z80.h"

int fusex_register_get( enum z80_register reg, libspectrum_word *value );

#endif
