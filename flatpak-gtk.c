#include "config.h"

#include <locale.h>
#include <string.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include "flatpak-portal-dbus.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

static GMainLoop *loop = NULL;

static gboolean opt_verbose;
static gboolean opt_replace;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace", NULL },
  { NULL }
};

typedef struct {
  char *id;
  char *app_id;
  char *sender;

  GtkWidget *dialog;
  GtkFileChooserAction action;
  gboolean multiple;

  GDBusInterfaceSkeleton *skeleton;
} DialogHandle;

static GHashTable *outstanding_handles;

static DialogHandle *
dialog_handle_new (const char *app_id,
                   const char *sender,
                   GtkWidget *dialog,
                   GDBusInterfaceSkeleton *skeleton)
{
  DialogHandle *handle = g_new0 (DialogHandle, 1);
  guint32 r;

  if (outstanding_handles == NULL)
    outstanding_handles = g_hash_table_new (g_str_hash, g_str_equal);

  r = g_random_int ();
  do
    {
      g_free (handle->id);
      handle->id = g_strdup_printf ("/org/freedesktop/portal/desktop/%u", r);
    }
  while (g_hash_table_lookup (outstanding_handles, handle->id) != NULL);

  handle->app_id = g_strdup (app_id);
  handle->sender = g_strdup (sender);
  handle->dialog = g_object_ref (dialog);
  handle->skeleton = g_object_ref (skeleton);

  g_hash_table_insert (outstanding_handles, handle->id, handle);

  /* TODO: Track lifetime of sender and close handle */

  return handle;
}

static void
dialog_handle_free (DialogHandle *handle)
{
  g_hash_table_remove (outstanding_handles, handle->id);
  g_free (handle->id);
  g_free (handle->app_id);
  g_free (handle->sender);
  g_object_unref (handle->dialog);
  g_object_unref (handle->skeleton);
  g_free (handle);
}

static void
dialog_handle_close (DialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  dialog_handle_free (handle);
}

static DialogHandle *
dialog_handle_verify_call (GDBusMethodInvocation *invocation,
                           const char *arg_sender,
                           const char *arg_app_id,
                           const char *arg_handle,
                           GType skel_type)
{
  DialogHandle *handle;

  handle = g_hash_table_lookup (outstanding_handles, arg_handle);

  if (handle != NULL &&
      (/* App is unconfined */
       strcmp (arg_app_id, "") == 0 ||
       /* or same app */
       strcmp (handle->app_id, arg_app_id) == 0) &&
      g_type_check_instance_is_a ((GTypeInstance *)handle->skeleton, skel_type))
    return handle;

  g_dbus_method_invocation_return_dbus_error (invocation,
                                              "org.freedesktop.Flatpak.Error.NotFound",
                                              "No such handle");
  return NULL;
}

static void
dialog_handler_emit_response (DialogHandle *handle,
                              const char *interface,
                              const char *signal,
                              GVariant *arguments)
{
  g_dbus_connection_emit_signal (g_dbus_interface_skeleton_get_connection (handle->skeleton),
                                 "org.freedesktop.portal.Desktop",
                                 "/org/freedesktop/portal/desktop",
                                 interface, signal, arguments, NULL);
}

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("XDP: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

static void
handle_file_chooser_open_response (GtkWidget *widget,
                                   int response,
                                   gpointer user_data)
{
  DialogHandle *handle = user_data;
  char *uri = NULL;
  guint32 portal_response = 0;
  GVariant *options;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      /* Fall through */
    case GTK_RESPONSE_DELETE_EVENT:
      portal_response = 2;
      break;

    case GTK_RESPONSE_CANCEL:
      portal_response = 1;
      break;

    case GTK_RESPONSE_OK:
      uri = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (widget));
      portal_response = 0;
      break;
    }

  options = g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0);

  if (handle->action == GTK_FILE_CHOOSER_ACTION_SAVE ||
      (handle->action == GTK_FILE_CHOOSER_ACTION_OPEN && !handle->multiple))
    {
      const char *signal_name;

      if (handle->action == GTK_FILE_CHOOSER_ACTION_SAVE)
        signal_name = "SaveFileResponse";
      else
        signal_name = "OpenFileResponse";

      dialog_handler_emit_response (handle,
                                    "org.freedesktop.impl.portal.FileChooser",
                                    signal_name,
                                    g_variant_new ("(sous@a{sv})",
                                                   handle->sender,
                                                   handle->id,
                                                   portal_response,
                                                   uri ? uri : "",
                                                   options));
    }
  else
    {
      g_auto(GStrv) uris = NULL;

      if (response == GTK_RESPONSE_OK)
        {
          g_autoptr(GSList) list;
          GSList *l;
          gint i;

          list = gtk_file_chooser_get_uris (GTK_FILE_CHOOSER (widget));
          uris = g_new (char *, g_slist_length (list) + 1);
          for (l = list, i = 0; l; l = l->next)
            uris[i++] = l->data;
          uris[i] = NULL;
          g_slist_free (list);
        }
      else
        uris = g_new0 (char *, 1);

      dialog_handler_emit_response (handle,
                                    "org.freedesktop.impl.portal.FileChooser",
                                    "OpenFilesResponse",
                                    g_variant_new ("(sou^as@a{sv})",
                                                   handle->sender,
                                                   handle->id,
                                                   portal_response,
                                                   uris,
                                                   options));
    }

  dialog_handle_close (handle);
}

static gboolean
handle_file_chooser_open (FlatpakDesktopFileChooser *object,
                          GDBusMethodInvocation *invocation,
                          const gchar *arg_sender,
                          const gchar *arg_app_id,
                          const gchar *arg_parent_window,
                          const gchar *arg_title,
                          GVariant *arg_options)
{
  const gchar *method_name;
  GtkFileChooserAction action;
  gboolean multiple;
  GtkWidget *dialog;
  GdkWindow *foreign_parent = NULL;
  GtkWidget *fake_parent;
  DialogHandle *handle;
  FlatpakDesktopFileChooser *chooser = FLATPAK_DESKTOP_FILE_CHOOSER (g_dbus_method_invocation_get_user_data (invocation));
  const char *cancel_label;
  const char *accept_label;

  method_name = g_dbus_method_invocation_get_method_name (invocation);

  g_print ("%s, app_id: %s, object: %p, user_data: %p\n",
           method_name, arg_app_id, object,
           g_dbus_method_invocation_get_user_data (invocation));

  fake_parent = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_object_ref_sink (fake_parent);

  action = GTK_FILE_CHOOSER_ACTION_OPEN;
  multiple = FALSE;

  if (strcmp (method_name, "SaveFile") == 0)
    action = GTK_FILE_CHOOSER_ACTION_SAVE;
  else if (strcmp (method_name, "OpenFiles") == 0)
    multiple = TRUE;

  if (!g_variant_lookup (arg_options, "cancel_label", "&s", &cancel_label))
    cancel_label = "_Cancel";
  if (!g_variant_lookup (arg_options, "accept_label", "&s", &accept_label))
    accept_label = "_Open";

  dialog = gtk_file_chooser_dialog_new (arg_title, GTK_WINDOW (fake_parent), action,
                                        cancel_label, GTK_RESPONSE_CANCEL,
                                        accept_label, GTK_RESPONSE_OK,
                                        NULL);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
  gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER (dialog), multiple);

  g_object_unref (fake_parent);

#ifdef GDK_WINDOWING_X11
  if (g_str_has_prefix (arg_parent_window, "x11:"))
    {
      int xid;

      if (sscanf (arg_parent_window, "x11:%x", &xid) != 1)
        g_warning ("invalid xid");
      else
        foreign_parent = gdk_x11_window_foreign_new_for_display (gtk_widget_get_display (dialog), xid);
    }
#endif
  else
    g_warning ("Unhandled parent window type %s\n", arg_parent_window);

  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);

  handle = dialog_handle_new (arg_app_id, arg_sender, dialog, G_DBUS_INTERFACE_SKELETON (chooser));

  handle->dialog = dialog;
  handle->action = action;
  handle->multiple = multiple;

  g_signal_connect (G_OBJECT (dialog), "response",
                    G_CALLBACK (handle_file_chooser_open_response), handle);

  gtk_widget_realize (dialog);

  if (foreign_parent)
    gdk_window_set_transient_for (gtk_widget_get_window (dialog), foreign_parent);

  gtk_widget_show (dialog);

  flatpak_desktop_file_chooser_complete_open_file (chooser,
                                                   invocation,
                                                   handle->id);

  return TRUE;
}

static gboolean
handle_file_chooser_close (FlatpakDesktopFileChooser *object,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_sender,
                           const gchar *arg_app_id,
                           const gchar *arg_handle)
{
  DialogHandle *handle;

  handle = dialog_handle_verify_call (invocation, arg_sender, arg_app_id, arg_handle,
                                      FLATPAK_DESKTOP_TYPE_FILE_CHOOSER_SKELETON);
  if (handle != NULL)
    {
      dialog_handle_close (handle);
      flatpak_desktop_file_chooser_complete_close (object, invocation);
    }

  return TRUE;
}


static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  FlatpakDesktopFileChooser *helper;
  GError *error = NULL;

  helper = flatpak_desktop_file_chooser_skeleton_new ();

  g_signal_connect (helper, "handle-open-file", G_CALLBACK (handle_file_chooser_open), NULL);
  g_signal_connect (helper, "handle-open-files", G_CALLBACK (handle_file_chooser_open), NULL);
  g_signal_connect (helper, "handle-save-file", G_CALLBACK (handle_file_chooser_open), NULL);
  g_signal_connect (helper, "handle-close", G_CALLBACK (handle_file_chooser_close), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         "/org/freedesktop/portal/desktop",
                                         &error))
    {
      g_warning ("error: %s\n", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("org.freedesktop.impl.portal.desktop.gtk acquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_main_loop_quit (loop);
}

int
main (int argc, char *argv[])
{
  guint owner_id;
  g_autoptr(GError) error = NULL;
  GDBusConnection  *session_bus;
  GOptionContext *context;

  setlocale (LC_ALL, "");

  /* Avoid even loading gvfs to avoid accidental confusion */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  gtk_init (&argc, &argv);

  context = g_option_context_new ("- file chooser portal");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("option parsing failed: %s\n", error->message);
      return 1;
    }

  if (opt_verbose)
    g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname (argv[0]);

  loop = g_main_loop_new (NULL, FALSE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s\n", error->message);
      return 2;
    }

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.impl.portal.desktop.gtk",
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  return 0;
}
