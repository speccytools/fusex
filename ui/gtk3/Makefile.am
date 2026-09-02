## Process this file with automake to produce Makefile.in
## Copyright (c) 2001-2016 Philip Kendall
## Copyright (c) 2015-2018 Sergio Baldoví
## Copyright (c) 2026 Alberto Garcia
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

if UI_GTK

fuse_SOURCES += $(ui_gtk_files)

BUILT_SOURCES += $(ui_gtk_built)

endif

gtkui_resource_files = \
              ui/gtk3/icons/disk_active.png \
              ui/gtk3/icons/disk_inactive.png \
              ui/gtk3/icons/mdr_active.png \
              ui/gtk3/icons/mdr_inactive.png \
              ui/gtk3/icons/mouse_active.png \
              ui/gtk3/icons/mouse_inactive.png \
              ui/gtk3/icons/pause_active.png \
              ui/gtk3/icons/pause_inactive.png \
              ui/gtk3/icons/tape_active.png \
              ui/gtk3/icons/tape_inactive.png \
              ui/gtk3/icons/tape_marker.png

gtkui_built_resource_files = \
              ui/gtk3/menu_data.ui

CLEANFILES += $(ui_gtk_built) ui/gtk3/options.stamp

ui_gtk_files = \
               ui/gtk3/binary.c \
               ui/gtk3/browse.c \
               ui/gtk3/confirm.c \
               ui/gtk3/debugger.c \
               ui/gtk3/fileselector.c \
               ui/display_timing.c \
               ui/gtk3/filter_options.c \
               ui/gtk3/gtkdisplay.c \
               ui/gtk3/gtkinternals.h \
               ui/gtk3/gtkjoystick.c \
               ui/gtk3/gtkkeyboard.c \
               ui/gtk3/gtkmouse.c \
               ui/gtk3/gtkui.c \
               ui/gtk3/keysyms.c \
               ui/gtk3/menu_data.c \
               ui/gtk3/options.c \
               ui/gtk3/picture.c \
               ui/gtk3/pokefinder.c \
               ui/gtk3/pokemem.c \
               ui/gtk3/resources.c \
               ui/gtk3/resources.h \
               ui/gtk3/rollback.c \
               ui/gtk3/roms.c \
               ui/gtk3/statusbar.c \
               ui/gtk3/stock.c \
               ui/gtk3/memory.c

ui_gtk_built = \
               ui/gtk3/keysyms.c \
               ui/gtk3/menu_data.c \
               ui/gtk3/menu_data.ui \
               ui/gtk3/options.c \
               ui/gtk3/options_internals.h \
               ui/gtk3/resources.c \
               ui/gtk3/resources.h

EXTRA_DIST += \
              $(gtkui_resource_files) \
              ui/gtk3/options.pl \
              ui/gtk3/options-header.pl \
              ui/gtk3/resources.xml

ui/gtk3/keysyms.c: $(srcdir)/keysyms.pl $(srcdir)/keysyms.dat
	@$(MKDIR_P) ui/gtk3
	$(AM_V_GEN)$(PERL) -I$(srcdir)/perl $(srcdir)/keysyms.pl gtk $(srcdir)/keysyms.dat > $@.tmp && mv $@.tmp $@

ui/gtk3/menu_data.c: $(srcdir)/perl/cpp-perl.pl $(srcdir)/menu_data.pl $(srcdir)/menu_data.dat config.h
	@$(MKDIR_P) ui/gtk3
	$(AM_V_GEN)$(PERL) $(srcdir)/perl/cpp-perl.pl config.h $(srcdir)/menu_data.dat | $(PERL) -I$(srcdir)/perl $(srcdir)/menu_data.pl gtk c > $@.tmp && mv $@.tmp $@

ui/gtk3/menu_data.ui: $(srcdir)/perl/cpp-perl.pl $(srcdir)/menu_data.pl $(srcdir)/menu_data.dat config.h
	@$(MKDIR_P) ui/gtk3
	$(AM_V_GEN)$(PERL) $(srcdir)/perl/cpp-perl.pl config.h $(srcdir)/menu_data.dat | $(PERL) -I$(srcdir)/perl $(srcdir)/menu_data.pl gtk ui > $@.tmp && mv $@.tmp $@

ui/gtk3/options.stamp: $(srcdir)/perl/cpp-perl.pl config.h $(srcdir)/ui/gtk3/options.pl $(srcdir)/ui/gtk3/options-header.pl $(srcdir)/ui/options.dat $(srcdir)/perl/Fuse.pm $(srcdir)/perl/Fuse/Dialog.pm
	@$(MKDIR_P) ui/gtk3
	$(AM_V_GEN)$(PERL) $(srcdir)/perl/cpp-perl.pl config.h $(srcdir)/ui/options.dat | $(PERL) -I$(srcdir)/perl $(srcdir)/ui/gtk3/options.pl - > ui/gtk3/options.c.tmp && mv ui/gtk3/options.c.tmp ui/gtk3/options.c
	$(AM_V_GEN)$(PERL) $(srcdir)/perl/cpp-perl.pl config.h $(srcdir)/ui/options.dat | $(PERL) -I$(srcdir)/perl $(srcdir)/ui/gtk3/options-header.pl - > ui/gtk3/options_internals.h.tmp && mv ui/gtk3/options_internals.h.tmp ui/gtk3/options_internals.h
	@touch $@

ui/gtk3/options.c ui/gtk3/options_internals.h: ui/gtk3/options.stamp

ui/gtk3/resources.c: ui/gtk3/resources.xml $(gtkui_resource_files) $(gtkui_built_resource_files)
	$(AM_V_GEN)glib-compile-resources --sourcedir=$(srcdir) --sourcedir=$(builddir) --target=$@ --internal --generate-source --c-name=gtkui $<

ui/gtk3/resources.h: ui/gtk3/resources.xml $(gtkui_resource_files) $(gtkui_built_resource_files)
	$(AM_V_GEN)glib-compile-resources --sourcedir=$(srcdir) --sourcedir=$(builddir) --target=$@ --internal --generate-header --c-name=gtkui $<
