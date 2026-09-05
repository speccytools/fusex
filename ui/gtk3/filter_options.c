/* filter_options.c: options for current filter (scaler)
*/

#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include "compat.h"
#include "fuse.h"
#include "gtkinternals.h"
#include "menu.h"
#include "settings.h"
#include "ui/ui.h"

static int create_dialog( void );
static void browse_done( GtkWidget *widget, gpointer data );
static gboolean delete_dialog( GtkWidget *widget, GdkEvent *event,
             gpointer user_data );

static GtkWidget
  *dialog;      /* The dialog box itself */

static int dialog_created;  /* Have we created the dialog box yet? */

MENU_CALLBACK( menu_options_filteroptions )
{
  /* Firstly, stop emulation */
  fuse_emulation_pause();

  if( !dialog_created )
    if( create_dialog() ) { fuse_emulation_unpause(); return; }

  gtk_widget_show_all( dialog );

  /* Carry on with emulation */
  fuse_emulation_unpause();
}

struct option_t {
  const char *label;
  int *value;
};

gboolean value_change(GtkRange     *range,
               GtkScrollType scroll,
               gdouble       value,
               gpointer      user_data
) {
  *((int *)user_data) = (int)value;
  return FALSE;
}

static int
create_dialog( void )
{
  GtkWidget *content_area, *range, *label, *box;
  struct option_t options[] = {
    {
      .label = "Hue",
      .value = &settings_current.composite_filter_hue
    },
    {
      .label = "Saturation",
      .value = &settings_current.composite_filter_saturation
    },
    {
      .label = "Contrast",
      .value = &settings_current.composite_filter_contrast
    },
    {
      .label = "Brightness",
      .value = &settings_current.composite_filter_brightness
    },
    {
      .label = "Sharpness",
      .value = &settings_current.composite_filter_sharpness
    },
    {
      .label = "Gamma",
      .value = &settings_current.composite_filter_gamma
    },
    {
      .label = "Resolution",
      .value = &settings_current.composite_filter_resolution
    },
    {
      .label = "Artifacts",
      .value = &settings_current.composite_filter_artifacts
    },
    {
      .label = "Fringing",
      .value = &settings_current.composite_filter_fringing
    },
    {
      .label = "Bleed",
      .value = &settings_current.composite_filter_bleed
    }
  };
  const struct option_t *option;

  /* Give me a new dialog box */
  dialog = gtkstock_dialog_new( "Fuse - Filter Options",
        G_CALLBACK( delete_dialog ) );

gtk_widget_set_size_request (dialog, 300, 600);
  content_area = gtk_dialog_get_content_area( GTK_DIALOG( dialog ) );

  /* Create the OK button */
  gtkstock_create_close( dialog, NULL, G_CALLBACK( browse_done ), FALSE );

  /* Make the window big enough to show at least some data */
  gtk_window_set_default_size( GTK_WINDOW( dialog ), -1, 250 );

  for (option = options; option != options + sizeof(options)/sizeof(*options); ++option) {
    label = gtk_label_new(option->label);
    range = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -100, 100, 1);
    gtk_range_set_value( GTK_RANGE( range ), *(option->value) );
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start( GTK_BOX( box ), label, TRUE, TRUE, 5 );
    gtk_box_pack_start( GTK_BOX( box ), range, TRUE, TRUE, 5 );
    gtk_box_pack_start( GTK_BOX( content_area ), box, TRUE, TRUE, 5 );
    g_signal_connect(range, "change-value", G_CALLBACK(value_change), option->value);
  }

  dialog_created = 1;

  return 0;
}

/* Called if the OK button is clicked */
static void
browse_done( GtkWidget *widget GCC_UNUSED, gpointer data GCC_UNUSED )
{
  gtk_widget_hide( dialog );
}

/* Catch attempts to delete the window and just hide it instead */
static gboolean
delete_dialog( GtkWidget *widget, GdkEvent *event GCC_UNUSED,
         gpointer user_data GCC_UNUSED )
{
  gtk_widget_hide( widget );
  return TRUE;
}
