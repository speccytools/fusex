/* file.c: File-related compatibility routines
   Copyright (c) 2008-2011 Philip Kendall

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

   E-mail: philip-fuse@shadowmagic.org.uk

*/

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat.h"
#include "utils.h"
#include "ui/ui.h"

const compat_fd COMPAT_FILE_OPEN_FAILED = NULL;

static compat_fd default_file_open( const char *path, int write );
static off_t default_file_get_length( compat_fd fd );
static int default_file_read( compat_fd fd, utils_file *file );
static int default_file_write( compat_fd fd, const unsigned char *buffer,
                               size_t length );
static int default_file_close( compat_fd fd );
static int default_file_exists( const char *path );

static compat_file_vtable_t file_vtable = {
  default_file_open, default_file_get_length, default_file_read,
  default_file_write, default_file_close, default_file_exists
};

void
compat_file_set_vtable( compat_file_vtable_t *vtable )
{
  file_vtable = *vtable;
}

void
compat_file_get_vtable( compat_file_vtable_t *vtable )
{
  *vtable = file_vtable;
}

static compat_fd
default_file_open( const char *path, int write )
{
  struct stat statbuf;

  if( !write ) {
    if( stat( path, &statbuf ) ) {
      return NULL;
    }

    /* Check file type */
    if( !S_ISREG( statbuf.st_mode ) ) {
      errno = EINVAL;
      return NULL;
    }
  }

  return fopen( path, write ? "wb" : "rb" );
}

static off_t
default_file_get_length( compat_fd fd )
{
  struct stat file_info;

  if( fstat( fileno( fd ), &file_info ) ) {
    ui_error( UI_ERROR_ERROR, "couldn't stat file: %s", strerror( errno ) );
    return -1;
  }

  return file_info.st_size;
}

static int
default_file_read( compat_fd fd, utils_file *file )
{
  size_t bytes = fread( file->buffer, 1, file->length, fd );
  if( bytes != file->length ) {
    ui_error( UI_ERROR_ERROR,
              "error reading file: expected %lu bytes, but read only %lu",
              (unsigned long)file->length, (unsigned long)bytes );
    return 1;
  }

  return 0;
}

static int
default_file_write( compat_fd fd, const unsigned char *buffer, size_t length )
{
  size_t bytes = fwrite( buffer, 1, length, fd );
  if( bytes != length ) {
    ui_error( UI_ERROR_ERROR,
              "error writing file: expected %lu bytes, but wrote only %lu",
              (unsigned long)length, (unsigned long)bytes );
    return 1;
  }

  return 0;
}

static int
default_file_close( compat_fd fd )
{
  return fclose( fd );
}

static int
default_file_exists( const char *path )
{
  return ( access( path, R_OK ) != -1 );
}

compat_fd
compat_file_open( const char *path, int write )
{
  return file_vtable.open( path, write );
}

off_t
compat_file_get_length( compat_fd fd )
{
  return file_vtable.get_length( fd );
}

int
compat_file_read( compat_fd fd, utils_file *file )
{
  return file_vtable.read( fd, file );
}

int
compat_file_write( compat_fd fd, const unsigned char *buffer, size_t length )
{
  return file_vtable.write( fd, buffer, length );
}

int
compat_file_close( compat_fd fd )
{
  return file_vtable.close( fd );
}

int
compat_file_exists( const char *path )
{
  return file_vtable.exists( path );
}
