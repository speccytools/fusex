/* gdbserver_stub.c: Stubs for builds without the GDB server
   Copyright (c) 2026 The FuseX authors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
*/

#include "config.h"

#include "gdbserver.h"
#include "settings.h"
#include "ui/ui.h"

void
gdbserver_init( void )
{
  if( settings_current.gdbserver_enable ) {
    ui_error( UI_ERROR_WARNING,
              "GDB server support is not available in this build" );
    settings_current.gdbserver_enable = 0;
  }
}

int
gdbserver_start( int port GCC_UNUSED )
{
  return 1;
}

void
gdbserver_stop( void )
{
}

int
gdbserver_activate( void )
{
  return 0;
}

int
gdbserver_activate_with_reason( int trap_reason GCC_UNUSED )
{
  return 0;
}

void
gdbserver_note_emulating( void )
{
}

void
gdbserver_refresh_status( void )
{
  settings_current.gdbserver_enable = 0;
}

void
gdbserver_schedule_reset( void )
{
}

void
gdbserver_schedule_autoboot( void )
{
}

void
gdbserver_send_remote_console_output( const char *text GCC_UNUSED )
{
}

uint8_t
gdbserver_reset_via_remote_command( void )
{
  return 0;
}

void
gdbserver_lock_network_write( void )
{
}

void
gdbserver_unlock_network_write( void )
{
}

uint8_t
gdbserver_execute_on_main_thread( trapped_action_t call GCC_UNUSED,
                                  const void *data GCC_UNUSED,
                                  void *response GCC_UNUSED )
{
  return 0;
}

void
gdbserver_on_machine_reset( void )
{
}
