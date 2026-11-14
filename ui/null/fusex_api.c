#include <stdio.h>
#include <string.h>
#include <libspectrum.h>

#include "debugger/debugger.h"
#include "memory_pages.h"
#include "ui/ui.h"

#include "fusex_debugger.h"
#include "fusex_display.h"
#include "fusex_memory.h"
#include "fusex_registers.h"

extern const fusex_display_info_t *null_ui_display_info( void );
extern const fusex_display_frame_t *null_ui_display_frame( void );

const fusex_display_info_t *
fusex_display_info( void )
{
  return null_ui_display_info();
}

const fusex_display_frame_t *
fusex_display_frame( void )
{
  return null_ui_display_frame();
}

uint8_t
fusex_peek(const uint16_t addr )
{
  return readbyte_internal( addr );
}

void
fusex_poke(const uint16_t addr, const uint8_t value )
{
  writebyte_internal( addr, value );
}

int
fusex_register_get( enum z80_register reg, libspectrum_word *value )
{
  return z80_register_get( reg, value );
}

struct text_buffer {
  char *buf;
  size_t size;
};

static void
append_text( const char *text, void *user )
{
  struct text_buffer *out = user;
  size_t length = strlen( out->buf );

  snprintf( out->buf + length, out->size - length, "%s", text );
}

int
fusex_debugger_command( const char *command, char *output, const int output_size )
{
  struct text_buffer out = { output, (size_t)output_size };

  output[0] = '\0';
  ui_error_capture_begin( append_text, &out );
  debugger_output_capture_begin( append_text, &out );
  debugger_command_evaluate( command );
  debugger_output_capture_end();
  ui_error_capture_end();
  return ui_error_capture_had_error();
}
