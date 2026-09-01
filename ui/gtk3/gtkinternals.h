/* gtkinternals.h: stuff internal to the GTK UI
   Copyright (c) 2003-2018 Philip Kendall

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

#ifndef FUSE_GTKINTERNALS_H
#define FUSE_GTKINTERNALS_H

#include <gtk/gtk.h>
#include "libspectrum.h"

/*
 * Display routines (gtkdisplay.c)
 */

/* The colour palette in use */
extern libspectrum_dword gtkdisplay_colours[ 16 ];

void gtkdisplay_update_geometry( void );
void gtkdisplay_get_window_size( int *width, int *height );

/*
 * Keyboard routines (gtkkeyboard.c)
 */

int gtkkeyboard_keypress( GtkWidget *widget, GdkEvent *event,
			  gpointer data);
int gtkkeyboard_keyrelease( GtkWidget *widget, GdkEvent *event,
			    gpointer data);
int gtkkeyboard_release_all( GtkWidget *widget, GdkEvent *event,
			     gpointer data );

/*
 * Mouse routines (gtkmouse.c)
 */

void gtkmouse_init( void );

/*
 * General user interface routines (gtkui.c)
 */

extern GtkWidget *gtkui_window;
extern GtkWidget *gtkui_drawing_area;

void gtkui_destroy_widget_and_quit( GtkWidget *widget, gpointer data );

int gtkui_confirm( const char *string );

int gtkui_picture( const char *filename, int border );

extern void gtkui_popup_menu(void);

void gtkui_fullscreen_apply( void );

GtkAccelGroup* gtkstock_add_accel_group( GtkWidget *widget );

/* Set modifier=0 to use the first default accel key.
 * Set modifier_alt=0 to use the second default accel key.
 * For either, GDK_KEY_VoidSymbol means "no accel key".
 */
typedef struct gtkstock_button {
  const gchar *label;
  GCallback action;		/* "clicked" func; data is actiondata. */
  gpointer actiondata;
  GCallback destroy;	/* "clicked" func; data is parent widget */
  guint shortcut;
  GdkModifierType modifier;     /* primary shortcut */
  guint shortcut_alt;
  GdkModifierType modifier_alt; /* secondary shortcut */
  gint response_id;             /* response id for dialog */
} gtkstock_button;

/*
 * GTK3: GtkStock has been deprecated, we should not use "gtk-" prefix.
 *
 * If the target widget is a GtkDialog, then created buttons are put in its
 * action area.
 *
 * If the label begins with "!", then g_signal_connect_swapped, rather than
 * g_signal_connect, is used to connect the action function.
 */
GtkWidget* gtkstock_create_button( GtkWidget *widget, GtkAccelGroup *accel,
				   const gtkstock_button *btn );
GtkAccelGroup*
gtkstock_create_buttons( GtkWidget *widget, GtkAccelGroup *accel,
			 const gtkstock_button *buttons, size_t count );
GtkAccelGroup* gtkstock_create_ok_cancel( GtkWidget *widget,
					  GtkAccelGroup *accel,
	/* for OK button -> */	          GCallback action,
				          gpointer actiondata,
	/* for both buttons -> */         GCallback destroy_ok,
	                                  GCallback destroy_cancel );
GtkAccelGroup* gtkstock_create_close( GtkWidget *widget, GtkAccelGroup *accel,
				      GCallback destroy,
				      gboolean esconly );
	/* destroy==NULL => use DEFAULT_DESTROY */

#define DEFAULT_DESTROY ( G_CALLBACK( gtkui_destroy_widget_and_quit ) )

GtkWidget *gtkstock_dialog_new( const gchar *title, GCallback destroy );

int gtkui_get_monospaced_font( PangoFontDescription **font );
void gtkui_free_font( PangoFontDescription *font );


/* Show/hide the menu bar and status bar */
void gtkui_set_bars_visible( int visible );

/*
 * The menu data (menu_data.c)
 */

extern GtkActionEntry gtkui_menu_data[];
extern guint gtkui_menu_data_size;

/*
 * Resources for the GTK UI (ui/gtk3/resources.xml)
 */

#define FUSE_RESOURCE_PREFIX "/net/sourceforge/fuse_emulator/"
#define ICON_DISK_ACTIVE     FUSE_RESOURCE_PREFIX "disk_active.png"
#define ICON_DISK_INACTIVE   FUSE_RESOURCE_PREFIX "disk_inactive.png"
#define ICON_MDR_ACTIVE      FUSE_RESOURCE_PREFIX "mdr_active.png"
#define ICON_MDR_INACTIVE    FUSE_RESOURCE_PREFIX "mdr_inactive.png"
#define ICON_MOUSE_ACTIVE    FUSE_RESOURCE_PREFIX "mouse_active.png"
#define ICON_MOUSE_INACTIVE  FUSE_RESOURCE_PREFIX "mouse_inactive.png"
#define ICON_PAUSE_ACTIVE    FUSE_RESOURCE_PREFIX "pause_active.png"
#define ICON_PAUSE_INACTIVE  FUSE_RESOURCE_PREFIX "pause_inactive.png"
#define ICON_TAPE_ACTIVE     FUSE_RESOURCE_PREFIX "tape_active.png"
#define ICON_TAPE_INACTIVE   FUSE_RESOURCE_PREFIX "tape_inactive.png"
#define ICON_TAPE_MARKER     FUSE_RESOURCE_PREFIX "tape_marker.png"
#define GTK_MENU_DATA        FUSE_RESOURCE_PREFIX "menu_data.ui"

/*
 * Statusbar routines (statusbar.c)
 */


int gtkstatusbar_create( GtkBox *parent );
int gtkstatusbar_get_height( void );
int gtkstatusbar_set_visibility( int visible );
void gtkstatusbar_update_machine( const char *name );

/*
 * Routines for list widgets
 */

void gtkui_scroll_connect( GtkTreeView *list, GtkAdjustment *adj );
void gtkui_list_set_cursor( GtkTreeView *list, int row );
int gtkui_list_get_cursor( GtkTreeView *list );

/*
 * Dialog box reset
 */

void gtkui_pokefinder_clear( void );

#endif				/* #ifndef FUSE_GTKINTERNALS_H */
