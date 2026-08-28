#include "gdbserver_remote_commands.h"
#include "gdbserver.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuse.h"
#include "libspectrum.h"
#include "peripherals/fs/xfs.h"
#include "peripherals/spectranet.h"
#include "snapshot.h"
#include "utils.h"

static const char *
skip_spaces( const char *text )
{
    while( text && *text && isspace( (unsigned char)*text ) ) text++;
    return text;
}

static uint8_t remote_command_help( const char *args GCC_UNUSED )
{
    const struct remote_command_entry_t *entry;

    gdbserver_send_remote_console_output("Supported commands:\n");

    for (entry = remote_commands; entry->name; entry++) {
        gdbserver_send_remote_console_output(entry->name);
        gdbserver_send_remote_console_output("\n");
    }

    return 0;
}

static uint8_t remote_command_reset( const char *args GCC_UNUSED )
{
    return gdbserver_reset_via_remote_command();
}

static int
write_sna_snapshot( const char *filename )
{
    libspectrum_snap *snap;
    libspectrum_byte *buffer = NULL;
    size_t length = 0;
    int flags = 0;
    int error;

    snap = libspectrum_snap_alloc();
    if( !snap ) return 1;

    error = snapshot_copy_to( snap );
    if( error ) { libspectrum_snap_free( snap ); return error; }

    error = libspectrum_snap_write( &buffer, &length, &flags, snap,
                                    LIBSPECTRUM_ID_SNAPSHOT_SNA,
                                    fuse_creator, 0 );
    libspectrum_snap_free( snap );
    if( error ) return error;

    error = utils_write_file( filename, buffer, length );
    libspectrum_free( buffer );

    return error;
}

static uint8_t remote_command_dump( const char *args )
{
    char type[32];
    const char *filename;
    size_t type_len;

    args = skip_spaces( args );
    if( !args || !*args ) {
        gdbserver_send_remote_console_output(
            "Usage: dump <sna|spectranet-ram> <file>\n" );
        return 1;
    }

    filename = args;
    while( *filename && !isspace( (unsigned char)*filename ) ) filename++;

    type_len = filename - args;
    if( type_len == 0 || type_len >= sizeof( type ) ) return 1;

    memcpy( type, args, type_len );
    type[type_len] = '\0';

    filename = skip_spaces( filename );
    if( !filename || !*filename ) {
        gdbserver_send_remote_console_output(
            "Usage: dump <sna|spectranet-ram> <file>\n" );
        return 1;
    }

    if( !strcmp( type, "sna" ) ) {
        return write_sna_snapshot( filename ) ? 1 : 0;
    }

    if( !strcmp( type, "spectranet-ram" ) ) {
        return spectranet_dump_ram( filename ) ? 1 : 0;
    }

    gdbserver_send_remote_console_output(
        "Unsupported dump type. Supported: sna, spectranet-ram\n" );
    return 1;
}

static uint8_t remote_command_spectranet_info( const char *args GCC_UNUSED )
{
    spectranet_paging_info_t info = spectranet_get_paging_info();
    char buffer[128];

    snprintf( buffer, sizeof( buffer ),
              "Spectranet available: %s\n"
              "Spectranet paged in: %s\n"
              "Page 0: 0x%02x\n"
              "Page A: 0x%02x\n"
              "Page B: 0x%02x\n"
              "Page 3: 0x%02x\n",
              info.available ? "yes" : "no",
              info.paged ? "yes" : "no",
              info.page_0,
              info.page_a,
              info.page_b,
              info.page_3 );
    gdbserver_send_remote_console_output( buffer );

    return 0;
}

static uint8_t
remote_command_spectranet_set_page( const char *args )
{
    char section;
    const char *end;
    char *page_end;
    unsigned long page;
    int destination;
    char buffer[64];

    args = skip_spaces( args );
    if( !args || !*args ) goto usage;

    section = tolower( (unsigned char)*args++ );
    if( *args && !isspace( (unsigned char)*args ) ) goto usage;
    args = skip_spaces( args );
    if( !args || !*args ) goto usage;

    if( *args == '$' )
        page = strtoul( args + 1, &page_end, 16 );
    else
        page = strtoul( args, &page_end, 0 );
    end = page_end;
    if( end == args || (args[0] == '$' && end == args + 1) ||
        *skip_spaces( end ) || page > 0xff ) goto usage;

    switch( section ) {
    case '0': destination = 0; break;
    case 'a': destination = 1; break;
    case 'b': destination = 2; break;
    case '3': destination = 3; break;
    default: goto usage;
    }

    spectranet_map_page( destination, (int)page );
    snprintf( buffer, sizeof( buffer ), "Spectranet page %c set to 0x%02lx\n",
              section, page );
    gdbserver_send_remote_console_output( buffer );
    return 0;

usage:
    gdbserver_send_remote_console_output(
        "Usage: spectranet-set-page <a|b|0|3> <page>\n" );
    return 1;
}

static uint8_t
remote_command_spectranet_set_pagein( const char *args )
{
    const char *end;
    size_t length;

    args = skip_spaces( args );
    if( !args || !*args ) goto usage;
    end = args;
    while( *end && !isspace( (unsigned char)*end ) ) end++;
    length = end - args;
    if( *skip_spaces( end ) ) goto usage;

    if( length == 3 && !strncmp( args, "yes", length ) ) {
        spectranet_page( 0 );
        gdbserver_send_remote_console_output( "Spectranet paged in\n" );
        return 0;
    }
    if( length == 2 && !strncmp( args, "no", length ) ) {
        spectranet_unpage();
        gdbserver_send_remote_console_output( "Spectranet paged out\n" );
        return 0;
    }

usage:
    gdbserver_send_remote_console_output(
        "Usage: spectranet-set-pagein <yes|no>\n" );
    return 1;
}

static uint8_t remote_command_xfs_debug( const char *args )
{
    int enable;
    const char *end;
    size_t length;
    char buffer[64];

    args = skip_spaces( args );

    if( !args || !*args ) {
        gdbserver_send_remote_console_output(
            "Usage: xfs-debug <on|off|1|0>\n" );
        return 1;
    }

    end = args;
    while( *end && !isspace( (unsigned char)*end ) ) end++;
    length = end - args;

    if( length == 2 && !strncmp( args, "on", length ) ) {
        enable = 1;
    } else if( length == 1 && !strncmp( args, "1", length ) ) {
        enable = 1;
    } else if( length == 4 && !strncmp( args, "true", length ) ) {
        enable = 1;
    } else if( length == 3 && !strncmp( args, "off", length ) ) {
        enable = 0;
    } else if( length == 1 && !strncmp( args, "0", length ) ) {
        enable = 0;
    } else if( length == 5 && !strncmp( args, "false", length ) ) {
        enable = 0;
    } else {
        gdbserver_send_remote_console_output(
            "Usage: xfs-debug <on|off|1|0>\n" );
        return 1;
    }

    end = skip_spaces( end );
    if( end && *end ) {
        gdbserver_send_remote_console_output(
            "Usage: xfs-debug <on|off|1|0>\n" );
        return 1;
    }

    xfs_debug_enable( enable );

    snprintf( buffer, sizeof( buffer ), "XFS debug: %s\n",
              xfs_debug_is_enabled() ? "on" : "off" );
    gdbserver_send_remote_console_output( buffer );

    return 0;
}

const struct remote_command_entry_t remote_commands[] = {
    { "help", remote_command_help },
    { "reset", remote_command_reset },
    { "dump", remote_command_dump },
    { "spectranet-info", remote_command_spectranet_info },
    { "spectranet-set-page", remote_command_spectranet_set_page },
    { "spectranet-set-pagein", remote_command_spectranet_set_pagein },
    { "xfs-debug", remote_command_xfs_debug },
    { NULL, NULL }
};
