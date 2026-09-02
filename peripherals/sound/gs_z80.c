/* gs_z80.c: the General Sound card's Z80
   Copyright (c) 2026 The FuseX authors

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

   The card carries a second Z80, so this file compiles the emulator's own
   opcode tables a second time against a private register file, clock and
   memory map. Everything the generated tables reach for is redefined
   below before they are included; the instruction semantics are shared
   with the host CPU rather than reimplemented.

*/

#include "config.h"

#include "libspectrum.h"

#include "compat.h"
#include "general_sound.h"
#include "gs_z80.h"
#include "z80/z80.h"

/* --- the card's own state ------------------------------------------- */

static processor gs_z80_cpu;
libspectrum_dword gs_z80_tstates;
libspectrum_qword gs_z80_period_base;

static libspectrum_byte gs_z80_readbyte( libspectrum_word address );
static libspectrum_byte gs_z80_readbyte_timed( libspectrum_word address );
static void gs_z80_writebyte( libspectrum_word address, libspectrum_byte b );
static void gs_z80_writebyte_timed( libspectrum_word address,
				    libspectrum_byte b );
static libspectrum_byte gs_z80_readport( libspectrum_word port );
static void gs_z80_writeport( libspectrum_word port, libspectrum_byte b );
static void gs_z80_step( void );
static int gs_z80_interrupt( void );

/* --- what the generated tables expect ------------------------------- */

/* The register macros in z80_macros.h all reach through a variable called
   `z80`; point them at the card's register file instead. */
#define z80     gs_z80_cpu
#define tstates gs_z80_tstates

/* The generated tables only carry the CB/DD/ED/FD/DDCB opcodes inline
   under HAVE_ENOUGH_MEMORY; the out-of-line dispatchers they call
   otherwise are static to z80_ops.c. The card's core always takes the
   inline form, whatever the rest of the build was configured for. */
#undef HAVE_ENOUGH_MEMORY
#define HAVE_ENOUGH_MEMORY 1

#include "z80/z80_macros.h"

/* The card's memory is not contended, so the timing macros only need to
   charge the T-states. */
#undef contend_read
#undef contend_read_no_mreq
#undef contend_write_no_mreq
#define contend_read(address,time)          tstates += (time);
#define contend_read_no_mreq(address,time)  tstates += (time);
#define contend_write_no_mreq(address,time) tstates += (time);

/* The card's Z80 is an NMOS part. */
#undef IS_CMOS
#define IS_CMOS 0

/* As on the host: the plain forms charge the three T-states of the bus
   cycle, the _internal forms are the untimed accessors used for the M1
   fetch. */
#define readbyte(address)             gs_z80_readbyte_timed( address )
#define readbyte_internal(address)    gs_z80_readbyte( address )
#define writebyte(address,b)          gs_z80_writebyte_timed( (address), (b) )
#define writebyte_internal(address,b) gs_z80_writebyte( (address), (b) )

/* An uncontended I/O cycle: one T-state before the access and three
   after, as on the host. The card's port map itself lives with the card. */
#define readport(port)      gs_z80_readport( port )
#define writeport(port,b)   gs_z80_writeport( (port), (b) )

/* The host's tape and SLT traps key off addresses in the Spectrum ROM and
   have no meaning here; returning non-zero lets the opcode run normally. */
#define tape_save_trap()    1
#define tape_load_trap()    1
#define slt_trap(addr,level) do { (void)(addr); (void)(level); } while( 0 )

/* The card schedules nothing through the host's event queue. EI still
   records interrupts_enabled_at, which is what suppresses an interrupt
   immediately after it. */
#define event_add(time,type) do { (void)(time); (void)(type); } while( 0 )

/* The opcode itself restores IFF1 from IFF2; the card has nothing further
   to do on a return from NMI. */
#define z80_retn() do { } while( 0 )

/* Only the host CPU's instruction count feeds RZX. */
static int rzx_instructions_offset;

/* --- memory ---------------------------------------------------------- */

static libspectrum_byte
gs_z80_readbyte( libspectrum_word address )
{
  libspectrum_byte b = general_sound_bank_read[ address >> 14 ][ address & 0x3fff ];

  if( ( address & 0xe000 ) == 0x6000 ) general_sound_dac_read( address, b );

  return b;
}

static void
gs_z80_writebyte( libspectrum_word address, libspectrum_byte b )
{
  libspectrum_byte *bank = general_sound_bank_write[ address >> 14 ];
  if( bank ) bank[ address & 0x3fff ] = b;
}

static libspectrum_byte
gs_z80_readbyte_timed( libspectrum_word address )
{
  gs_z80_tstates += 3;
  return gs_z80_readbyte( address );
}

static void
gs_z80_writebyte_timed( libspectrum_word address, libspectrum_byte b )
{
  gs_z80_tstates += 3;
  gs_z80_writebyte( address, b );
}

static libspectrum_byte
gs_z80_readport( libspectrum_word port )
{
  libspectrum_byte b;

  gs_z80_tstates++;
  b = general_sound_port_read( port );
  gs_z80_tstates += 3;

  return b;
}

static void
gs_z80_writeport( libspectrum_word port, libspectrum_byte b )
{
  gs_z80_tstates++;
  general_sound_port_write( port, b );
  gs_z80_tstates += 3;
}

/* --- execution ------------------------------------------------------- */

/* One instruction. Under HAVE_ENOUGH_MEMORY the generated table carries
   the CB, DD, ED, FD and DDCB tables inline, so this single include is the
   whole instruction set. */
static void
gs_z80_step( void )
{
  libspectrum_byte opcode;
  libspectrum_byte last_Q;

  /* LD A,I and LD A,R raise this, and it stands only until the next
     instruction runs: the window in which an accepted interrupt has to
     correct the flag they copied IFF2 into. */
  z80.iff2_read = 0;

  contend_read( PC, 4 );
  opcode = readbyte_internal( PC );

 end_opcode:
  PC++; R++;
  last_Q = Q;
  Q = 0;

  switch( opcode ) {
#include "z80/opcodes_base.c"
  }

  (void)last_Q;
}

static int
gs_z80_interrupt( void )
{
  if( !IFF1 ) return 0;

  /* Not immediately after an EI */
  if( tstates == z80.interrupts_enabled_at ) return 0;

  /* On an NMOS part IFF2 is already clear by the time LD A,I or LD A,R
     copies it, so an interrupt taken in that window leaves P/V clear. */
  if( z80.iff2_read ) F &= ~FLAG_P;

  if( z80.halted ) { PC++; z80.halted = 0; }

  IFF1 = IFF2 = 0;
  R++;
  tstates += 7;

  writebyte( --SP, PCH );
  writebyte( --SP, PCL );

  if( IM < 2 ) {
    PC = 0x0038;
  } else {
    /* The card leaves 0xff on its data bus */
    libspectrum_word vector = ( 0x100 * I ) + 0xff;
    PCL = readbyte( vector++ );
    PCH = readbyte( vector );
  }

  z80.memptr.w = PC;
  Q = 0;

  return 1;
}

void
gs_z80_reset( void )
{
  CLOCKL = 0;
  CLOCKH = 0;
  AF = AF_ = 0xffff;
  I = R = R7 = 0;
  PC = 0;
  SP = 0xffff;
  IFF1 = IFF2 = IM = 0;
  BC = DE = HL = 0;
  BC_ = DE_ = HL_ = 0;
  IX = IY = 0;
  z80.memptr.w = 0;
  z80.halted = 0;
  z80.iff2_read = 0;
  Q = 0;
  z80.interrupts_enabled_at = -1;
}

/* Zero the card's timebase. Its interrupt divider free-runs, so this is
   done once when the card appears, not on every reset. */
void
gs_z80_reset_clock( void )
{
  tstates = 0;
  gs_z80_period_base = 0;
}

void
gs_z80_nmi( void )
{
  if( z80.halted ) { PC++; z80.halted = 0; }

  IFF1 = 0;
  R++;
  tstates += 5;

  writebyte( --SP, PCH );
  writebyte( --SP, PCL );

  Q = 0;
  PC = 0x0066;
}

libspectrum_qword
gs_z80_now( void )
{
  return gs_z80_period_base + tstates;
}

void
gs_z80_run( libspectrum_qword target )
{
  for(;;) {
    while( gs_z80_period_base + tstates < target &&
	   tstates < GS_Z80_INT_PERIOD )
      gs_z80_step();

    if( tstates < GS_Z80_INT_PERIOD ) break;

    gs_z80_interrupt();
    tstates -= GS_Z80_INT_PERIOD;
    if( z80.interrupts_enabled_at >= 0 )
      z80.interrupts_enabled_at -= GS_Z80_INT_PERIOD;
    gs_z80_period_base += GS_Z80_INT_PERIOD;
  }
}
