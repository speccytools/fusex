## Process this file with automake to produce Makefile.in
## Copyright (c) 1999-2017 Philip Kendall
## Copyright (c) 2026 Fredrick Meunier

## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License along
## with this program; if not, write to the Free Software Foundation, Inc.,
## 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
##
## Author contact information:
##
## E-mail: philip-fuse@shadowmagic.org.uk

ACLOCAL_AMFLAGS = -I m4

bin_PROGRAMS = fuse

noinst_PROGRAMS =

fuse_SOURCES = display.c \
	event.c \
	fuse.c \
	input.c \
	keyboard.c \
	loader.c \
	machine.c \
	memory_pages.c \
	mempool.c \
	menu.c \
	movie.c \
	module.c \
	periph.c \
	phantom_typist.c \
	profile.c \
	psg.c \
	rectangle.c \
	rzx.c \
	screenshot.c \
	settings.c \
	slt.c \
	snapshot.c \
	sound.c \
	spectrum.c \
	svg.c \
	tape.c \
	ui.c \
	uidisplay.c \
	uimedia.c \
	utils.c

fuse_LDADD = \
             $(PTHREAD_LIBS) \
             $(LIBSPECTRUM_LIBS) \
             $(GTK_LIBS) \
             $(GLIB_LIBS) \
             $(PNG_LIBS) \
             $(X_LIBS) \
             $(XML_LIBS)

if BUILD_SPECTRANET
fuse_LDADD += $(LIBSSH2_LIBS) $(MBEDTLS_LIBS)
endif

if USE_SDL1
fuse_LDADD += $(SDL1_LIBS)
endif

if USE_SDL2
fuse_LDADD += $(SDL2_LIBS)
endif

fuse_DEPENDENCIES =

EXTRA_fuse_SOURCES =

BUILT_SOURCES = options.h settings.c settings.h \
		debugger/commandy.c \
		debugger/commandy.h \
		debugger/commandy.tab.h \
		debugger/commandl.c

if COMPAT_WIN32
settings.c: settings-win32.pl settings.dat
	$(AM_V_GEN)$(PERL) -I$(srcdir)/perl $(srcdir)/settings-win32.pl $(srcdir)/settings.dat > $@.tmp && mv $@.tmp $@
else
settings.c: settings.pl settings.dat
	$(AM_V_GEN)$(PERL) -I$(srcdir)/perl $(srcdir)/settings.pl $(srcdir)/settings.dat > $@.tmp && mv $@.tmp $@
endif

settings.h: settings-header.pl settings.dat
	$(AM_V_GEN)$(PERL) -I$(srcdir)/perl $(srcdir)/settings-header.pl $(srcdir)/settings.dat > $@.tmp && mv $@.tmp $@

# commandl.l #includes "commandy.tab.h" (the bison default name the Xcode build
# produces); automake's yacc rule emits commandy.h, so alias it.
debugger/commandy.tab.h: debugger/commandy.h
	$(AM_V_GEN)cp $< $@

options.h: $(srcdir)/perl/cpp-perl.pl config.h $(srcdir)/ui/@OPTIONS_DIR@/options-header.pl $(srcdir)/ui/options.dat $(srcdir)/perl/Fuse.pm $(srcdir)/perl/Fuse/Dialog.pm
	$(AM_V_GEN)$(PERL) $(srcdir)/perl/cpp-perl.pl config.h $(srcdir)/ui/options.dat | $(PERL) -I$(srcdir)/perl $(srcdir)/ui/@OPTIONS_DIR@/options-header.pl - public > $@.tmp && mv $@.tmp $@

AM_CPPFLAGS = \
              $(GLIB_CFLAGS) \
              $(GTK_CFLAGS) \
              $(LIBSPECTRUM_CFLAGS) \
              $(XML_CFLAGS) \
              -DFUSEDATADIR="\"${pkgdatadir}\"" \
              $(PNG_CFLAGS)

if USE_SDL1
AM_CPPFLAGS += $(SDL1_CFLAGS)
endif

if USE_SDL2
AM_CPPFLAGS += $(SDL2_CFLAGS)
endif

if BUILD_SPECTRANET
AM_CPPFLAGS += $(MBEDTLS_CFLAGS) $(LIBSSH2_CFLAGS)
endif

AM_CFLAGS = $(WARN_CFLAGS) $(PTHREAD_CFLAGS)

noinst_HEADERS = bitmap.h \
	compat.h \
	display.h \
	event.h \
	fuse.h \
	input.h \
	keyboard.h \
	loader.h \
	machine.h \
	memory_pages.h \
	mempool.h \
	menu.h \
	movie.h \
	movie_tables.h \
	module.h \
	periph.h \
	phantom_typist.h \
	psg.h \
	rectangle.h \
	rzx.h \
	screenshot.h \
	settings.h \
	slt.h \
	snapshot.h \
	sound.h \
	spectrum.h \
	svg.h \
	tape.h \
	utils.h \
	options.h \
	profile.h

EXTRA_DIST = AUTHORS \
	     INSTALL \
	     PORTING \
	     README \
	     THANKS \
	     keysyms.dat \
	     keysyms.pl \
	     m4/ax_pthread.m4 \
	     m4/ax_string_strcasecmp.m4 \
	     m4/pkg.m4 \
	     m4/sdl.m4 \
	     menu_data.dat \
	     menu_data.pl \
	     settings.dat \
	     settings.pl \
	     settings-win32.pl \
	     settings-header.pl \
	     update-release.py \
	     .agents/skills/README.md \
	     .agents/skills/update-changelog/SKILL.md

CLEANFILES = options.h \
	     settings.c \
	     settings.h

DISTCLEANFILES =

pkgdata_DATA =

test: z80/coretest unittests/displaytest
	z80/coretest $(srcdir)/z80/tests/tests.in > z80/tests.actual
	cmp z80/tests.actual $(srcdir)/z80/tests/tests.expected
	./unittests/displaytest


## Resources for Windows executables
if COMPAT_WIN32

fuse_LDFLAGS = -mwindows
fuse_SOURCES += windres.rc
fuse_LDADD += -lcomctl32 -lwinmm -lshell32 $(WINSPARKLE_LIBS) windres.o
fuse_DEPENDENCIES += windres.o
AM_CPPFLAGS += $(WINSPARKLE_CFLAGS)

windres.o: windres.rc data/win32/winfuse.ico data/win32/fuse.manifest $(ui_win32_res)
	$(AM_V_GEN)$(WINDRES) -I$(srcdir) -I. $(srcdir)/windres.rc $(LIBSPECTRUM_CFLAGS) $(CPPFLAGS) windres.o

endif


include compat/Makefile.am
include data/Makefile.am
include debugger/Makefile.am
include hacking/Makefile.am
include infrastructure/Makefile.am
include lib/Makefile.am
include machines/Makefile.am
include man/Makefile.am
include peripherals/Makefile.am
include perl/Makefile.am
include pokefinder/Makefile.am
include roms/Makefile.am
include sound/Makefile.am
include timer/Makefile.am
include ui/Makefile.am
include ui/fb/Makefile.am
include ui/gtk3/Makefile.am
include ui/null/Makefile.am
include ui/scaler/Makefile.am
include ui/sdl/Makefile.am
include ui/sdl2/Makefile.am
include ui/widget/Makefile.am
include ui/wii/Makefile.am
include ui/win32/Makefile.am
include ui/xlib/Makefile.am
include unittests/Makefile.am
include z80/Makefile.am

check-local: fuse unittests/displaytest unittests/sdl2displaytest unittests/sdl2scalerstatetest unittests/sdl2joysticktest unittests/sdl2mousetest unittests/palcompositetest unittests/scalerexpandtest z80/coretest
	@test -e $(top_builddir)/roms || ln -sf "$(abs_top_srcdir)/roms" $(top_builddir)/roms
	$(top_builddir)/fuse --unittests
	$(top_builddir)/unittests/displaytest
	$(top_builddir)/unittests/sdl2displaytest
	$(top_builddir)/unittests/sdl2scalerstatetest
	$(top_builddir)/unittests/sdl2joysticktest
	$(top_builddir)/unittests/sdl2mousetest
	$(top_builddir)/unittests/palcompositetest
	$(top_builddir)/unittests/scalerexpandtest
	$(top_builddir)/z80/coretest $(srcdir)/z80/tests/tests.in > $(top_builddir)/z80/tests.actual
	cmp $(top_builddir)/z80/tests.actual $(srcdir)/z80/tests/tests.expected
