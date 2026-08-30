/* disassemble.c: Fuse's disassembler
   Copyright (c) 2002-2018 Darren Salt, Philip Kendall
   Copyright (c) 2016 BogDan Vatra
   Copyright (c) 2026 Fredrick Meunier

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

   Darren: linux@youmustbejoking.demon.co.uk

   Philip: philip-fuse@shadowmagic.org.uk

*/

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "libspectrum.h"

#include "debugger.h"
#include "fuse.h"
#include "memory_pages.h"
#include "ui/ui.h"

/* Size of temporary string buffers used throughout the disassembler */
#define DISASM_BUF_LEN 40

/* Used to flag whether we're after a DD or FD prefix */
enum hl_type { USE_HL, USE_IX, USE_IY };

static void disassemble_main( libspectrum_word address, char *buffer,
			      size_t buflen, size_t *length,
			      enum hl_type use_hl );
static void disassemble_00xxxxxx( libspectrum_word address, char *buffer,
				  size_t buflen, size_t *length,
				  enum hl_type use_hl );
static void disassemble_00xxx010( libspectrum_word address, char *buffer,
				  size_t buflen, size_t *length,
				  enum hl_type use_hl );
static void disassemble_00xxx110( libspectrum_word address, char *buffer,
				  size_t buflen, size_t *length,
				  enum hl_type use_hl );
static void disassemble_11xxxxxx( libspectrum_word address, char *buffer,
				  size_t buflen, size_t *length,
				  enum hl_type use_hl );
static void disassemble_11xxx001( libspectrum_byte b, char *buffer,
				  size_t buflen, size_t *length,
				  enum hl_type use_hl );
static void disassemble_11xxx011( libspectrum_word address, char *buffer,
				  size_t buflen, size_t *length,
				  enum hl_type use_hl );
static void disassemble_11xxx101( libspectrum_word address, char *buffer,
				  size_t buflen, size_t *length,
				  enum hl_type use_hl );
static void disassemble_cb( libspectrum_word address, char *buffer,
			    size_t buflen, size_t *length );
static void disassemble_ed( libspectrum_word address, char *buffer,
			    size_t buflen, size_t *length );
static void disassemble_ddfd_cb( libspectrum_word address, char offset,
				 enum hl_type use_hl, char *buffer,
				 size_t buflen, size_t *length );

static void get_byte( char *buffer, size_t buflen, libspectrum_byte b );
static void get_word( char *buffer, size_t buflen, libspectrum_word address );
static void get_offset( char *buffer, size_t buflen, libspectrum_word address,
			libspectrum_byte offset );

static const char *reg_pair( libspectrum_byte b, enum hl_type use_hl );
static const char *hl_ix_iy( enum hl_type use_hl );
static void ix_iy_offset( char *buffer, size_t buflen, enum hl_type use_hl,
			  libspectrum_byte offset );

static int source_reg( libspectrum_word address, enum hl_type use_hl,
		       char *buffer, size_t buflen );
static int dest_reg( libspectrum_word address, enum hl_type use_hl,
		     char *buffer, size_t buflen );
static int single_reg( int i, enum hl_type use_hl, libspectrum_byte offset,
		       char *buffer, size_t buflen );

static const char *addition_op( libspectrum_byte b );
static const char *condition( libspectrum_byte b );
static const char *rotate_op( libspectrum_byte b );
static const char *bit_op( libspectrum_byte b );
static int bit_op_bit( libspectrum_byte b );

/* A very thin wrapper to avoid exposing the USE_HL constant */
void
debugger_disassemble( char *buffer, size_t buflen, size_t *length,
		      libspectrum_word address )
{
  disassemble_main( address, buffer, buflen, length, USE_HL );
}

/* Disassemble one instruction */
static void
disassemble_main( libspectrum_word address, char *buffer, size_t buflen,
		  size_t *length, enum hl_type use_hl )
{
  libspectrum_byte b;
  char buffer2[DISASM_BUF_LEN], buffer3[DISASM_BUF_LEN];
  size_t prefix_length = 0;

  b = readbyte_internal( address );

  /* Before we do anything else, strip off any DD or FD prefixes, keeping
     a count of how many we've seen */
  while( b == 0xdd || b == 0xfd ) {
    use_hl = b == 0xdd ? USE_IX : USE_IY;
    address++;
    prefix_length++;
    b = readbyte_internal( address );
  }

  if( b < 0x40 ) {
    disassemble_00xxxxxx( address, buffer, buflen, length, use_hl );
  } else if( b == 0x76 ) {
    snprintf( buffer, buflen, "HALT" ); *length = 1;
  } else if( b < 0x80 ) {

    if( ( b & 0x07 ) == 0x06 ) {		 /* LD something,(HL) */
      dest_reg( address, USE_HL, buffer2, DISASM_BUF_LEN );
      source_reg( address, use_hl, buffer3, DISASM_BUF_LEN );
      *length = ( use_hl == USE_HL ? 1 : 2 );
    } else if( ( ( b >> 3 ) & 0x07 ) == 0x06 ) { /* LD (HL),something */
      dest_reg( address, use_hl, buffer2, DISASM_BUF_LEN );
      source_reg( address, USE_HL, buffer3, DISASM_BUF_LEN );
      *length = ( use_hl == USE_HL ? 1 : 2 );
    } else {				/* Does not involve (HL) at all */
      dest_reg( address, use_hl, buffer2, DISASM_BUF_LEN );
      source_reg( address, use_hl, buffer3, DISASM_BUF_LEN );
      *length = 1;
    }
    /* Note LD (HL),(HL) does not exist */

    snprintf( buffer, buflen, "LD %s,%s", buffer2, buffer3 );

  } else if( b < 0xc0 ) {
    *length = 1 + source_reg( address, use_hl, buffer2, DISASM_BUF_LEN );
    snprintf( buffer, buflen, addition_op( b ), buffer2 );
  } else {
    disassemble_11xxxxxx( address, buffer, buflen, length, use_hl );
  }

  /* Increment the instruction length by the number of prefix bytes */
  *length += prefix_length;
}

/* Disassemble something of the form 00xxxxxx */
static void
disassemble_00xxxxxx( libspectrum_word address, char *buffer, size_t buflen,
		      size_t *length, enum hl_type use_hl )
{
  const char *opcode_00xxx000[] = {
    "NOP", "EX AF,AF'", "DJNZ ", "JR ", "JR NZ,", "JR Z,", "JR NC,", "JR C,"
  };
  const char *opcode_00xxx111[] = {
    "RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"
  };
  char buffer2[DISASM_BUF_LEN], buffer3[DISASM_BUF_LEN];

  libspectrum_byte b = readbyte_internal( address );

  switch( b & 0x0f ) {

  case 0x00: case 0x08:
    if( b <= 0x08 ) {
      snprintf( buffer, buflen, "%s", opcode_00xxx000[ b >> 3 ] ); *length = 1;
    } else {
      get_offset( buffer2, DISASM_BUF_LEN, address + 2, readbyte_internal( address + 1 ) );
      snprintf( buffer, buflen, "%s%s", opcode_00xxx000[ b >> 3 ], buffer2 );
      *length = 2;
    }
    break;

  case 0x01:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "LD %s,%s", reg_pair( b, use_hl ), buffer2 );
    *length = 3;
    break;

  case 0x02:
    disassemble_00xxx010( address, buffer, buflen, length, use_hl );
    break;

  case 0x03:
    snprintf( buffer, buflen, "INC %s", reg_pair( b, use_hl ) ); *length = 1;
    break;

  case 0x04: case 0x0c:
    *length = 1 + dest_reg( address, use_hl, buffer2, DISASM_BUF_LEN );
    snprintf( buffer, buflen, "INC %s", buffer2 );
    break;

  case 0x05: case 0x0d:
    *length = 1 + dest_reg( address, use_hl, buffer2, DISASM_BUF_LEN );
    snprintf( buffer, buflen, "DEC %s", buffer2 );
    break;

  case 0x06: case 0x0e:
    *length = 2 + dest_reg( address, use_hl, buffer2, DISASM_BUF_LEN );
    get_byte( buffer3, DISASM_BUF_LEN, readbyte_internal( address + *length - 1 ) );
    snprintf( buffer, buflen, "LD %s,%s", buffer2, buffer3 );
    break;

  case 0x07: case 0x0f:
    snprintf( buffer, buflen, "%s", opcode_00xxx111[ b >> 3 ] ); *length = 1;
    break;

  case 0x09:
    snprintf( buffer, buflen, "ADD %s,%s", hl_ix_iy( use_hl ),
	      reg_pair( b, use_hl ) );
    *length = 1;
    break;

  case 0x0a:
    disassemble_00xxx110( address, buffer, buflen, length, use_hl );
    break;

  case 0x0b:
    snprintf( buffer, buflen, "DEC %s", reg_pair( b, use_hl ) );
    *length = 1;
    break;

  }
}

/* Disassemble something of the form 00xxx010 */
static void
disassemble_00xxx010( libspectrum_word address, char *buffer, size_t buflen,
		      size_t *length, enum hl_type use_hl )
{
  char buffer2[DISASM_BUF_LEN];
  libspectrum_byte b = readbyte_internal( address );

  switch( b >> 4 ) {

  case 0: case 1: 
    snprintf( buffer, buflen, "LD (%s),A", reg_pair( b, use_hl ) );
    *length = 1;
    break;

  case 2:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "LD (%s),%s", buffer2, hl_ix_iy( use_hl ) );
    *length = 3;
    break;

  case 3:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "LD (%s),A", buffer2 ); *length = 3;
    break;
  }
}

/* Disassemble something of the form 00xxx110 */
static void
disassemble_00xxx110( libspectrum_word address, char *buffer, size_t buflen,
		      size_t *length, enum hl_type use_hl )
{
  char buffer2[DISASM_BUF_LEN];
  libspectrum_byte b = readbyte_internal( address );

  switch( b >> 4 ) {

  case 0: case 1: 
    snprintf( buffer, buflen, "LD A,(%s)", reg_pair( b, use_hl ) );
    *length = 1;
    break;

  case 2:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "LD %s,(%s)", hl_ix_iy( use_hl ), buffer2 );
    *length = 3;
    break;

  case 3:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "LD A,(%s)", buffer2 ); *length = 3;
    break;
  }
}
  
/* Disassemble something of the form 11xxxxxx */
static void
disassemble_11xxxxxx( libspectrum_word address, char *buffer, size_t buflen,
		      size_t *length, enum hl_type use_hl )
{
  char buffer2[DISASM_BUF_LEN];
  libspectrum_byte b = readbyte_internal( address );

  switch( b & 0x07 ) {

  case 0x00:
    snprintf( buffer, buflen, "RET %s", condition( b ) ); *length = 1;
    break;

  case 0x01:
    disassemble_11xxx001( b, buffer, buflen, length, use_hl );
    break;

  case 0x02:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "JP %s,%s", condition( b ), buffer2 );
    *length = 3;
    break;
      
  case 0x03:
    disassemble_11xxx011( address, buffer, buflen, length, use_hl );
    break;

  case 0x04:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "CALL %s,%s", condition( b ), buffer2 );
    *length = 3;
    break;

  case 0x05:
    disassemble_11xxx101( address, buffer, buflen, length, use_hl );
    break;

  case 0x06:
    get_byte( buffer2, DISASM_BUF_LEN, readbyte_internal( address + 1 ) );
    snprintf( buffer, buflen, addition_op( b ), buffer2 );
    *length = 2;
    break;

  case 0x07:
    snprintf( buffer, buflen, "RST %X", 0x08 * ( ( b >> 3 ) - 0x18 ) );
    *length = 1;
    break;
  }
}

/* Disassemble something for the form 11xxx001 */
static void
disassemble_11xxx001( libspectrum_byte b, char *buffer, size_t buflen,
		      size_t *length, enum hl_type use_hl )
{
  switch( ( b >> 3 ) - 0x18 ) {
    
  case 0x00: case 0x02: case 0x04:
    snprintf( buffer, buflen, "POP %s", reg_pair( b, use_hl ) ); *length = 1;
    break;

  case 0x01: snprintf( buffer, buflen, "RET" ); *length = 1; break;
  case 0x03: snprintf( buffer, buflen, "EXX" ); *length = 1; break;

  case 0x05: 
    snprintf( buffer, buflen, "JP (%s)", hl_ix_iy( use_hl ) ); *length = 1;
    break;

  case 0x06: snprintf( buffer, buflen, "POP AF" ); *length = 1; break;

  case 0x07:
    snprintf( buffer, buflen, "LD SP,%s", hl_ix_iy( use_hl ) ); *length = 1;
    break;
  }
}

/* Disassemble something for the form 11xxx011 */
static void
disassemble_11xxx011( libspectrum_word address, char *buffer, size_t buflen,
		      size_t *length, enum hl_type use_hl )
{
  char buffer2[DISASM_BUF_LEN];
  libspectrum_byte b = readbyte_internal( address );

  switch( ( b >> 3 ) - 0x18 ) {

  case 0x00:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "JP %s", buffer2 ); *length = 3;
    break;

  case 0x01:
    if( use_hl != USE_HL ) {
      char offset = readbyte_internal( address + 1 );
      disassemble_ddfd_cb( address+2, offset, use_hl, buffer, buflen,
			   length );
      (*length) += 2;
    } else {
      disassemble_cb( address+1, buffer, buflen, length ); (*length)++;
    }
    break;

  case 0x02:
    get_byte( buffer2, DISASM_BUF_LEN, readbyte_internal( address + 1 ) );
    snprintf( buffer, buflen, "OUT (%s),A", buffer2 ); *length = 2;
    break;

  case 0x03:
    get_byte( buffer2, DISASM_BUF_LEN, readbyte_internal( address + 1 ) );
    snprintf( buffer, buflen, "IN A,(%s)", buffer2 ); *length = 2;
    break;

  case 0x04:
    snprintf( buffer, buflen, "EX (SP),%s", hl_ix_iy( use_hl ) ); *length = 1;
    break;

  case 0x05:
    /* Note: does not get modified by DD or FD */
    snprintf( buffer, buflen, "EX DE,HL" ); *length = 1;
    break;

  case 0x06:
    snprintf( buffer, buflen, "DI" ); *length = 1;
    break;

  case 0x07:
    snprintf( buffer, buflen, "EI" ); *length = 1;
    break;
  }
}

/* Disassemble something for the form 11xxx101 */
static void
disassemble_11xxx101( libspectrum_word address, char *buffer, size_t buflen,
		      size_t *length, enum hl_type use_hl )
{
  char buffer2[DISASM_BUF_LEN];
  libspectrum_byte b = readbyte_internal( address );

  switch( ( b >> 3 ) - 0x18 ) {
	
  case 0x00: case 0x02: case 0x04:
    snprintf( buffer, buflen, "PUSH %s", reg_pair( b, use_hl ) ); *length = 1;
    break;

  case 0x01:
    get_word( buffer2, DISASM_BUF_LEN, address + 1 );
    snprintf( buffer, buflen, "CALL %s", buffer2 ); *length = 3;
    break;

  case 0x03:
  case 0x07:
    /* These should never happen as we strip off all DD/FD prefixes before
     * disassembling the instruction itself */
    ui_error( UI_ERROR_ERROR, "disassemble_11xx101: b = 0x%02x", b );
    fuse_abort();
    break;

  case 0x05:
    disassemble_ed( address+1, buffer, buflen, length ); (*length)++;
    break;

  case 0x06:
    snprintf( buffer, buflen, "PUSH AF" ); *length = 1;
    break;
  }
}

/* Disassemble an instruction after a CB prefix */
static void
disassemble_cb( libspectrum_word address, char *buffer, size_t buflen,
		size_t *length )
{
  char buffer2[DISASM_BUF_LEN];
  libspectrum_byte b = readbyte_internal( address );

  source_reg( address, USE_HL, buffer2, DISASM_BUF_LEN );

  if( b < 0x40 ) {
    snprintf( buffer, buflen, "%s %s", rotate_op( b ), buffer2 );
    *length = 1;
  } else {
    snprintf( buffer, buflen, "%s %d,%s", bit_op( b ), bit_op_bit( b ),
	      buffer2 );
    *length = 1;
  }
}

/* Disassemble an instruction after an ED prefix */
static void
disassemble_ed( libspectrum_word address, char *buffer, size_t buflen,
		size_t *length )
{
  libspectrum_byte b;
  char buffer2[DISASM_BUF_LEN];

  const char *opcode_01xxx111[] = {
    "LD I,A", "LD R,A", "LD A,I", "LD A,R", "RRD", "RLD", "NOPD", "NOPD"
  };

  /* Note 0xbc to 0xbf removed before this table is used */
  const char *opcode_101xxxxx[] = {
    "LDI",  "CPI",  "INI",  "OUTI", "NOPD", "NOPD", "NOPD", "NOPD",
    "LDD",  "CPD",  "IND",  "OUTD", "NOPD", "NOPD", "NOPD", "NOPD",
    "LDIR", "CPIR", "INIR", "OTIR", "NOPD", "NOPD", "NOPD", "NOPD",
    "LDDR", "CPDR", "INDR", "OTDR"
  };

  /* The order in which the IM x instructions appear */
  const int im_modes[] = { 0, 0, 1, 2 };

  b = readbyte_internal( address );

  if( b < 0x40 || b > 0xbb ) {
    snprintf( buffer, buflen, "NOPD" ); *length = 1;
  } else if( b < 0x80 ) {

    switch( b & 0x0f ) {

    case 0x00: case 0x08:
      if( b == 0x70 ) {
	snprintf( buffer, buflen, "IN F,(C)" ); *length = 1;
      } else {
	dest_reg( address, USE_HL, buffer2, DISASM_BUF_LEN );
	snprintf( buffer, buflen, "IN %s,(C)", buffer2 ); *length = 1;
      }
      break;

    case 0x01: case 0x09:
      if( b == 0x71 ) {
	snprintf( buffer, buflen, "OUT (C),0" ); *length = 1;
      } else {
	dest_reg( address, USE_HL, buffer2, DISASM_BUF_LEN );
	snprintf( buffer, buflen, "OUT (C),%s", buffer2 ); *length = 1;
      }
      break;

    case 0x02:
      snprintf( buffer, buflen, "SBC HL,%s", reg_pair( b, USE_HL ) );
      *length = 1;
      break;

    case 0x03:
      get_word( buffer2, DISASM_BUF_LEN, address + 1 );
      snprintf( buffer, buflen, "LD (%s),%s", buffer2, reg_pair( b, USE_HL ) );
      *length = 3;
      break;

    case 0x04: case 0x0c:
      snprintf( buffer, buflen, "NEG" ); *length = 1;
      break;

    case 0x05: case 0x0d:
      if( b == 0x4d ) {
	snprintf( buffer, buflen, "RETI" ); *length = 1;
      } else {
	snprintf( buffer, buflen, "RETN" ); *length = 1;
      }
      break;

    case 0x06: case 0x0e:
      snprintf( buffer, buflen, "IM %d", im_modes[ ( b >> 3 ) & 0x03 ] );
      *length = 1;
      break;

    case 0x07: case 0x0f:
      snprintf( buffer, buflen, "%s", opcode_01xxx111[ ( b >> 3 ) & 0x07 ] );
      *length = 1;
      break;

    case 0x0a:
      snprintf( buffer, buflen, "ADC HL,%s", reg_pair( b, USE_HL ) );
      *length = 1;
      break;

    case 0x0b:
      get_word( buffer2, DISASM_BUF_LEN, address + 1 );
      snprintf( buffer, buflen, "LD %s,(%s)", reg_pair( b, USE_HL ), buffer2 );
      *length = 3;
      break;

    }
  } else if( b < 0xa0 ) {
    snprintf( buffer, buflen, "NOPD" ); *length = 1;
  } else {
    /* Note: 0xbc to 0xbf already removed */
    snprintf( buffer, buflen, "%s", opcode_101xxxxx[ b & 0x1f ] ); *length = 1;
  }
}

/* Disassemble an instruction after DD/FD CB prefixes */
static void
disassemble_ddfd_cb( libspectrum_word address, char offset,
		     enum hl_type use_hl, char *buffer, size_t buflen,
		     size_t *length )
{
  libspectrum_byte b = readbyte_internal( address );
  char buffer2[DISASM_BUF_LEN], buffer3[DISASM_BUF_LEN];

  if( b < 0x40 ) {
    if( ( b & 0x07 ) == 0x06 ) {
      ix_iy_offset( buffer2, DISASM_BUF_LEN, use_hl, offset );
      snprintf( buffer, buflen, "%s %s", rotate_op( b ), buffer2 );
      *length = 1;
    } else {
      source_reg( address, USE_HL, buffer2, DISASM_BUF_LEN );
      ix_iy_offset( buffer3, DISASM_BUF_LEN, use_hl, offset );
      snprintf( buffer, buflen, "LD %s,%s %s", buffer2,
		rotate_op( b ), buffer3 );
      *length = 1;
    }
  } else if( b < 0x80 ) {
    ix_iy_offset( buffer2, DISASM_BUF_LEN, use_hl, offset );
    snprintf( buffer, buflen, "%s %d,%s", bit_op( b ), bit_op_bit( b ), buffer2 );
    *length = 1;
  } else {
    if( ( b & 0x07 ) == 0x06 ) {
      ix_iy_offset( buffer2, DISASM_BUF_LEN, use_hl, offset );
      snprintf( buffer, buflen, "%s %d,%s", bit_op( b ), bit_op_bit( b ),
		buffer2 );
      *length = 1;
    } else {
      source_reg( address, USE_HL, buffer2, DISASM_BUF_LEN );
      ix_iy_offset( buffer3, DISASM_BUF_LEN, use_hl, offset );
      snprintf( buffer, buflen, "LD %s,%s %d,%s", buffer2, bit_op( b ), bit_op_bit( b ), buffer3 );
      *length = 1;
    }
  }
}

/* Get a text representation of a one-byte number */
static void
get_byte( char *buffer, size_t buflen, libspectrum_byte b )
{
  snprintf( buffer, buflen, debugger_output_base == 10 ? "%d" : "%02X", b );
}

/* Get a text representation of an (LSB) two-byte number */
static void
get_word( char *buffer, size_t buflen, libspectrum_word address )
{
  libspectrum_word w;

  w  = readbyte_internal( address + 1 ); w <<= 8;
  w += readbyte_internal( address     );

  snprintf( buffer, buflen, debugger_output_base == 10 ? "%d" : "%04X", w );
}

/* Get a text representation of ( 'address' + 'offset' ) */
static void
get_offset( char *buffer, size_t buflen, libspectrum_word address,
	    libspectrum_byte offset )
{
  address += ( offset >= 0x80 ? offset-0x100 : offset );
  snprintf( buffer, buflen, debugger_output_base == 10 ? "%d" : "%04X",
	    address );
}

/* Select the appropriate register pair from BC, DE, HL (or IX, IY) or
   SP, depending on bits 4 and 5 of the opcode */
static const char *
reg_pair( libspectrum_byte b, enum hl_type use_hl )
{
  switch( ( b >> 4 ) & 0x03 ) {
  case 0: return "BC";
  case 1: return "DE";
  case 2: return hl_ix_iy( use_hl );
  case 3: return "SP";
  }
  return "* INTERNAL ERROR *";	/* Should never happen */
}

/* Get whichever of HL, IX or IY is in use here */
static const char *
hl_ix_iy( enum hl_type use_hl )
{
  switch( use_hl ) {
  case USE_HL: return "HL";
  case USE_IX: return "IX";
  case USE_IY: return "IY";
  }
  return "* INTERNAL ERROR *";	/* Should never happen */
}

/* Get a text representation of '(IX+03)' or similar things */
static void
ix_iy_offset( char *buffer, size_t buflen, enum hl_type use_hl,
	      libspectrum_byte offset )
{
  if( offset < 0x80 ) {
    snprintf( buffer, buflen,
	      debugger_output_base == 10 ? "(%s+%d)" : "(%s+%02X)",
	      hl_ix_iy( use_hl ), offset );
  } else {
    snprintf( buffer, buflen,
	      debugger_output_base == 10 ? "(%s-%d)" : "(%s-%02X)",
	      hl_ix_iy( use_hl ), 0x100 - offset );
  }
}

/* Get an 8-bit register, based on bits 0-2 of the opcode at 'address' */
static int
source_reg( libspectrum_word address, enum hl_type use_hl, char *buffer,
	    size_t buflen )
{
  return single_reg( readbyte_internal( address ) & 0x07, use_hl,
		     readbyte_internal( address + 1 ), buffer, buflen );
}

/* Get an 8-bit register, based on bits 3-5 of the opcode at 'address' */
static int
dest_reg( libspectrum_word address, enum hl_type use_hl, char *buffer,
	  size_t buflen )
{
  return single_reg( ( readbyte_internal( address ) >> 3 ) & 0x07, use_hl,
		     readbyte_internal( address + 1 ), buffer, buflen );
}

/* Get an 8-bit register name, including (HL). Also substitutes
   IXh, IXl and (IX+nn) and the IY versions if appropriate */
static int
single_reg( int i, enum hl_type use_hl, libspectrum_byte offset,
	    char *buffer, size_t buflen )
{
  char buffer2[DISASM_BUF_LEN];

  if( i == 0x04 && use_hl != USE_HL ) {
    snprintf( buffer, buflen, "%sh", hl_ix_iy( use_hl ) );
    return 0;
  } else if( i == 0x05 && use_hl != USE_HL ) {
    snprintf( buffer, buflen, "%sl", hl_ix_iy( use_hl ) );
    return 0;
  } else if( i == 0x06 && use_hl != USE_HL ) {
    ix_iy_offset( buffer2, DISASM_BUF_LEN, use_hl, offset );
    snprintf( buffer, buflen, "%s", buffer2 );
    return 1;
  } else {
    const char *regs[] = { "B", "C", "D", "E", "H", "L", "(HL)", "A" };
    snprintf( buffer, buflen, "%s", regs[i] );
    return 0;
  }
}

/* Various lookup tables for opcodes */

/* Addition/subtraction opcodes:
   10xxxrrr: 'xxx' selects the opcode and 'rrr' the register to be added
   11xxx110: 'xxx' selects the opcode and add a constant
*/
static const char *
addition_op( libspectrum_byte b )
{
  const char *ops[] = { "ADD A,%s", "ADC A,%s", "SUB %s", "SBC A,%s",
			"AND %s",   "XOR %s",   "OR %s",  "CP %s"     };
  return ops[ ( b >> 3 ) & 0x07 ];
}

/* Conditions for jumps, etc:
   11xxx000: RET condition
   11xxx010: JP condition,nnnn
   11xxx100: CALL condition,nnnn
*/
static const char *
condition( libspectrum_byte b )
{
  const char *conds[] = { "NZ", "Z", "NC", "C", "PO", "PE", "P", "M" };
  return conds[ ( b >> 3 ) & 0x07 ];
}

/* Shift/rotate operations:
   CB 00xxxrrr: 'xxx' selects the opcode and 'rrr' the register
   DD/FD CB <offset> 00xxxrrr: the documented rotate/shifts on (IX+nn) etc
                               and the undocumented rotate-and-store opcodes
*/
static const char *
rotate_op( libspectrum_byte b )
{
  const char *ops[] = { "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL" };
  return ops[ b >> 3 ];
}

/* Bit operations:
   CB oobbbrrr: 'oo' (not 00) selects operation
                'bbb' selects bit
                'rrr' selects register
   DD/FD CB <offset> oobbbrrr: the documented bit ops on (IX+nn) etc and the
                               undocumented bit-op-and store
*/
static const char *
bit_op( libspectrum_byte b )
{
  const char *ops[] = { "BIT", "RES", "SET" };
  return ops[ ( b >> 6 ) - 1 ];
}

/* Which bit is used by a BIT, RES or SET with this opcode (bits 3-5) */
static int
bit_op_bit( libspectrum_byte b )
{
  return ( b >> 3 ) & 0x07;
}

/* Get an instruction relative to a specific address */
libspectrum_word
debugger_search_instruction( libspectrum_word address, int delta )
{
  size_t j, length, longest;
  int i;

  if( !delta ) return address;

  if( delta > 0 ) {

    for( i = 0; i < delta; i++ ) {
      debugger_disassemble( NULL, 0, &length, address );
      address += length;
    }

  } else {

    for( i = 0; i > delta; i-- ) {
      /* Look for _longest_ opcode which produces the current top in second
         place */
      for( longest = 1, j = 1; j <= 8; j++ ) {
        debugger_disassemble( NULL, 0, &length, address - j );
        if( length == j ) longest = j;
      }
      address -= longest;
    }

  }

  return address;
}

/* Unit tests */

/* Disassembly test data */
libspectrum_byte test1_data[] = { 0x00 };

libspectrum_byte test2_data[] = { 0xdd, 0x00 };
libspectrum_byte test3_data[] = { 0xdd, 0x09 };
libspectrum_byte test4_data[] = { 0xdd, 0xdd, 0x00 };
libspectrum_byte test5_data[] = { 0xdd, 0xcb, 0x55, 0x06 };

libspectrum_byte test6_data[] = { 0xfd, 0x00 };
libspectrum_byte test7_data[] = { 0xfd, 0x09 };
libspectrum_byte test8_data[] = { 0xfd, 0xfd, 0x00 };
libspectrum_byte test9_data[] = { 0xfd, 0xcb, 0x55, 0x06 };

libspectrum_byte test10_data[] = { 0xdd, 0xfd, 0x09 };
libspectrum_byte test11_data[] = { 0xfd, 0xdd, 0x09 };

libspectrum_byte test12_data[] = { 0xdd, 0xfd, 0xdd, 0xfd, 0xdd, 0xfd, 0xdd,
                                   0xfd, 0xdd, 0xfd, 0xdd, 0xfd, 0x09 };
libspectrum_byte test13_data[] = { 0xfd, 0xdd, 0xfd, 0xdd, 0xfd, 0xdd, 0xfd,
                                   0xdd, 0xfd, 0xdd, 0xfd, 0xdd, 0x09 };

libspectrum_byte test14_data[] = { 0x7e };
libspectrum_byte test15_data[] = { 0xdd, 0x7e, 0x55 };

/* CB prefix rotation/shift tests (one per operation, using register B) */
libspectrum_byte test72_data[] = { 0xcb, 0x00 };  /* RLC B */
libspectrum_byte test73_data[] = { 0xcb, 0x08 };  /* RRC B */
libspectrum_byte test74_data[] = { 0xcb, 0x10 };  /* RL B */
libspectrum_byte test75_data[] = { 0xcb, 0x18 };  /* RR B */
libspectrum_byte test76_data[] = { 0xcb, 0x20 };  /* SLA B */
libspectrum_byte test77_data[] = { 0xcb, 0x28 };  /* SRA B */
libspectrum_byte test78_data[] = { 0xcb, 0x30 };  /* SLL B (undocumented) */
libspectrum_byte test79_data[] = { 0xcb, 0x38 };  /* SRL B */

/* CB prefix rotation on (HL) — exercises (HL) indirect addressing */
libspectrum_byte test80_data[] = { 0xcb, 0x06 };  /* RLC (HL) */

/* CB prefix BIT/RES/SET tests */
libspectrum_byte test16_data[] = { 0xcb, 0x47 };  /* BIT 0,A */
libspectrum_byte test17_data[] = { 0xcb, 0x87 };  /* RES 0,A */
libspectrum_byte test18_data[] = { 0xcb, 0xcf };  /* SET 1,A */

/* DD CB prefix: BIT/RES/SET on (IX+d) */
libspectrum_byte test19_data[] = { 0xdd, 0xcb, 0x55, 0x46 };  /* BIT 0,(IX+55) */
libspectrum_byte test20_data[] = { 0xdd, 0xcb, 0x55, 0x86 };  /* RES 0,(IX+55) */
libspectrum_byte test21_data[] = { 0xdd, 0xcb, 0x55, 0xc6 };  /* SET 0,(IX+55) */

/* FD CB prefix: BIT/RES/SET on (IY+d) */
libspectrum_byte test22_data[] = { 0xfd, 0xcb, 0x55, 0x46 };  /* BIT 0,(IY+55) */
libspectrum_byte test25_data[] = { 0xfd, 0xcb, 0x55, 0x86 };  /* RES 0,(IY+55) */
libspectrum_byte test26_data[] = { 0xfd, 0xcb, 0x55, 0xc6 };  /* SET 0,(IY+55) */

/* DD CB undocumented: LD reg,RES/SET n,(IX+d) — regression for bug #515 fix */
libspectrum_byte test23_data[] = { 0xdd, 0xcb, 0x55, 0x87 };  /* LD A,RES 0,(IX+55) */
libspectrum_byte test24_data[] = { 0xdd, 0xcb, 0x55, 0xcf };  /* LD A,SET 1,(IX+55) */

/* FD CB undocumented: LD reg,RES/SET n,(IY+d) */
libspectrum_byte test27_data[] = { 0xfd, 0xcb, 0x55, 0x87 };  /* LD A,RES 0,(IY+55) */
libspectrum_byte test28_data[] = { 0xfd, 0xcb, 0x55, 0xcf };  /* LD A,SET 1,(IY+55) */

/* DD CB / FD CB: BIT 7, RES 7, SET 7 (bit number 7 = bits 3-5 all set) */
libspectrum_byte testbit7a_data[] = { 0xdd, 0xcb, 0x55, 0x7e };  /* BIT 7,(IX+55) */
libspectrum_byte testbit7b_data[] = { 0xdd, 0xcb, 0x55, 0xbe };  /* RES 7,(IX+55) */
libspectrum_byte testbit7c_data[] = { 0xdd, 0xcb, 0x55, 0xfe };  /* SET 7,(IX+55) */
libspectrum_byte testbit7d_data[] = { 0xfd, 0xcb, 0x55, 0x7e };  /* BIT 7,(IY+55) */
libspectrum_byte testbit7e_data[] = { 0xfd, 0xcb, 0x55, 0xbe };  /* RES 7,(IY+55) */
libspectrum_byte testbit7f_data[] = { 0xfd, 0xcb, 0x55, 0xfe };  /* SET 7,(IY+55) */

/* Negative (IX+d)/(IY+d) offset (offset >= 0x80) */
libspectrum_byte test29_data[] = { 0xdd, 0xcb, 0xff, 0x46 };  /* BIT 0,(IX-01) */
libspectrum_byte test30_data[] = { 0xfd, 0xcb, 0xff, 0x46 };  /* BIT 0,(IY-01) */
libspectrum_byte test31_data[] = { 0xdd, 0x7e, 0xff };         /* LD A,(IX-01) */

/* Relative jump instructions */
libspectrum_byte test32_data[] = { 0x18, 0x00 };  /* JR 0 offset -> target 0x4002 */
libspectrum_byte test33_data[] = { 0x18, 0xfe };  /* JR -2 offset -> target 0x4000 */
libspectrum_byte test34_data[] = { 0x10, 0xfe };  /* DJNZ -2 offset -> target 0x4000 */
libspectrum_byte test35_data[] = { 0x20, 0x04 };  /* JR NZ,+4 -> target 0x4006 */

/* ED prefix: IN reg,(C) and OUT (C),reg */
libspectrum_byte test36_data[] = { 0xed, 0x40 };  /* IN B,(C) */
libspectrum_byte test37_data[] = { 0xed, 0x70 };  /* IN F,(C) — special case */
libspectrum_byte test38_data[] = { 0xed, 0x41 };  /* OUT (C),B */
libspectrum_byte test39_data[] = { 0xed, 0x71 };  /* OUT (C),0 — special case */

/* ED prefix: SBC HL,rr and ADC HL,rr — all register pairs */
libspectrum_byte test40_data[] = { 0xed, 0x42 };  /* SBC HL,BC */
libspectrum_byte test41_data[] = { 0xed, 0x4a };  /* ADC HL,BC */
libspectrum_byte test81_data[] = { 0xed, 0x52 };  /* SBC HL,DE */
libspectrum_byte test82_data[] = { 0xed, 0x5a };  /* ADC HL,DE */
libspectrum_byte test83_data[] = { 0xed, 0x62 };  /* SBC HL,HL */
libspectrum_byte test84_data[] = { 0xed, 0x6a };  /* ADC HL,HL */
libspectrum_byte test85_data[] = { 0xed, 0x72 };  /* SBC HL,SP */
libspectrum_byte test86_data[] = { 0xed, 0x7a };  /* ADC HL,SP */

/* ED prefix: LD (nn),rr and LD rr,(nn) — all register pairs */
libspectrum_byte test42_data[] = { 0xed, 0x43, 0x56, 0x34 };  /* LD (3456),BC */
libspectrum_byte test43_data[] = { 0xed, 0x4b, 0x56, 0x34 };  /* LD BC,(3456) */
libspectrum_byte test87_data[] = { 0xed, 0x53, 0x56, 0x34 };  /* LD (3456),DE */
libspectrum_byte test88_data[] = { 0xed, 0x5b, 0x56, 0x34 };  /* LD DE,(3456) */
libspectrum_byte test89_data[] = { 0xed, 0x63, 0x56, 0x34 };  /* LD (3456),HL */
libspectrum_byte test90_data[] = { 0xed, 0x6b, 0x56, 0x34 };  /* LD HL,(3456) */
libspectrum_byte test91_data[] = { 0xed, 0x73, 0x56, 0x34 };  /* LD (3456),SP */
libspectrum_byte test92_data[] = { 0xed, 0x7b, 0x56, 0x34 };  /* LD SP,(3456) */

/* ED prefix: NEG, RETN, RETI */
libspectrum_byte test44_data[] = { 0xed, 0x44 };  /* NEG */
libspectrum_byte test45_data[] = { 0xed, 0x45 };  /* RETN */
libspectrum_byte test46_data[] = { 0xed, 0x4d };  /* RETI */

/* ED prefix: IM 0, IM 1, IM 2 */
libspectrum_byte test47_data[] = { 0xed, 0x46 };  /* IM 0 */
libspectrum_byte test48_data[] = { 0xed, 0x56 };  /* IM 1 */
libspectrum_byte test49_data[] = { 0xed, 0x5e };  /* IM 2 */

/* ED prefix: LD I,A and LD A,I */
libspectrum_byte test50_data[] = { 0xed, 0x47 };  /* LD I,A */
libspectrum_byte test51_data[] = { 0xed, 0x57 };  /* LD A,I */
libspectrum_byte test56_data[] = { 0xed, 0x4f };  /* LD R,A */
libspectrum_byte test57_data[] = { 0xed, 0x5f };  /* LD A,R */
libspectrum_byte test58_data[] = { 0xed, 0x67 };  /* RRD */
libspectrum_byte test59_data[] = { 0xed, 0x6f };  /* RLD */

/* ED prefix: block instructions */
libspectrum_byte test52_data[] = { 0xed, 0xa0 };  /* LDI */
libspectrum_byte test53_data[] = { 0xed, 0xb0 };  /* LDIR */
libspectrum_byte test54_data[] = { 0xed, 0xa8 };  /* LDD */
libspectrum_byte test55_data[] = { 0xed, 0xb8 };  /* LDDR */
libspectrum_byte test60_data[] = { 0xed, 0xa1 };  /* CPI */
libspectrum_byte test61_data[] = { 0xed, 0xa9 };  /* CPD */
libspectrum_byte test62_data[] = { 0xed, 0xb1 };  /* CPIR */
libspectrum_byte test63_data[] = { 0xed, 0xb9 };  /* CPDR */
libspectrum_byte test64_data[] = { 0xed, 0xa2 };  /* INI */
libspectrum_byte test65_data[] = { 0xed, 0xaa };  /* IND */
libspectrum_byte test66_data[] = { 0xed, 0xb2 };  /* INIR */
libspectrum_byte test67_data[] = { 0xed, 0xba };  /* INDR */
libspectrum_byte test68_data[] = { 0xed, 0xa3 };  /* OUTI */
libspectrum_byte test69_data[] = { 0xed, 0xab };  /* OUTD */
libspectrum_byte test70_data[] = { 0xed, 0xb3 };  /* OTIR */
libspectrum_byte test71_data[] = { 0xed, 0xbb };  /* OTDR */

/* DD prefix: ADD IX,rr completeness */
libspectrum_byte test93_data[] = { 0xdd, 0x19 };   /* ADD IX,DE */
libspectrum_byte test94_data[] = { 0xdd, 0x29 };   /* ADD IX,IX */
libspectrum_byte test95_data[] = { 0xdd, 0x39 };   /* ADD IX,SP */

/* FD prefix: ADD IY,rr completeness */
libspectrum_byte test96_data[] = { 0xfd, 0x19 };   /* ADD IY,DE */
libspectrum_byte test97_data[] = { 0xfd, 0x29 };   /* ADD IY,IY */
libspectrum_byte test98_data[] = { 0xfd, 0x39 };   /* ADD IY,SP */

/* DD/FD prefix: LD IX/IY,nn */
libspectrum_byte test99_data[]  = { 0xdd, 0x21, 0x34, 0x12 }; /* LD IX,1234 */
libspectrum_byte test100_data[] = { 0xfd, 0x21, 0x34, 0x12 }; /* LD IY,1234 */

/* DD/FD prefix: INC/DEC IX/IY */
libspectrum_byte test101_data[] = { 0xdd, 0x23 };  /* INC IX */
libspectrum_byte test102_data[] = { 0xdd, 0x2b };  /* DEC IX */
libspectrum_byte test103_data[] = { 0xfd, 0x23 };  /* INC IY */
libspectrum_byte test104_data[] = { 0xfd, 0x2b };  /* DEC IY */

/* DD/FD prefix: LD (nn),IX/IY and LD IX/IY,(nn) */
libspectrum_byte test105_data[] = { 0xdd, 0x22, 0x34, 0x12 }; /* LD (1234),IX */
libspectrum_byte test106_data[] = { 0xdd, 0x2a, 0x34, 0x12 }; /* LD IX,(1234) */
libspectrum_byte test107_data[] = { 0xfd, 0x22, 0x34, 0x12 }; /* LD (1234),IY */
libspectrum_byte test108_data[] = { 0xfd, 0x2a, 0x34, 0x12 }; /* LD IY,(1234) */

/* DD/FD prefix: PUSH/POP IX/IY */
libspectrum_byte test109_data[] = { 0xdd, 0xe5 };  /* PUSH IX */
libspectrum_byte test110_data[] = { 0xdd, 0xe1 };  /* POP IX */
libspectrum_byte test111_data[] = { 0xfd, 0xe5 };  /* PUSH IY */
libspectrum_byte test112_data[] = { 0xfd, 0xe1 };  /* POP IY */

/* DD/FD prefix: JP (IX/IY), EX (SP),IX/IY, LD SP,IX/IY */
libspectrum_byte test113_data[] = { 0xdd, 0xe9 };  /* JP (IX) */
libspectrum_byte test114_data[] = { 0xdd, 0xe3 };  /* EX (SP),IX */
libspectrum_byte test115_data[] = { 0xdd, 0xf9 };  /* LD SP,IX */
libspectrum_byte test116_data[] = { 0xfd, 0xe9 };  /* JP (IY) */
libspectrum_byte test117_data[] = { 0xfd, 0xe3 };  /* EX (SP),IY */
libspectrum_byte test118_data[] = { 0xfd, 0xf9 };  /* LD SP,IY */

/* DD/FD prefix: undocumented IXh, IXl, IYh, IYl registers */
libspectrum_byte test119_data[] = { 0xdd, 0x7c };  /* LD A,IXh */
libspectrum_byte test120_data[] = { 0xdd, 0x7d };  /* LD A,IXl */
libspectrum_byte test121_data[] = { 0xfd, 0x7c };  /* LD A,IYh */
libspectrum_byte test122_data[] = { 0xfd, 0x7d };  /* LD A,IYl */

/* DD/FD prefix: LD r,(IX/IY+d) */
libspectrum_byte test123_data[] = { 0xdd, 0x46, 0x05 }; /* LD B,(IX+05) */
libspectrum_byte test124_data[] = { 0xfd, 0x46, 0x05 }; /* LD B,(IY+05) */

/* DD/FD prefix: LD (IX/IY+d),r */
libspectrum_byte test125_data[] = { 0xdd, 0x70, 0x05 }; /* LD (IX+05),B */
libspectrum_byte test126_data[] = { 0xfd, 0x70, 0x05 }; /* LD (IY+05),B */

/* DD/FD prefix: LD (IX/IY+d),n */
libspectrum_byte test127_data[] = { 0xdd, 0x36, 0x05, 0x07 }; /* LD (IX+05),07 */
libspectrum_byte test128_data[] = { 0xfd, 0x36, 0x05, 0x07 }; /* LD (IY+05),07 */

/* Main (unprefixed) opcodes: 00xxx000 group */
libspectrum_byte test129_data[] = { 0x08 };  /* EX AF,AF' */

/* 00xxx111 group: rotate/accumulator flags */
libspectrum_byte test130_data[] = { 0x07 };  /* RLCA */
libspectrum_byte test131_data[] = { 0x0f };  /* RRCA */
libspectrum_byte test132_data[] = { 0x17 };  /* RLA */
libspectrum_byte test133_data[] = { 0x1f };  /* RRA */
libspectrum_byte test134_data[] = { 0x27 };  /* DAA */
libspectrum_byte test135_data[] = { 0x2f };  /* CPL */
libspectrum_byte test136_data[] = { 0x37 };  /* SCF */
libspectrum_byte test137_data[] = { 0x3f };  /* CCF */

/* 00xxx001: LD rr,nn; 00xxx011/00xxx1011: INC/DEC rr; 00xxx1001: ADD HL,rr */
libspectrum_byte test138_data[] = { 0x01, 0x34, 0x12 };  /* LD BC,1234 */
libspectrum_byte test139_data[] = { 0x03 };               /* INC BC */
libspectrum_byte test140_data[] = { 0x0b };               /* DEC BC */
libspectrum_byte test141_data[] = { 0x19 };               /* ADD HL,DE */

/* ADD HL,rr — remaining three register pairs */
static libspectrum_byte test352_data[] = { 0x09 };  /* ADD HL,BC */
static libspectrum_byte test353_data[] = { 0x29 };  /* ADD HL,HL */
static libspectrum_byte test354_data[] = { 0x39 };  /* ADD HL,SP */

/* INC/DEC rr — remaining three register pairs (DE, HL, SP) */
static libspectrum_byte test355_data[] = { 0x13 };  /* INC DE */
static libspectrum_byte test356_data[] = { 0x23 };  /* INC HL */
static libspectrum_byte test357_data[] = { 0x33 };  /* INC SP */
static libspectrum_byte test358_data[] = { 0x1b };  /* DEC DE */
static libspectrum_byte test359_data[] = { 0x2b };  /* DEC HL */
static libspectrum_byte test360_data[] = { 0x3b };  /* DEC SP */

/* 00xxx100/101/110: INC r, DEC r, LD r,n */
libspectrum_byte test142_data[] = { 0x04 };        /* INC B */
libspectrum_byte test143_data[] = { 0x05 };        /* DEC B */
libspectrum_byte test144_data[] = { 0x06, 0x07 };  /* LD B,07 */

/* 00xxx010: LD (rr),A / LD (nn),HL / LD (nn),A */
libspectrum_byte test145_data[] = { 0x02 };               /* LD (BC),A */
libspectrum_byte test146_data[] = { 0x12 };               /* LD (DE),A */
libspectrum_byte test147_data[] = { 0x22, 0x34, 0x12 };  /* LD (1234),HL */
libspectrum_byte test148_data[] = { 0x32, 0x34, 0x12 };  /* LD (1234),A */

/* 00xxx110 (0x0a column): LD A,(rr) / LD HL,(nn) / LD A,(nn) */
libspectrum_byte test149_data[] = { 0x0a };               /* LD A,(BC) */
libspectrum_byte test150_data[] = { 0x1a };               /* LD A,(DE) */
libspectrum_byte test151_data[] = { 0x2a, 0x34, 0x12 };  /* LD HL,(1234) */
libspectrum_byte test152_data[] = { 0x3a, 0x34, 0x12 };  /* LD A,(1234) */

/* 01xxxxxx: LD r,r' and HALT */
libspectrum_byte test153_data[] = { 0x76 };  /* HALT */
libspectrum_byte test154_data[] = { 0x41 };  /* LD B,C */

/* 10xxxxxx: arithmetic/logic on registers */
libspectrum_byte test155_data[] = { 0x80 };  /* ADD A,B */
libspectrum_byte test156_data[] = { 0x91 };  /* SUB C */
libspectrum_byte test157_data[] = { 0xa2 };  /* AND D */
libspectrum_byte test158_data[] = { 0xab };  /* XOR E */
libspectrum_byte test159_data[] = { 0xb4 };  /* OR H */
libspectrum_byte test160_data[] = { 0xbd };  /* CP L */

/* 10xxxxxx: ADC A,r and SBC A,r (register forms — distinct from immediate) */
static libspectrum_byte test342_data[] = { 0x88 };  /* ADC A,B */
static libspectrum_byte test343_data[] = { 0x99 };  /* SBC A,C */

/* 10xxxxxx: arithmetic/logic with (HL) operand (rrr = 110) */
static libspectrum_byte test344_data[] = { 0x86 };  /* ADD A,(HL) */
static libspectrum_byte test345_data[] = { 0x8e };  /* ADC A,(HL) */
static libspectrum_byte test346_data[] = { 0x96 };  /* SUB (HL) */
static libspectrum_byte test347_data[] = { 0x9e };  /* SBC A,(HL) */
static libspectrum_byte test348_data[] = { 0xa6 };  /* AND (HL) */
static libspectrum_byte test349_data[] = { 0xae };  /* XOR (HL) */
static libspectrum_byte test350_data[] = { 0xb6 };  /* OR (HL) */
static libspectrum_byte test351_data[] = { 0xbe };  /* CP (HL) */

/* 11xxx110: immediate arithmetic/logic */
libspectrum_byte test161_data[] = { 0xc6, 0x07 };  /* ADD A,07 */

/* 11xxx000: conditional RET; 11xxx001: POP/RET/EXX/JP(HL)/PUSH/CALL */
libspectrum_byte test162_data[] = { 0xc0 };  /* RET NZ */
libspectrum_byte test163_data[] = { 0xc1 };  /* POP BC */
libspectrum_byte test164_data[] = { 0xc9 };  /* RET */
libspectrum_byte test165_data[] = { 0xd9 };  /* EXX */
libspectrum_byte test166_data[] = { 0xe9 };  /* JP (HL) */
libspectrum_byte test167_data[] = { 0xf1 };  /* POP AF */
libspectrum_byte test168_data[] = { 0xf9 };  /* LD SP,HL */

/* 11xxx010: conditional JP; 11xxx011: JP/CB/OUT/IN/EX/DI/EI */
libspectrum_byte test169_data[] = { 0xc2, 0x34, 0x12 };  /* JP NZ,1234 */
libspectrum_byte test170_data[] = { 0xc3, 0x34, 0x12 };  /* JP 1234 */
libspectrum_byte test171_data[] = { 0xd3, 0x07 };         /* OUT (07),A */
libspectrum_byte test172_data[] = { 0xdb, 0x07 };         /* IN A,(07) */
libspectrum_byte test173_data[] = { 0xe3 };               /* EX (SP),HL */
libspectrum_byte test174_data[] = { 0xeb };               /* EX DE,HL */
libspectrum_byte test175_data[] = { 0xf3 };               /* DI */
libspectrum_byte test176_data[] = { 0xfb };               /* EI */

/* 11xxx100: conditional CALL; 11xxx101: PUSH/CALL */
libspectrum_byte test177_data[] = { 0xc4, 0x34, 0x12 };  /* CALL NZ,1234 */
libspectrum_byte test178_data[] = { 0xc5 };               /* PUSH BC */
libspectrum_byte test179_data[] = { 0xcd, 0x34, 0x12 };  /* CALL 1234 */
libspectrum_byte test180_data[] = { 0xf5 };               /* PUSH AF */

/* 11xxx111: RST */
libspectrum_byte test181_data[] = { 0xc7 };  /* RST 0 */
libspectrum_byte test182_data[] = { 0xef };  /* RST 28 */

/* Decimal-mode tests: each exercises a distinct numeric-formatting path */

/* testd1: get_byte() in decimal mode -- LD B,n */
libspectrum_byte testd1_data[] = { 0x06, 0x07 };          /* LD B,07h */

/* testd2: get_word() in decimal mode -- LD rr,nn */
libspectrum_byte testd2_data[] = { 0x01, 0x34, 0x12 };    /* LD BC,1234h */

/* testd3: get_offset() in decimal mode -- JR e */
libspectrum_byte testd3_data[] = { 0x18, 0x00 };          /* JR +0 -> 4002h */

/* testd4: ix_iy_offset() positive in decimal mode -- LD A,(IX+d) */
libspectrum_byte testd4_data[] = { 0xdd, 0x7e, 0x55 };    /* LD A,(IX+55h) */

/* testd5: ix_iy_offset() negative in decimal mode -- BIT b,(IX+d) */
libspectrum_byte testd5_data[] = { 0xdd, 0xcb, 0xff, 0x46 };  /* BIT 0,(IX-01h) */

/* testd6: get_byte() via 11xxx110 path -- ADD A,n */
libspectrum_byte testd6_data[] = { 0xc6, 0x07 };          /* ADD A,07h */

/* testd7: ix_iy_offset() positive with IY in decimal mode -- LD A,(IY+d) */
libspectrum_byte testd7_data[] = { 0xfd, 0x7e, 0x55 };    /* LD A,(IY+55h) */

/* testd8: ix_iy_offset() negative with IY in decimal mode -- BIT b,(IY+d) */
libspectrum_byte testd8_data[] = { 0xfd, 0xcb, 0xff, 0x46 };  /* BIT 0,(IY-01h) */

/* ED NOPD boundary tests: opcodes that fall outside the valid ED range
   (b < 0x40 or b > 0xbb) are decoded as NOPD (2-byte instruction).
   The 0x80-0x9f sub-range inside the ED handler also decodes as NOPD. */
libspectrum_byte test183_data[] = { 0xed, 0x00 };  /* NOPD: lower range start */
libspectrum_byte test184_data[] = { 0xed, 0x3f };  /* NOPD: lower range end */
libspectrum_byte test185_data[] = { 0xed, 0x80 };  /* NOPD: mid-range 0x80–0x9f start */
libspectrum_byte test186_data[] = { 0xed, 0x9f };  /* NOPD: mid-range 0x80–0x9f end */
libspectrum_byte test187_data[] = { 0xed, 0xbc };  /* NOPD: upper range start */
libspectrum_byte test188_data[] = { 0xed, 0xff };  /* NOPD: upper range end */

/* 00xxx000: conditional JR — remaining conditions (NZ already covered) */
libspectrum_byte test189_data[] = { 0x28, 0x04 };  /* JR Z,+4  -> 4006 */
libspectrum_byte test190_data[] = { 0x30, 0x04 };  /* JR NC,+4 -> 4006 */
libspectrum_byte test191_data[] = { 0x38, 0x04 };  /* JR C,+4  -> 4006 */

/* 11xxx000: conditional RET — remaining seven conditions */
libspectrum_byte test192_data[] = { 0xc8 };  /* RET Z */
libspectrum_byte test193_data[] = { 0xd0 };  /* RET NC */
libspectrum_byte test194_data[] = { 0xd8 };  /* RET C */
libspectrum_byte test195_data[] = { 0xe0 };  /* RET PO */
libspectrum_byte test196_data[] = { 0xe8 };  /* RET PE */
libspectrum_byte test197_data[] = { 0xf0 };  /* RET P */
libspectrum_byte test198_data[] = { 0xf8 };  /* RET M */

/* 11xxx010: conditional JP — remaining seven conditions */
libspectrum_byte test199_data[] = { 0xca, 0x34, 0x12 };  /* JP Z,1234 */
libspectrum_byte test200_data[] = { 0xd2, 0x34, 0x12 };  /* JP NC,1234 */
libspectrum_byte test201_data[] = { 0xda, 0x34, 0x12 };  /* JP C,1234 */
libspectrum_byte test202_data[] = { 0xe2, 0x34, 0x12 };  /* JP PO,1234 */
libspectrum_byte test203_data[] = { 0xea, 0x34, 0x12 };  /* JP PE,1234 */
libspectrum_byte test204_data[] = { 0xf2, 0x34, 0x12 };  /* JP P,1234 */
libspectrum_byte test205_data[] = { 0xfa, 0x34, 0x12 };  /* JP M,1234 */

/* 11xxx100: conditional CALL — remaining seven conditions */
libspectrum_byte test206_data[] = { 0xcc, 0x34, 0x12 };  /* CALL Z,1234 */
libspectrum_byte test207_data[] = { 0xd4, 0x34, 0x12 };  /* CALL NC,1234 */
libspectrum_byte test208_data[] = { 0xdc, 0x34, 0x12 };  /* CALL C,1234 */
libspectrum_byte test209_data[] = { 0xe4, 0x34, 0x12 };  /* CALL PO,1234 */
libspectrum_byte test210_data[] = { 0xec, 0x34, 0x12 };  /* CALL PE,1234 */
libspectrum_byte test211_data[] = { 0xf4, 0x34, 0x12 };  /* CALL P,1234 */
libspectrum_byte test212_data[] = { 0xfc, 0x34, 0x12 };  /* CALL M,1234 */

/* 11xxx111: RST — remaining six targets */
libspectrum_byte test213_data[] = { 0xcf };  /* RST 8 */
libspectrum_byte test214_data[] = { 0xd7 };  /* RST 10 */
libspectrum_byte test215_data[] = { 0xdf };  /* RST 18 */
libspectrum_byte test216_data[] = { 0xe7 };  /* RST 20 */
libspectrum_byte test217_data[] = { 0xf7 };  /* RST 30 */
libspectrum_byte test218_data[] = { 0xff };  /* RST 38 */

/* 11xxx110: immediate arithmetic/logic — remaining seven operations */
libspectrum_byte test219_data[] = { 0xce, 0x07 };  /* ADC A,07 */
libspectrum_byte test220_data[] = { 0xd6, 0x07 };  /* SUB 07 */
libspectrum_byte test221_data[] = { 0xde, 0x07 };  /* SBC A,07 */
libspectrum_byte test222_data[] = { 0xe6, 0x07 };  /* AND 07 */
libspectrum_byte test223_data[] = { 0xee, 0x07 };  /* XOR 07 */
libspectrum_byte test224_data[] = { 0xf6, 0x07 };  /* OR 07 */
libspectrum_byte test225_data[] = { 0xfe, 0x07 };  /* CP 07 */

/* DD CB prefix: remaining documented rotate/shift ops on (IX+d) */
libspectrum_byte test226_data[] = { 0xdd, 0xcb, 0x55, 0x0e };  /* RRC (IX+55) */
libspectrum_byte test227_data[] = { 0xdd, 0xcb, 0x55, 0x16 };  /* RL (IX+55) */
libspectrum_byte test228_data[] = { 0xdd, 0xcb, 0x55, 0x1e };  /* RR (IX+55) */
libspectrum_byte test229_data[] = { 0xdd, 0xcb, 0x55, 0x26 };  /* SLA (IX+55) */
libspectrum_byte test230_data[] = { 0xdd, 0xcb, 0x55, 0x2e };  /* SRA (IX+55) */
libspectrum_byte test231_data[] = { 0xdd, 0xcb, 0x55, 0x36 };  /* SLL (IX+55) */
libspectrum_byte test232_data[] = { 0xdd, 0xcb, 0x55, 0x3e };  /* SRL (IX+55) */

/* FD CB prefix: remaining documented rotate/shift ops on (IY+d) */
libspectrum_byte test233_data[] = { 0xfd, 0xcb, 0x55, 0x0e };  /* RRC (IY+55) */
libspectrum_byte test234_data[] = { 0xfd, 0xcb, 0x55, 0x16 };  /* RL (IY+55) */
libspectrum_byte test235_data[] = { 0xfd, 0xcb, 0x55, 0x1e };  /* RR (IY+55) */
libspectrum_byte test236_data[] = { 0xfd, 0xcb, 0x55, 0x26 };  /* SLA (IY+55) */
libspectrum_byte test237_data[] = { 0xfd, 0xcb, 0x55, 0x2e };  /* SRA (IY+55) */
libspectrum_byte test238_data[] = { 0xfd, 0xcb, 0x55, 0x36 };  /* SLL (IY+55) */
libspectrum_byte test239_data[] = { 0xfd, 0xcb, 0x55, 0x3e };  /* SRL (IY+55) */

/* DD CB undocumented: LD reg,rotate (IX+d) — exercises the b<0x40, b&7!=6 branch */
libspectrum_byte test240_data[] = { 0xdd, 0xcb, 0x55, 0x00 };  /* LD B,RLC (IX+55) */

/* FD CB undocumented: LD reg,rotate (IY+d) */
libspectrum_byte test241_data[] = { 0xfd, 0xcb, 0x55, 0x00 };  /* LD B,RLC (IY+55) */

/* DD CB undocumented: remaining destination registers with RLC (IX+d) */
libspectrum_byte test242_data[] = { 0xdd, 0xcb, 0x55, 0x01 };  /* LD C,RLC (IX+55) */
libspectrum_byte test243_data[] = { 0xdd, 0xcb, 0x55, 0x02 };  /* LD D,RLC (IX+55) */
libspectrum_byte test244_data[] = { 0xdd, 0xcb, 0x55, 0x03 };  /* LD E,RLC (IX+55) */
libspectrum_byte test245_data[] = { 0xdd, 0xcb, 0x55, 0x04 };  /* LD H,RLC (IX+55) */
libspectrum_byte test246_data[] = { 0xdd, 0xcb, 0x55, 0x05 };  /* LD L,RLC (IX+55) */
libspectrum_byte test247_data[] = { 0xdd, 0xcb, 0x55, 0x07 };  /* LD A,RLC (IX+55) */

/* FD CB undocumented: remaining destination registers with RLC (IY+d) */
libspectrum_byte test248_data[] = { 0xfd, 0xcb, 0x55, 0x01 };  /* LD C,RLC (IY+55) */
libspectrum_byte test249_data[] = { 0xfd, 0xcb, 0x55, 0x02 };  /* LD D,RLC (IY+55) */
libspectrum_byte test250_data[] = { 0xfd, 0xcb, 0x55, 0x03 };  /* LD E,RLC (IY+55) */
libspectrum_byte test251_data[] = { 0xfd, 0xcb, 0x55, 0x04 };  /* LD H,RLC (IY+55) */
libspectrum_byte test252_data[] = { 0xfd, 0xcb, 0x55, 0x05 };  /* LD L,RLC (IY+55) */
libspectrum_byte test253_data[] = { 0xfd, 0xcb, 0x55, 0x07 };  /* LD A,RLC (IY+55) */

/* DD CB undocumented: non-RLC rotation op to verify rotate_op() selection */
libspectrum_byte test254_data[] = { 0xdd, 0xcb, 0x55, 0x38 };  /* LD B,SRL (IX+55) */

/* DD/FD prefix: LD r,(IX/IY+d) — remaining registers C, D, E, H, L */
libspectrum_byte test255_data[] = { 0xdd, 0x4e, 0x05 };  /* LD C,(IX+05) */
libspectrum_byte test256_data[] = { 0xdd, 0x56, 0x05 };  /* LD D,(IX+05) */
libspectrum_byte test257_data[] = { 0xdd, 0x5e, 0x05 };  /* LD E,(IX+05) */
libspectrum_byte test258_data[] = { 0xdd, 0x66, 0x05 };  /* LD H,(IX+05) */
libspectrum_byte test259_data[] = { 0xdd, 0x6e, 0x05 };  /* LD L,(IX+05) */
libspectrum_byte test260_data[] = { 0xfd, 0x4e, 0x05 };  /* LD C,(IY+05) */
libspectrum_byte test261_data[] = { 0xfd, 0x56, 0x05 };  /* LD D,(IY+05) */
libspectrum_byte test262_data[] = { 0xfd, 0x5e, 0x05 };  /* LD E,(IY+05) */
libspectrum_byte test263_data[] = { 0xfd, 0x66, 0x05 };  /* LD H,(IY+05) */
libspectrum_byte test264_data[] = { 0xfd, 0x6e, 0x05 };  /* LD L,(IY+05) */
libspectrum_byte test265_data[] = { 0xfd, 0x7e, 0x05 };  /* LD A,(IY+05) */

/* DD/FD prefix: LD (IX/IY+d),r — remaining registers C, D, E, H, L, A */
libspectrum_byte test266_data[] = { 0xdd, 0x71, 0x05 };  /* LD (IX+05),C */
libspectrum_byte test267_data[] = { 0xdd, 0x72, 0x05 };  /* LD (IX+05),D */
libspectrum_byte test268_data[] = { 0xdd, 0x73, 0x05 };  /* LD (IX+05),E */
libspectrum_byte test269_data[] = { 0xdd, 0x74, 0x05 };  /* LD (IX+05),H */
libspectrum_byte test270_data[] = { 0xdd, 0x75, 0x05 };  /* LD (IX+05),L */
libspectrum_byte test271_data[] = { 0xdd, 0x77, 0x05 };  /* LD (IX+05),A */
libspectrum_byte test272_data[] = { 0xfd, 0x71, 0x05 };  /* LD (IY+05),C */
libspectrum_byte test273_data[] = { 0xfd, 0x72, 0x05 };  /* LD (IY+05),D */
libspectrum_byte test274_data[] = { 0xfd, 0x73, 0x05 };  /* LD (IY+05),E */
libspectrum_byte test275_data[] = { 0xfd, 0x74, 0x05 };  /* LD (IY+05),H */
libspectrum_byte test276_data[] = { 0xfd, 0x75, 0x05 };  /* LD (IY+05),L */
libspectrum_byte test277_data[] = { 0xfd, 0x77, 0x05 };  /* LD (IY+05),A */

/* CB prefix: all 8 rotate/shift operations with register A (rrr = 111).
   Together with the existing B tests (test72-79) these verify that
   source_reg() returns the correct register name at both ends of
   the register range used by disassemble_cb(). */
static libspectrum_byte test278_data[] = { 0xcb, 0x07 };  /* RLC A */
static libspectrum_byte test279_data[] = { 0xcb, 0x0f };  /* RRC A */
static libspectrum_byte test280_data[] = { 0xcb, 0x17 };  /* RL A */
static libspectrum_byte test281_data[] = { 0xcb, 0x1f };  /* RR A */
static libspectrum_byte test282_data[] = { 0xcb, 0x27 };  /* SLA A */
static libspectrum_byte test283_data[] = { 0xcb, 0x2f };  /* SRA A */
static libspectrum_byte test284_data[] = { 0xcb, 0x37 };  /* SLL A (undocumented) */
static libspectrum_byte test285_data[] = { 0xcb, 0x3f };  /* SRL A */

/* CB prefix: all remaining rotate/shift operations with (HL) (rrr = 110).
   RLC (HL) is already covered by test80; these add the other seven ops
   to verify rotate_op() selects each mnemonic when the register is (HL). */
static libspectrum_byte test286_data[] = { 0xcb, 0x0e };  /* RRC (HL) */
static libspectrum_byte test287_data[] = { 0xcb, 0x16 };  /* RL (HL) */
static libspectrum_byte test288_data[] = { 0xcb, 0x1e };  /* RR (HL) */
static libspectrum_byte test289_data[] = { 0xcb, 0x26 };  /* SLA (HL) */
static libspectrum_byte test290_data[] = { 0xcb, 0x2e };  /* SRA (HL) */
static libspectrum_byte test291_data[] = { 0xcb, 0x36 };  /* SLL (HL) (undocumented) */
static libspectrum_byte test292_data[] = { 0xcb, 0x3e };  /* SRL (HL) */

/* CB prefix: BIT, RES and SET with bit 7 and (HL) (rrr = 110).
   Exercises bit_op(), bit_op_bit() with the maximum bit number,
   and source_reg() with the (HL) register in the >= 0x40 branch. */
static libspectrum_byte test293_data[] = { 0xcb, 0x7e };  /* BIT 7,(HL) */
static libspectrum_byte test294_data[] = { 0xcb, 0xbe };  /* RES 7,(HL) */
static libspectrum_byte test295_data[] = { 0xcb, 0xfe };  /* SET 7,(HL) */

/* CB prefix: BIT, RES and SET with bit 0 and (HL) (rrr = 110).
   Exercises bit_op_bit() with the minimum bit number (b = 0) and
   source_reg() with (HL), complementing the bit-7 tests above. */
static libspectrum_byte test361_data[] = { 0xcb, 0x46 };  /* BIT 0,(HL) */
static libspectrum_byte test362_data[] = { 0xcb, 0x86 };  /* RES 0,(HL) */
static libspectrum_byte test363_data[] = { 0xcb, 0xc6 };  /* SET 0,(HL) */

/* CB prefix: all eight rotation/shift ops for register C (rrr = 001).
   Verifies that the dest_reg()/source_reg() lookup returns "C" for
   every shift-group opcode.  Opcode = (op << 3) | 001. */
static libspectrum_byte test296_data[] = { 0xcb, 0x01 };  /* RLC C */
static libspectrum_byte test297_data[] = { 0xcb, 0x09 };  /* RRC C */
static libspectrum_byte test298_data[] = { 0xcb, 0x11 };  /* RL  C */
static libspectrum_byte test299_data[] = { 0xcb, 0x19 };  /* RR  C */
static libspectrum_byte test300_data[] = { 0xcb, 0x21 };  /* SLA C */
static libspectrum_byte test301_data[] = { 0xcb, 0x29 };  /* SRA C */
static libspectrum_byte test302_data[] = { 0xcb, 0x31 };  /* SLL C (undocumented) */
static libspectrum_byte test303_data[] = { 0xcb, 0x39 };  /* SRL C */

/* CB prefix: one rotation op per remaining register to verify register
   name lookup for rrr = 010 (D), 011 (E), 100 (H), 101 (L). */
static libspectrum_byte test304_data[] = { 0xcb, 0x02 };  /* RLC D */
static libspectrum_byte test305_data[] = { 0xcb, 0x03 };  /* RLC E */
static libspectrum_byte test306_data[] = { 0xcb, 0x04 };  /* RLC H */
static libspectrum_byte test307_data[] = { 0xcb, 0x05 };  /* RLC L */

/* CB prefix BIT/RES/SET: cover all non-A, non-(HL) register names.
   The existing tests (test16-18) cover register A; test293-295 add (HL).
   These six tests exercise source_reg() for B(0), C(1), D(2), E(3), H(4), L(5)
   and bit_op_bit() for several mid-range bit numbers. */
static libspectrum_byte test308_data[] = { 0xcb, 0x58 };  /* BIT 3,B */
static libspectrum_byte test309_data[] = { 0xcb, 0x69 };  /* BIT 5,C */
static libspectrum_byte test310_data[] = { 0xcb, 0xa2 };  /* RES 4,D */
static libspectrum_byte test311_data[] = { 0xcb, 0x94 };  /* RES 2,H */
static libspectrum_byte test312_data[] = { 0xcb, 0xf5 };  /* SET 6,L */
static libspectrum_byte test313_data[] = { 0xcb, 0xdb };  /* SET 3,E */

/* CB prefix: remaining 7 rotation/shift ops for each of D, E, H, L.
   Completes single-register coverage so every op×register combination
   in the rotate_op()/source_reg() dispatch path is exercised.
   Opcode = (op_index << 3) | register_index, with:
     op:  0=RLC 1=RRC 2=RL 3=RR 4=SLA 5=SRA 6=SLL 7=SRL
     reg: 2=D  3=E  4=H  5=L */
/* D (rrr = 010) */
static libspectrum_byte test314_data[] = { 0xcb, 0x0a };  /* RRC D */
static libspectrum_byte test315_data[] = { 0xcb, 0x12 };  /* RL  D */
static libspectrum_byte test316_data[] = { 0xcb, 0x1a };  /* RR  D */
static libspectrum_byte test317_data[] = { 0xcb, 0x22 };  /* SLA D */
static libspectrum_byte test318_data[] = { 0xcb, 0x2a };  /* SRA D */
static libspectrum_byte test319_data[] = { 0xcb, 0x32 };  /* SLL D (undocumented) */
static libspectrum_byte test320_data[] = { 0xcb, 0x3a };  /* SRL D */
/* E (rrr = 011) */
static libspectrum_byte test321_data[] = { 0xcb, 0x0b };  /* RRC E */
static libspectrum_byte test322_data[] = { 0xcb, 0x13 };  /* RL  E */
static libspectrum_byte test323_data[] = { 0xcb, 0x1b };  /* RR  E */
static libspectrum_byte test324_data[] = { 0xcb, 0x23 };  /* SLA E */
static libspectrum_byte test325_data[] = { 0xcb, 0x2b };  /* SRA E */
static libspectrum_byte test326_data[] = { 0xcb, 0x33 };  /* SLL E (undocumented) */
static libspectrum_byte test327_data[] = { 0xcb, 0x3b };  /* SRL E */
/* H (rrr = 100) */
static libspectrum_byte test328_data[] = { 0xcb, 0x0c };  /* RRC H */
static libspectrum_byte test329_data[] = { 0xcb, 0x14 };  /* RL  H */
static libspectrum_byte test330_data[] = { 0xcb, 0x1c };  /* RR  H */
static libspectrum_byte test331_data[] = { 0xcb, 0x24 };  /* SLA H */
static libspectrum_byte test332_data[] = { 0xcb, 0x2c };  /* SRA H */
static libspectrum_byte test333_data[] = { 0xcb, 0x34 };  /* SLL H (undocumented) */
static libspectrum_byte test334_data[] = { 0xcb, 0x3c };  /* SRL H */
/* L (rrr = 101) */
static libspectrum_byte test335_data[] = { 0xcb, 0x0d };  /* RRC L */
static libspectrum_byte test336_data[] = { 0xcb, 0x15 };  /* RL  L */
static libspectrum_byte test337_data[] = { 0xcb, 0x1d };  /* RR  L */
static libspectrum_byte test338_data[] = { 0xcb, 0x25 };  /* SLA L */
static libspectrum_byte test339_data[] = { 0xcb, 0x2d };  /* SRA L */
static libspectrum_byte test340_data[] = { 0xcb, 0x35 };  /* SLL L (undocumented) */
static libspectrum_byte test341_data[] = { 0xcb, 0x3d };  /* SRL L */


static int
run_test( libspectrum_byte *data, size_t data_length, const char *expected )
{
  char disassembly[DISASM_BUF_LEN];
  size_t length;

  memcpy( memory_map_read[8].page, data, data_length );
  
  debugger_disassemble( disassembly, sizeof( disassembly ), &length, 0x4000 );

  if( strcmp( disassembly, expected ) ) {
    printf( "disassemble test: expected '%s', got '%s'\n", expected,
            disassembly );
    return 1;
  }
  if( length != data_length ) {
    printf( "disassemble test: '%s': expected length %zu, got %zu\n", expected,
            data_length, length );
    return 1;
  }

  return 0;
}

int
debugger_disassemble_unittest( void )
{
  int r = 0;

  r += run_test( test1_data, sizeof( test1_data ), "NOP" );

  r += run_test( test2_data, sizeof( test2_data ), "NOP" );
  r += run_test( test3_data, sizeof( test3_data ), "ADD IX,BC" );
  r += run_test( test4_data, sizeof( test4_data ), "NOP" );
  r += run_test( test5_data, sizeof( test5_data ), "RLC (IX+55)" );

  r += run_test( test6_data, sizeof( test6_data ), "NOP" );
  r += run_test( test7_data, sizeof( test7_data ), "ADD IY,BC" );
  r += run_test( test8_data, sizeof( test8_data ), "NOP" );
  r += run_test( test9_data, sizeof( test9_data ), "RLC (IY+55)" );

  r += run_test( test10_data, sizeof( test10_data ), "ADD IY,BC" );
  r += run_test( test11_data, sizeof( test11_data ), "ADD IX,BC" );

  r += run_test( test12_data, sizeof( test12_data ), "ADD IY,BC" );
  r += run_test( test13_data, sizeof( test13_data ), "ADD IX,BC" );

  r += run_test( test14_data, sizeof( test14_data ), "LD A,(HL)" );
  r += run_test( test15_data, sizeof( test15_data ), "LD A,(IX+55)" );

  /* CB prefix rotation/shift (one per operation) */
  r += run_test( test72_data, sizeof( test72_data ), "RLC B" );
  r += run_test( test73_data, sizeof( test73_data ), "RRC B" );
  r += run_test( test74_data, sizeof( test74_data ), "RL B" );
  r += run_test( test75_data, sizeof( test75_data ), "RR B" );
  r += run_test( test76_data, sizeof( test76_data ), "SLA B" );
  r += run_test( test77_data, sizeof( test77_data ), "SRA B" );
  r += run_test( test78_data, sizeof( test78_data ), "SLL B" );
  r += run_test( test79_data, sizeof( test79_data ), "SRL B" );
  r += run_test( test80_data, sizeof( test80_data ), "RLC (HL)" );

  /* CB prefix BIT/RES/SET */
  r += run_test( test16_data, sizeof( test16_data ), "BIT 0,A" );
  r += run_test( test17_data, sizeof( test17_data ), "RES 0,A" );
  r += run_test( test18_data, sizeof( test18_data ), "SET 1,A" );

  /* CB prefix BIT/RES/SET — remaining registers B, C, D, E, H, L */
  r += run_test( test308_data, sizeof( test308_data ), "BIT 3,B" );
  r += run_test( test309_data, sizeof( test309_data ), "BIT 5,C" );
  r += run_test( test310_data, sizeof( test310_data ), "RES 4,D" );
  r += run_test( test311_data, sizeof( test311_data ), "RES 2,H" );
  r += run_test( test312_data, sizeof( test312_data ), "SET 6,L" );
  r += run_test( test313_data, sizeof( test313_data ), "SET 3,E" );

  /* DD CB prefix BIT/RES/SET on (IX+d) */
  r += run_test( test19_data, sizeof( test19_data ), "BIT 0,(IX+55)" );
  r += run_test( test20_data, sizeof( test20_data ), "RES 0,(IX+55)" );
  r += run_test( test21_data, sizeof( test21_data ), "SET 0,(IX+55)" );

  /* FD CB prefix BIT/RES/SET on (IY+d) */
  r += run_test( test22_data, sizeof( test22_data ), "BIT 0,(IY+55)" );
  r += run_test( test25_data, sizeof( test25_data ), "RES 0,(IY+55)" );
  r += run_test( test26_data, sizeof( test26_data ), "SET 0,(IY+55)" );

  /* DD CB undocumented LD reg,RES/SET n,(IX+d) — regression for bug #515 */
  r += run_test( test23_data, sizeof( test23_data ), "LD A,RES 0,(IX+55)" );
  r += run_test( test24_data, sizeof( test24_data ), "LD A,SET 1,(IX+55)" );

  /* FD CB undocumented LD reg,RES/SET n,(IY+d) */
  r += run_test( test27_data, sizeof( test27_data ), "LD A,RES 0,(IY+55)" );
  r += run_test( test28_data, sizeof( test28_data ), "LD A,SET 1,(IY+55)" );

  /* DDCB/FDCB: BIT 7, RES 7, SET 7 — exercises the full 0-7 bit range */
  r += run_test( testbit7a_data, sizeof( testbit7a_data ), "BIT 7,(IX+55)" );
  r += run_test( testbit7b_data, sizeof( testbit7b_data ), "RES 7,(IX+55)" );
  r += run_test( testbit7c_data, sizeof( testbit7c_data ), "SET 7,(IX+55)" );
  r += run_test( testbit7d_data, sizeof( testbit7d_data ), "BIT 7,(IY+55)" );
  r += run_test( testbit7e_data, sizeof( testbit7e_data ), "RES 7,(IY+55)" );
  r += run_test( testbit7f_data, sizeof( testbit7f_data ), "SET 7,(IY+55)" );

  /* Negative (IX+d)/(IY+d) offsets */
  r += run_test( test29_data, sizeof( test29_data ), "BIT 0,(IX-01)" );
  r += run_test( test30_data, sizeof( test30_data ), "BIT 0,(IY-01)" );
  r += run_test( test31_data, sizeof( test31_data ), "LD A,(IX-01)" );

  /* Relative jump instructions */
  r += run_test( test32_data, sizeof( test32_data ), "JR 4002" );
  r += run_test( test33_data, sizeof( test33_data ), "JR 4000" );
  r += run_test( test34_data, sizeof( test34_data ), "DJNZ 4000" );
  r += run_test( test35_data, sizeof( test35_data ), "JR NZ,4006" );

  /* ED prefix: IN reg,(C) and OUT (C),reg */
  r += run_test( test36_data, sizeof( test36_data ), "IN B,(C)" );
  r += run_test( test37_data, sizeof( test37_data ), "IN F,(C)" );
  r += run_test( test38_data, sizeof( test38_data ), "OUT (C),B" );
  r += run_test( test39_data, sizeof( test39_data ), "OUT (C),0" );

  /* ED prefix: SBC HL,rr and ADC HL,rr — all register pairs */
  r += run_test( test40_data, sizeof( test40_data ), "SBC HL,BC" );
  r += run_test( test41_data, sizeof( test41_data ), "ADC HL,BC" );
  r += run_test( test81_data, sizeof( test81_data ), "SBC HL,DE" );
  r += run_test( test82_data, sizeof( test82_data ), "ADC HL,DE" );
  r += run_test( test83_data, sizeof( test83_data ), "SBC HL,HL" );
  r += run_test( test84_data, sizeof( test84_data ), "ADC HL,HL" );
  r += run_test( test85_data, sizeof( test85_data ), "SBC HL,SP" );
  r += run_test( test86_data, sizeof( test86_data ), "ADC HL,SP" );

  /* ED prefix: LD (nn),rr and LD rr,(nn) — all register pairs */
  r += run_test( test42_data, sizeof( test42_data ), "LD (3456),BC" );
  r += run_test( test43_data, sizeof( test43_data ), "LD BC,(3456)" );
  r += run_test( test87_data, sizeof( test87_data ), "LD (3456),DE" );
  r += run_test( test88_data, sizeof( test88_data ), "LD DE,(3456)" );
  r += run_test( test89_data, sizeof( test89_data ), "LD (3456),HL" );
  r += run_test( test90_data, sizeof( test90_data ), "LD HL,(3456)" );
  r += run_test( test91_data, sizeof( test91_data ), "LD (3456),SP" );
  r += run_test( test92_data, sizeof( test92_data ), "LD SP,(3456)" );

  /* ED prefix: NEG, RETN, RETI */
  r += run_test( test44_data, sizeof( test44_data ), "NEG" );
  r += run_test( test45_data, sizeof( test45_data ), "RETN" );
  r += run_test( test46_data, sizeof( test46_data ), "RETI" );

  /* ED prefix: IM 0, IM 1, IM 2 */
  r += run_test( test47_data, sizeof( test47_data ), "IM 0" );
  r += run_test( test48_data, sizeof( test48_data ), "IM 1" );
  r += run_test( test49_data, sizeof( test49_data ), "IM 2" );

  /* ED prefix: LD I,A, LD R,A, LD A,I, LD A,R, RRD, RLD */
  r += run_test( test50_data, sizeof( test50_data ), "LD I,A" );
  r += run_test( test51_data, sizeof( test51_data ), "LD A,I" );
  r += run_test( test56_data, sizeof( test56_data ), "LD R,A" );
  r += run_test( test57_data, sizeof( test57_data ), "LD A,R" );
  r += run_test( test58_data, sizeof( test58_data ), "RRD" );
  r += run_test( test59_data, sizeof( test59_data ), "RLD" );

  /* ED prefix: block instructions */
  r += run_test( test52_data, sizeof( test52_data ), "LDI" );
  r += run_test( test53_data, sizeof( test53_data ), "LDIR" );
  r += run_test( test54_data, sizeof( test54_data ), "LDD" );
  r += run_test( test55_data, sizeof( test55_data ), "LDDR" );
  r += run_test( test60_data, sizeof( test60_data ), "CPI" );
  r += run_test( test61_data, sizeof( test61_data ), "CPD" );
  r += run_test( test62_data, sizeof( test62_data ), "CPIR" );
  r += run_test( test63_data, sizeof( test63_data ), "CPDR" );
  r += run_test( test64_data, sizeof( test64_data ), "INI" );
  r += run_test( test65_data, sizeof( test65_data ), "IND" );
  r += run_test( test66_data, sizeof( test66_data ), "INIR" );
  r += run_test( test67_data, sizeof( test67_data ), "INDR" );
  r += run_test( test68_data, sizeof( test68_data ), "OUTI" );
  r += run_test( test69_data, sizeof( test69_data ), "OUTD" );
  r += run_test( test70_data, sizeof( test70_data ), "OTIR" );
  r += run_test( test71_data, sizeof( test71_data ), "OTDR" );

  /* DD prefix: ADD IX,rr completeness */
  r += run_test( test93_data, sizeof( test93_data ), "ADD IX,DE" );
  r += run_test( test94_data, sizeof( test94_data ), "ADD IX,IX" );
  r += run_test( test95_data, sizeof( test95_data ), "ADD IX,SP" );

  /* FD prefix: ADD IY,rr completeness */
  r += run_test( test96_data, sizeof( test96_data ), "ADD IY,DE" );
  r += run_test( test97_data, sizeof( test97_data ), "ADD IY,IY" );
  r += run_test( test98_data, sizeof( test98_data ), "ADD IY,SP" );

  /* DD/FD prefix: LD IX/IY,nn */
  r += run_test( test99_data,  sizeof( test99_data ),  "LD IX,1234" );
  r += run_test( test100_data, sizeof( test100_data ), "LD IY,1234" );

  /* DD/FD prefix: INC/DEC IX/IY */
  r += run_test( test101_data, sizeof( test101_data ), "INC IX" );
  r += run_test( test102_data, sizeof( test102_data ), "DEC IX" );
  r += run_test( test103_data, sizeof( test103_data ), "INC IY" );
  r += run_test( test104_data, sizeof( test104_data ), "DEC IY" );

  /* DD/FD prefix: LD (nn),IX/IY and LD IX/IY,(nn) */
  r += run_test( test105_data, sizeof( test105_data ), "LD (1234),IX" );
  r += run_test( test106_data, sizeof( test106_data ), "LD IX,(1234)" );
  r += run_test( test107_data, sizeof( test107_data ), "LD (1234),IY" );
  r += run_test( test108_data, sizeof( test108_data ), "LD IY,(1234)" );

  /* DD/FD prefix: PUSH/POP IX/IY */
  r += run_test( test109_data, sizeof( test109_data ), "PUSH IX" );
  r += run_test( test110_data, sizeof( test110_data ), "POP IX" );
  r += run_test( test111_data, sizeof( test111_data ), "PUSH IY" );
  r += run_test( test112_data, sizeof( test112_data ), "POP IY" );

  /* DD/FD prefix: JP (IX/IY), EX (SP),IX/IY, LD SP,IX/IY */
  r += run_test( test113_data, sizeof( test113_data ), "JP (IX)" );
  r += run_test( test114_data, sizeof( test114_data ), "EX (SP),IX" );
  r += run_test( test115_data, sizeof( test115_data ), "LD SP,IX" );
  r += run_test( test116_data, sizeof( test116_data ), "JP (IY)" );
  r += run_test( test117_data, sizeof( test117_data ), "EX (SP),IY" );
  r += run_test( test118_data, sizeof( test118_data ), "LD SP,IY" );

  /* DD/FD prefix: undocumented IXh, IXl, IYh, IYl registers */
  r += run_test( test119_data, sizeof( test119_data ), "LD A,IXh" );
  r += run_test( test120_data, sizeof( test120_data ), "LD A,IXl" );
  r += run_test( test121_data, sizeof( test121_data ), "LD A,IYh" );
  r += run_test( test122_data, sizeof( test122_data ), "LD A,IYl" );

  /* DD/FD prefix: LD r,(IX/IY+d) */
  r += run_test( test123_data, sizeof( test123_data ), "LD B,(IX+05)" );
  r += run_test( test124_data, sizeof( test124_data ), "LD B,(IY+05)" );

  /* DD/FD prefix: LD (IX/IY+d),r */
  r += run_test( test125_data, sizeof( test125_data ), "LD (IX+05),B" );
  r += run_test( test126_data, sizeof( test126_data ), "LD (IY+05),B" );

  /* DD/FD prefix: LD (IX/IY+d),n */
  r += run_test( test127_data, sizeof( test127_data ), "LD (IX+05),07" );
  r += run_test( test128_data, sizeof( test128_data ), "LD (IY+05),07" );

  /* Main (unprefixed) opcodes */

  /* 00xxx000 group: NOP already covered by test1; EX AF,AF' */
  r += run_test( test129_data, sizeof( test129_data ), "EX AF,AF'" );

  /* 00xxx111 group: rotate/accumulator flags */
  r += run_test( test130_data, sizeof( test130_data ), "RLCA" );
  r += run_test( test131_data, sizeof( test131_data ), "RRCA" );
  r += run_test( test132_data, sizeof( test132_data ), "RLA" );
  r += run_test( test133_data, sizeof( test133_data ), "RRA" );
  r += run_test( test134_data, sizeof( test134_data ), "DAA" );
  r += run_test( test135_data, sizeof( test135_data ), "CPL" );
  r += run_test( test136_data, sizeof( test136_data ), "SCF" );
  r += run_test( test137_data, sizeof( test137_data ), "CCF" );

  /* 00xxx001: LD rr,nn; 00xxx011/00xxx1011: INC/DEC rr; ADD HL,rr */
  r += run_test( test138_data, sizeof( test138_data ), "LD BC,1234" );
  r += run_test( test139_data, sizeof( test139_data ), "INC BC" );
  r += run_test( test140_data, sizeof( test140_data ), "DEC BC" );
  r += run_test( test141_data, sizeof( test141_data ), "ADD HL,DE" );

  /* ADD HL,rr — remaining three register pairs */
  r += run_test( test352_data, sizeof( test352_data ), "ADD HL,BC" );
  r += run_test( test353_data, sizeof( test353_data ), "ADD HL,HL" );
  r += run_test( test354_data, sizeof( test354_data ), "ADD HL,SP" );

  /* INC/DEC rr — remaining three register pairs (DE, HL, SP) */
  r += run_test( test355_data, sizeof( test355_data ), "INC DE" );
  r += run_test( test356_data, sizeof( test356_data ), "INC HL" );
  r += run_test( test357_data, sizeof( test357_data ), "INC SP" );
  r += run_test( test358_data, sizeof( test358_data ), "DEC DE" );
  r += run_test( test359_data, sizeof( test359_data ), "DEC HL" );
  r += run_test( test360_data, sizeof( test360_data ), "DEC SP" );

  /* 00xxx100/101/110: INC r, DEC r, LD r,n */
  r += run_test( test142_data, sizeof( test142_data ), "INC B" );
  r += run_test( test143_data, sizeof( test143_data ), "DEC B" );
  r += run_test( test144_data, sizeof( test144_data ), "LD B,07" );

  /* 00xxx010: LD (rr),A / LD (nn),HL / LD (nn),A */
  r += run_test( test145_data, sizeof( test145_data ), "LD (BC),A" );
  r += run_test( test146_data, sizeof( test146_data ), "LD (DE),A" );
  r += run_test( test147_data, sizeof( test147_data ), "LD (1234),HL" );
  r += run_test( test148_data, sizeof( test148_data ), "LD (1234),A" );

  /* 00xxx110 (0x0a column): LD A,(rr) / LD HL,(nn) / LD A,(nn) */
  r += run_test( test149_data, sizeof( test149_data ), "LD A,(BC)" );
  r += run_test( test150_data, sizeof( test150_data ), "LD A,(DE)" );
  r += run_test( test151_data, sizeof( test151_data ), "LD HL,(1234)" );
  r += run_test( test152_data, sizeof( test152_data ), "LD A,(1234)" );

  /* 01xxxxxx: LD r,r' and HALT */
  r += run_test( test153_data, sizeof( test153_data ), "HALT" );
  r += run_test( test154_data, sizeof( test154_data ), "LD B,C" );

  /* 10xxxxxx: arithmetic/logic on registers */
  r += run_test( test155_data, sizeof( test155_data ), "ADD A,B" );
  r += run_test( test156_data, sizeof( test156_data ), "SUB C" );
  r += run_test( test157_data, sizeof( test157_data ), "AND D" );
  r += run_test( test158_data, sizeof( test158_data ), "XOR E" );
  r += run_test( test159_data, sizeof( test159_data ), "OR H" );
  r += run_test( test160_data, sizeof( test160_data ), "CP L" );

  /* 10xxxxxx: ADC A,r and SBC A,r — register forms (distinct from immediate) */
  r += run_test( test342_data, sizeof( test342_data ), "ADC A,B" );
  r += run_test( test343_data, sizeof( test343_data ), "SBC A,C" );

  /* 10xxxxxx: arithmetic/logic with (HL) operand */
  r += run_test( test344_data, sizeof( test344_data ), "ADD A,(HL)" );
  r += run_test( test345_data, sizeof( test345_data ), "ADC A,(HL)" );
  r += run_test( test346_data, sizeof( test346_data ), "SUB (HL)" );
  r += run_test( test347_data, sizeof( test347_data ), "SBC A,(HL)" );
  r += run_test( test348_data, sizeof( test348_data ), "AND (HL)" );
  r += run_test( test349_data, sizeof( test349_data ), "XOR (HL)" );
  r += run_test( test350_data, sizeof( test350_data ), "OR (HL)" );
  r += run_test( test351_data, sizeof( test351_data ), "CP (HL)" );

  /* 11xxx110: immediate arithmetic */
  r += run_test( test161_data, sizeof( test161_data ), "ADD A,07" );

  /* 11xxx000: conditional RET */
  r += run_test( test162_data, sizeof( test162_data ), "RET NZ" );

  /* 11xxx001: POP, RET, EXX, JP(HL), POP AF, LD SP,HL */
  r += run_test( test163_data, sizeof( test163_data ), "POP BC" );
  r += run_test( test164_data, sizeof( test164_data ), "RET" );
  r += run_test( test165_data, sizeof( test165_data ), "EXX" );
  r += run_test( test166_data, sizeof( test166_data ), "JP (HL)" );
  r += run_test( test167_data, sizeof( test167_data ), "POP AF" );
  r += run_test( test168_data, sizeof( test168_data ), "LD SP,HL" );

  /* 11xxx010: conditional JP; 11xxx011: JP/OUT/IN/EX/DI/EI */
  r += run_test( test169_data, sizeof( test169_data ), "JP NZ,1234" );
  r += run_test( test170_data, sizeof( test170_data ), "JP 1234" );
  r += run_test( test171_data, sizeof( test171_data ), "OUT (07),A" );
  r += run_test( test172_data, sizeof( test172_data ), "IN A,(07)" );
  r += run_test( test173_data, sizeof( test173_data ), "EX (SP),HL" );
  r += run_test( test174_data, sizeof( test174_data ), "EX DE,HL" );
  r += run_test( test175_data, sizeof( test175_data ), "DI" );
  r += run_test( test176_data, sizeof( test176_data ), "EI" );

  /* 11xxx100: conditional CALL; 11xxx101: PUSH/CALL */
  r += run_test( test177_data, sizeof( test177_data ), "CALL NZ,1234" );
  r += run_test( test178_data, sizeof( test178_data ), "PUSH BC" );
  r += run_test( test179_data, sizeof( test179_data ), "CALL 1234" );
  r += run_test( test180_data, sizeof( test180_data ), "PUSH AF" );

  /* 11xxx111: RST */
  r += run_test( test181_data, sizeof( test181_data ), "RST 0" );
  r += run_test( test182_data, sizeof( test182_data ), "RST 28" );

  r += debugger_disassemble_decimal_unittest();

  /* Conditional JR — remaining conditions */
  r += run_test( test189_data, sizeof( test189_data ), "JR Z,4006" );
  r += run_test( test190_data, sizeof( test190_data ), "JR NC,4006" );
  r += run_test( test191_data, sizeof( test191_data ), "JR C,4006" );

  /* Conditional RET — remaining seven conditions */
  r += run_test( test192_data, sizeof( test192_data ), "RET Z" );
  r += run_test( test193_data, sizeof( test193_data ), "RET NC" );
  r += run_test( test194_data, sizeof( test194_data ), "RET C" );
  r += run_test( test195_data, sizeof( test195_data ), "RET PO" );
  r += run_test( test196_data, sizeof( test196_data ), "RET PE" );
  r += run_test( test197_data, sizeof( test197_data ), "RET P" );
  r += run_test( test198_data, sizeof( test198_data ), "RET M" );

  /* Conditional JP — remaining seven conditions */
  r += run_test( test199_data, sizeof( test199_data ), "JP Z,1234" );
  r += run_test( test200_data, sizeof( test200_data ), "JP NC,1234" );
  r += run_test( test201_data, sizeof( test201_data ), "JP C,1234" );
  r += run_test( test202_data, sizeof( test202_data ), "JP PO,1234" );
  r += run_test( test203_data, sizeof( test203_data ), "JP PE,1234" );
  r += run_test( test204_data, sizeof( test204_data ), "JP P,1234" );
  r += run_test( test205_data, sizeof( test205_data ), "JP M,1234" );

  /* Conditional CALL — remaining seven conditions */
  r += run_test( test206_data, sizeof( test206_data ), "CALL Z,1234" );
  r += run_test( test207_data, sizeof( test207_data ), "CALL NC,1234" );
  r += run_test( test208_data, sizeof( test208_data ), "CALL C,1234" );
  r += run_test( test209_data, sizeof( test209_data ), "CALL PO,1234" );
  r += run_test( test210_data, sizeof( test210_data ), "CALL PE,1234" );
  r += run_test( test211_data, sizeof( test211_data ), "CALL P,1234" );
  r += run_test( test212_data, sizeof( test212_data ), "CALL M,1234" );

  /* RST — remaining six targets */
  r += run_test( test213_data, sizeof( test213_data ), "RST 8" );
  r += run_test( test214_data, sizeof( test214_data ), "RST 10" );
  r += run_test( test215_data, sizeof( test215_data ), "RST 18" );
  r += run_test( test216_data, sizeof( test216_data ), "RST 20" );
  r += run_test( test217_data, sizeof( test217_data ), "RST 30" );
  r += run_test( test218_data, sizeof( test218_data ), "RST 38" );

  /* Immediate arithmetic/logic — remaining seven operations */
  r += run_test( test219_data, sizeof( test219_data ), "ADC A,07" );
  r += run_test( test220_data, sizeof( test220_data ), "SUB 07" );
  r += run_test( test221_data, sizeof( test221_data ), "SBC A,07" );
  r += run_test( test222_data, sizeof( test222_data ), "AND 07" );
  r += run_test( test223_data, sizeof( test223_data ), "XOR 07" );
  r += run_test( test224_data, sizeof( test224_data ), "OR 07" );
  r += run_test( test225_data, sizeof( test225_data ), "CP 07" );

  /* DD CB prefix: remaining documented rotate/shift ops on (IX+d) */
  r += run_test( test226_data, sizeof( test226_data ), "RRC (IX+55)" );
  r += run_test( test227_data, sizeof( test227_data ), "RL (IX+55)" );
  r += run_test( test228_data, sizeof( test228_data ), "RR (IX+55)" );
  r += run_test( test229_data, sizeof( test229_data ), "SLA (IX+55)" );
  r += run_test( test230_data, sizeof( test230_data ), "SRA (IX+55)" );
  r += run_test( test231_data, sizeof( test231_data ), "SLL (IX+55)" );
  r += run_test( test232_data, sizeof( test232_data ), "SRL (IX+55)" );

  /* FD CB prefix: remaining documented rotate/shift ops on (IY+d) */
  r += run_test( test233_data, sizeof( test233_data ), "RRC (IY+55)" );
  r += run_test( test234_data, sizeof( test234_data ), "RL (IY+55)" );
  r += run_test( test235_data, sizeof( test235_data ), "RR (IY+55)" );
  r += run_test( test236_data, sizeof( test236_data ), "SLA (IY+55)" );
  r += run_test( test237_data, sizeof( test237_data ), "SRA (IY+55)" );
  r += run_test( test238_data, sizeof( test238_data ), "SLL (IY+55)" );
  r += run_test( test239_data, sizeof( test239_data ), "SRL (IY+55)" );

  /* DD CB / FD CB undocumented: LD reg,rotate (IX/IY+d) */
  r += run_test( test240_data, sizeof( test240_data ), "LD B,RLC (IX+55)" );
  r += run_test( test241_data, sizeof( test241_data ), "LD B,RLC (IY+55)" );

  /* DD CB: remaining destination registers — C, D, E, H, L, A */
  r += run_test( test242_data, sizeof( test242_data ), "LD C,RLC (IX+55)" );
  r += run_test( test243_data, sizeof( test243_data ), "LD D,RLC (IX+55)" );
  r += run_test( test244_data, sizeof( test244_data ), "LD E,RLC (IX+55)" );
  r += run_test( test245_data, sizeof( test245_data ), "LD H,RLC (IX+55)" );
  r += run_test( test246_data, sizeof( test246_data ), "LD L,RLC (IX+55)" );
  r += run_test( test247_data, sizeof( test247_data ), "LD A,RLC (IX+55)" );

  /* FD CB: remaining destination registers — C, D, E, H, L, A */
  r += run_test( test248_data, sizeof( test248_data ), "LD C,RLC (IY+55)" );
  r += run_test( test249_data, sizeof( test249_data ), "LD D,RLC (IY+55)" );
  r += run_test( test250_data, sizeof( test250_data ), "LD E,RLC (IY+55)" );
  r += run_test( test251_data, sizeof( test251_data ), "LD H,RLC (IY+55)" );
  r += run_test( test252_data, sizeof( test252_data ), "LD L,RLC (IY+55)" );
  r += run_test( test253_data, sizeof( test253_data ), "LD A,RLC (IY+55)" );

  /* DD CB: verify rotation-op selection (SRL) independent of destination */
  r += run_test( test254_data, sizeof( test254_data ), "LD B,SRL (IX+55)" );

  /* DD/FD prefix: LD r,(IX/IY+d) — remaining registers C, D, E, H, L */
  r += run_test( test255_data, sizeof( test255_data ), "LD C,(IX+05)" );
  r += run_test( test256_data, sizeof( test256_data ), "LD D,(IX+05)" );
  r += run_test( test257_data, sizeof( test257_data ), "LD E,(IX+05)" );
  r += run_test( test258_data, sizeof( test258_data ), "LD H,(IX+05)" );
  r += run_test( test259_data, sizeof( test259_data ), "LD L,(IX+05)" );
  r += run_test( test260_data, sizeof( test260_data ), "LD C,(IY+05)" );
  r += run_test( test261_data, sizeof( test261_data ), "LD D,(IY+05)" );
  r += run_test( test262_data, sizeof( test262_data ), "LD E,(IY+05)" );
  r += run_test( test263_data, sizeof( test263_data ), "LD H,(IY+05)" );
  r += run_test( test264_data, sizeof( test264_data ), "LD L,(IY+05)" );
  r += run_test( test265_data, sizeof( test265_data ), "LD A,(IY+05)" );

  /* DD/FD prefix: LD (IX/IY+d),r — remaining registers C, D, E, H, L, A */
  r += run_test( test266_data, sizeof( test266_data ), "LD (IX+05),C" );
  r += run_test( test267_data, sizeof( test267_data ), "LD (IX+05),D" );
  r += run_test( test268_data, sizeof( test268_data ), "LD (IX+05),E" );
  r += run_test( test269_data, sizeof( test269_data ), "LD (IX+05),H" );
  r += run_test( test270_data, sizeof( test270_data ), "LD (IX+05),L" );
  r += run_test( test271_data, sizeof( test271_data ), "LD (IX+05),A" );
  r += run_test( test272_data, sizeof( test272_data ), "LD (IY+05),C" );
  r += run_test( test273_data, sizeof( test273_data ), "LD (IY+05),D" );
  r += run_test( test274_data, sizeof( test274_data ), "LD (IY+05),E" );
  r += run_test( test275_data, sizeof( test275_data ), "LD (IY+05),H" );
  r += run_test( test276_data, sizeof( test276_data ), "LD (IY+05),L" );
  r += run_test( test277_data, sizeof( test277_data ), "LD (IY+05),A" );

  /* CB prefix: all rotate/shift ops with register A */
  r += run_test( test278_data, sizeof( test278_data ), "RLC A" );
  r += run_test( test279_data, sizeof( test279_data ), "RRC A" );
  r += run_test( test280_data, sizeof( test280_data ), "RL A" );
  r += run_test( test281_data, sizeof( test281_data ), "RR A" );
  r += run_test( test282_data, sizeof( test282_data ), "SLA A" );
  r += run_test( test283_data, sizeof( test283_data ), "SRA A" );
  r += run_test( test284_data, sizeof( test284_data ), "SLL A" );
  r += run_test( test285_data, sizeof( test285_data ), "SRL A" );

  /* CB prefix: remaining rotate/shift ops with (HL) */
  r += run_test( test286_data, sizeof( test286_data ), "RRC (HL)" );
  r += run_test( test287_data, sizeof( test287_data ), "RL (HL)" );
  r += run_test( test288_data, sizeof( test288_data ), "RR (HL)" );
  r += run_test( test289_data, sizeof( test289_data ), "SLA (HL)" );
  r += run_test( test290_data, sizeof( test290_data ), "SRA (HL)" );
  r += run_test( test291_data, sizeof( test291_data ), "SLL (HL)" );
  r += run_test( test292_data, sizeof( test292_data ), "SRL (HL)" );

  /* CB prefix: BIT, RES, SET with bit 7 and (HL) */
  r += run_test( test293_data, sizeof( test293_data ), "BIT 7,(HL)" );
  r += run_test( test294_data, sizeof( test294_data ), "RES 7,(HL)" );
  r += run_test( test295_data, sizeof( test295_data ), "SET 7,(HL)" );

  /* CB prefix: BIT, RES, SET with bit 0 and (HL) */
  r += run_test( test361_data, sizeof( test361_data ), "BIT 0,(HL)" );
  r += run_test( test362_data, sizeof( test362_data ), "RES 0,(HL)" );
  r += run_test( test363_data, sizeof( test363_data ), "SET 0,(HL)" );

  /* CB prefix: all rotation/shift ops for register C (rrr = 001) */
  r += run_test( test296_data, sizeof( test296_data ), "RLC C" );
  r += run_test( test297_data, sizeof( test297_data ), "RRC C" );
  r += run_test( test298_data, sizeof( test298_data ), "RL C" );
  r += run_test( test299_data, sizeof( test299_data ), "RR C" );
  r += run_test( test300_data, sizeof( test300_data ), "SLA C" );
  r += run_test( test301_data, sizeof( test301_data ), "SRA C" );
  r += run_test( test302_data, sizeof( test302_data ), "SLL C" );
  r += run_test( test303_data, sizeof( test303_data ), "SRL C" );

  /* CB prefix: RLC for registers D, E, H, L (rrr = 010, 011, 100, 101) */
  r += run_test( test304_data, sizeof( test304_data ), "RLC D" );
  r += run_test( test305_data, sizeof( test305_data ), "RLC E" );
  r += run_test( test306_data, sizeof( test306_data ), "RLC H" );
  r += run_test( test307_data, sizeof( test307_data ), "RLC L" );

  /* CB prefix: remaining rotation/shift ops for D (rrr = 010) */
  r += run_test( test314_data, sizeof( test314_data ), "RRC D" );
  r += run_test( test315_data, sizeof( test315_data ), "RL D" );
  r += run_test( test316_data, sizeof( test316_data ), "RR D" );
  r += run_test( test317_data, sizeof( test317_data ), "SLA D" );
  r += run_test( test318_data, sizeof( test318_data ), "SRA D" );
  r += run_test( test319_data, sizeof( test319_data ), "SLL D" );
  r += run_test( test320_data, sizeof( test320_data ), "SRL D" );

  /* CB prefix: remaining rotation/shift ops for E (rrr = 011) */
  r += run_test( test321_data, sizeof( test321_data ), "RRC E" );
  r += run_test( test322_data, sizeof( test322_data ), "RL E" );
  r += run_test( test323_data, sizeof( test323_data ), "RR E" );
  r += run_test( test324_data, sizeof( test324_data ), "SLA E" );
  r += run_test( test325_data, sizeof( test325_data ), "SRA E" );
  r += run_test( test326_data, sizeof( test326_data ), "SLL E" );
  r += run_test( test327_data, sizeof( test327_data ), "SRL E" );

  /* CB prefix: remaining rotation/shift ops for H (rrr = 100) */
  r += run_test( test328_data, sizeof( test328_data ), "RRC H" );
  r += run_test( test329_data, sizeof( test329_data ), "RL H" );
  r += run_test( test330_data, sizeof( test330_data ), "RR H" );
  r += run_test( test331_data, sizeof( test331_data ), "SLA H" );
  r += run_test( test332_data, sizeof( test332_data ), "SRA H" );
  r += run_test( test333_data, sizeof( test333_data ), "SLL H" );
  r += run_test( test334_data, sizeof( test334_data ), "SRL H" );

  /* CB prefix: remaining rotation/shift ops for L (rrr = 101) */
  r += run_test( test335_data, sizeof( test335_data ), "RRC L" );
  r += run_test( test336_data, sizeof( test336_data ), "RL L" );
  r += run_test( test337_data, sizeof( test337_data ), "RR L" );
  r += run_test( test338_data, sizeof( test338_data ), "SLA L" );
  r += run_test( test339_data, sizeof( test339_data ), "SRA L" );
  r += run_test( test340_data, sizeof( test340_data ), "SLL L" );
  r += run_test( test341_data, sizeof( test341_data ), "SRL L" );

  return r;
}

/* Tests for decimal output mode (debugger_output_base == 10).
   Each test exercises a distinct numeric-formatting code path. */
int
debugger_disassemble_decimal_unittest( void )
{
  int r = 0;
  int saved_base = debugger_output_base;

  debugger_output_base = 10;

  /* get_byte() via 00xxx110 path: LD B,07h -> "LD B,7" */
  r += run_test( testd1_data, sizeof( testd1_data ), "LD B,7" );

  /* get_word() via 00xxx001 path: LD BC,1234h -> "LD BC,4660" */
  r += run_test( testd2_data, sizeof( testd2_data ), "LD BC,4660" );

  /* get_offset() via JR: offset 0 from 4000h -> 4002h = 16386 decimal */
  r += run_test( testd3_data, sizeof( testd3_data ), "JR 16386" );

  /* ix_iy_offset() positive: LD A,(IX+55h) -> "LD A,(IX+85)" */
  r += run_test( testd4_data, sizeof( testd4_data ), "LD A,(IX+85)" );

  /* ix_iy_offset() negative: BIT 0,(IX-01h) -> "BIT 0,(IX-1)" */
  r += run_test( testd5_data, sizeof( testd5_data ), "BIT 0,(IX-1)" );

  /* get_byte() via 11xxx110 path: ADD A,07h -> "ADD A,7" */
  r += run_test( testd6_data, sizeof( testd6_data ), "ADD A,7" );

  /* ix_iy_offset() positive with IY: LD A,(IY+55h) -> "LD A,(IY+85)" */
  r += run_test( testd7_data, sizeof( testd7_data ), "LD A,(IY+85)" );

  /* ix_iy_offset() negative with IY: BIT 0,(IY-01h) -> "BIT 0,(IY-1)" */
  r += run_test( testd8_data, sizeof( testd8_data ), "BIT 0,(IY-1)" );

  debugger_output_base = saved_base;

  /* ED prefix: NOPD boundary — b < 0x40 (lower range) and b > 0xbb (upper range) */
  r += run_test( test183_data, sizeof( test183_data ), "NOPD" );
  r += run_test( test184_data, sizeof( test184_data ), "NOPD" );
  /* ED prefix: NOPD mid-range 0x80–0x9f */
  r += run_test( test185_data, sizeof( test185_data ), "NOPD" );
  r += run_test( test186_data, sizeof( test186_data ), "NOPD" );
  /* ED prefix: NOPD upper range 0xbc–0xff */
  r += run_test( test187_data, sizeof( test187_data ), "NOPD" );
  r += run_test( test188_data, sizeof( test188_data ), "NOPD" );

  return r;
}
