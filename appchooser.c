#include "appchooser.h"

#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "flatpak-portal-dbus.h"
#include "app-portal-dbus.h"
#include "xdp-dbus.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

typedef struct {
  char *app_id;
  char *sender;

  char *uri;
  char *content_type;
  GAppInfo *default_app_info;
  GAppInfo *other_app_info;

  GtkWidget *dialog;
  GtkWidget *other_app_button;
  GDBusInterfaceSkeleton *skeleton;

} AppDialogHandle;

static AppDialogHandle *
app_dialog_handle_new (const char *app_id,
                       const char *sender,
                       GtkWidget *dialog,
                       GDBusInterfaceSkeleton *skeleton)
{
  AppDialogHandle *handle = g_new0 (AppDialogHandle, 1);

  handle->app_id = g_strdup (app_id);
  handle->sender = g_strdup (sender);
  handle->dialog = g_object_ref (dialog);
  handle->skeleton = g_object_ref (skeleton);

  return handle;
}

static void
app_dialog_handle_free (AppDialogHandle *handle)
{
  g_free (handle->app_id);
  g_free (handle->sender);
  g_free (handle->uri);
  g_free (handle->content_type);
  g_clear_object (&handle->default_app_info);
  g_clear_object (&handle->other_app_info);
  g_object_unref (handle->dialog);
  g_object_unref (handle->skeleton);
  g_free (handle);
}

static void
app_dialog_handle_close (AppDialogHandle *handle)
{
  g_print ("handle close\n");
  gtk_widget_destroy (handle->dialog);
  app_dialog_handle_free (handle);
}

static void update_button_for_other_app (AppDialogHandle *handle);

static void
app_chooser_response (GtkDialog *dialog,
                      gint response,
                      gpointer data)
{
  AppDialogHandle *handle = data;
  switch (response)
    {
    case GTK_RESPONSE_OK:
      {
        g_autoptr(GAppInfo) app_info = gtk_app_chooser_get_app_info (GTK_APP_CHOOSER (dialog));
        g_set_object (&handle->other_app_info, app_info);
        update_button_for_other_app (handle);
      }
      break;
    case GTK_RESPONSE_CANCEL:
    default:
      break;
    }

  gtk_widget_destroy (GTK_WIDGET (dialog));
}

static void
open_appchooser (GtkButton *button, gpointer data)
{
  AppDialogHandle *handle = data;
  GtkWidget *parent = gtk_widget_get_toplevel (GTK_WIDGET (button));
  GtkWidget *dialog;

  dialog = gtk_app_chooser_dialog_new_for_content_type (GTK_WINDOW (parent),
                                                        GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
                                                        handle->content_type);
  g_signal_connect (dialog, "response", G_CALLBACK (app_chooser_response), handle);
  gtk_window_present (GTK_WINDOW (dialog));
}

static void
open_uri (AppDialogHandle *handle, GAppInfo *app_info)
{
  GList uris;

  uris.data = handle->uri;
  uris.next = NULL;

  g_app_info_launch_uris (app_info, &uris, NULL, NULL);
  app_dialog_handle_close (handle);
}

static void
open_default (GtkButton *button, AppDialogHandle *handle)
{
  open_uri (handle, handle->default_app_info);
}

static void
open_other (GtkButton *button, AppDialogHandle *handle)
{
  open_uri (handle, handle->other_app_info);
}

static void
update_button_for_other_app (AppDialogHandle *handle)
{
  if (handle->other_app_info)
    {
      char *str;
      str = g_strdup_printf ("Open with %s", g_app_info_get_display_name (handle->other_app_info));
      gtk_button_set_label (GTK_BUTTON (handle->other_app_button), str);
      g_free (str);
      gtk_widget_show (handle->other_app_button);
    }
  else
    gtk_widget_hide (handle->other_app_button);
}

static gboolean
handle_app_chooser_open_uri (FlatpakDesktopAppChooser *object,
                             GDBusMethodInvocation *invocation,
                             const gchar *arg_sender,
                             const gchar *arg_app_id,
                             const gchar *arg_parent_window,
                             const gchar *arg_uri,
                             GVariant *arg_options)
{
  GtkWidget *window;
  GtkWidget *header;
  GtkWidget *grid;
  GtkWidget *label;
  GtkWidget *button;
  char *str;
  char *uri_scheme;
  char *content_type;
  g_autoptr(GAppInfo) app_info = NULL;
  AppDialogHandle *handle;

  g_print ("OpenUri: %s\n", arg_uri);

  uri_scheme = g_uri_parse_scheme (arg_uri);
  if (uri_scheme && uri_scheme[0] != '\0')
    {
      g_autofree char *scheme_down = g_ascii_strdown (uri_scheme, -1);
      content_type = g_strconcat ("x-scheme-handler/", scheme_down, NULL);
      app_info = g_app_info_get_default_for_uri_scheme (uri_scheme);
    }
  else
    {
      g_autoptr(GFile) file = g_file_new_for_uri (arg_uri);
      g_autoptr(GFileInfo) info = g_file_query_info (file,
                                                     G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                                     0,
                                                     NULL,
                                                     NULL);
      content_type = g_strdup (g_file_info_get_content_type (info));
    }
  g_free (uri_scheme);

  app_info = g_app_info_get_default_for_type (content_type, FALSE);

  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);

  handle = app_dialog_handle_new (arg_app_id, arg_sender, window, G_DBUS_INTERFACE_SKELETON (object));

  g_signal_connect_swapped (window, "delete-event", G_CALLBACK (app_dialog_handle_close), handle);

  handle->uri = g_strdup (arg_uri);
  handle->content_type = content_type;
  g_set_object (&handle->default_app_info, app_info);

  header = gtk_header_bar_new ();
  gtk_widget_show (header);
  gtk_header_bar_set_title (GTK_HEADER_BAR (header), "Open a URI");
  gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
  gtk_window_set_titlebar (GTK_WINDOW (window), header);

  grid = gtk_grid_new ();
  gtk_widget_show (grid);
  gtk_container_add (GTK_CONTAINER (window), grid);
  if (strcmp (arg_app_id, "") == 0)
    {
      str = g_strdup_printf ("An application wants to open %s", arg_uri);
    }
  else
    {
      g_autofree char *desktop_id = g_strconcat (arg_app_id, ".desktop", NULL);
      g_autoptr(GAppInfo) app_info = (GAppInfo *)g_desktop_app_info_new (desktop_id);
      str = g_strdup_printf ("%s wants to open %s", g_app_info_get_display_name (app_info), arg_uri);
    }
  label = gtk_label_new (str);
  g_free (str);
  gtk_widget_show (label);
  gtk_grid_attach (GTK_GRID (grid), label, 1, 1, 3, 1);

  if (app_info)
    {
      str = g_strdup_printf ("Open with %s", g_app_info_get_display_name (app_info));

      button = gtk_button_new_with_label (str);
      g_signal_connect (button, "clicked", G_CALLBACK (open_default), handle);
      gtk_widget_show (button);
      gtk_grid_attach (GTK_GRID (grid), button, 1, 2, 1, 1);
    }

  button = gtk_button_new_with_label ("");
  g_signal_connect (button, "clicked", G_CALLBACK (open_other), handle);
  gtk_grid_attach (GTK_GRID (grid), button, 1, 3, 1, 1);
  handle->other_app_button = button;

  button = gtk_button_new_with_label ("Choose another application");
  g_signal_connect (button, "clicked", G_CALLBACK (open_appchooser), handle);
  gtk_widget_show (button);
  gtk_grid_attach (GTK_GRID (grid), button, 2, 2, 1, 1);

  gtk_widget_show (window);

  if (invocation)
    g_dbus_method_invocation_return_value (invocation, NULL);

  return TRUE;
}

gboolean
app_chooser_init (GDBusConnection *bus,
                  GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (flatpak_desktop_app_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-open-uri", G_CALLBACK (handle_app_chooser_open_uri), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         "/org/freedesktop/portal/desktop",
                                         error))
    return FALSE;

  return TRUE;
}
