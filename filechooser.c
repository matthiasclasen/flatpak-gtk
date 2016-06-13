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

#include "appchooser.h"

static XdpDbusDocuments *documents = NULL;
static char *mountpoint = NULL;
static GHashTable *outstanding_handles = NULL;

typedef struct {
  char *id;
  char *app_id;
  char *sender;

  GtkWidget *dialog;
  GtkFileChooserAction action;
  gboolean multiple;

  int response;
  GSList *raw_uris;
  GSList *uris;

  GDBusInterfaceSkeleton *skeleton;

  gboolean allow_write;

} DialogHandle;

static DialogHandle *
dialog_handle_new (const char *app_id,
                   const char *sender,
                   GtkWidget *dialog,
                   GDBusInterfaceSkeleton *skeleton)
{
  DialogHandle *handle = g_new0 (DialogHandle, 1);
  guint32 r;

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

  handle->allow_write = TRUE;

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
  g_slist_free_full (handle->raw_uris, g_free);
  g_slist_free_full (handle->uris, g_free);
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
send_response (DialogHandle *handle)
{
  GVariant *options = g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0);

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
                                                   handle->response,
                                                   handle->uris ? (char *)handle->uris->data : "",
                                                   options));
    }
  else
    {
      g_auto(GStrv) uris = NULL;
      GSList *l;
      gint i;

      uris = g_new (char *, g_slist_length (handle->uris) + 1);
      for (l = handle->uris, i = 0; l; l = l->next)
        uris[i++] = l->data;
      uris[i] = NULL;

      g_slist_free (handle->uris);
      handle->uris = NULL;

      dialog_handler_emit_response (handle,
                                    "org.freedesktop.impl.portal.FileChooser",
                                    "OpenFilesResponse",
                                    g_variant_new ("(sou^as@a{sv})",
                                                   handle->sender,
                                                   handle->id,
                                                   handle->response,
                                                   uris,
                                                   options));
    }

  dialog_handle_close (handle);
}

static gboolean
app_can_access (DialogHandle *handle,
                const char *uri)
{
  //TODO
  if (strcmp (handle->app_id, "") == 0)
    return TRUE;
  else
    return FALSE;
}

static void
convert_one_uri (DialogHandle *handle,
                 const char *uri)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *doc_id = NULL;
  g_autofree char *path = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *dirname = NULL;
  GUnixFDList *fd_list = NULL;
  int fd, fd_in;
  g_autoptr(GFile) file = NULL;
  gboolean ret;
  const char *permissions[5];
  g_autofree char *fuse_path = NULL;
  int i;

  if (app_can_access (handle, uri))
    {
      handle->uris = g_slist_prepend (handle->uris, g_strdup (uri));
      return;
    }

  file = g_file_new_for_uri (uri);
  path = g_file_get_path (file);
  basename = g_path_get_basename (path);
  dirname = g_path_get_dirname (path);

  if (handle->action == GTK_FILE_CHOOSER_ACTION_SAVE)
    fd = open (dirname, O_PATH | O_CLOEXEC);
  else
    fd = open (path, O_PATH | O_CLOEXEC);

  if (fd == -1)
    {
      g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Failed to open %s", uri);
      goto out;
    }

  fd_list = g_unix_fd_list_new ();
  fd_in = g_unix_fd_list_append (fd_list, fd, &error);
  close (fd);

  if (fd_in == -1)
    goto out;

  i = 0;
  permissions[i++] = "read";
  if (!handle->allow_write)
    permissions[i++] = "write";
  permissions[i++] = "grant";
  permissions[i++] = NULL;

  if (handle->action == GTK_FILE_CHOOSER_ACTION_SAVE)
    ret = xdp_dbus_documents_call_add_named_sync (documents,
                                                  g_variant_new_handle (fd_in),
                                                  basename,
                                                  TRUE,
                                                  TRUE,
                                                  fd_list,
                                                  &doc_id,
                                                  NULL,
                                                  NULL,
                                                  &error);
  else
    ret = xdp_dbus_documents_call_add_sync (documents,
                                            g_variant_new_handle (fd_in),
                                            TRUE,
                                            TRUE,
                                            fd_list,
                                            &doc_id,
                                            NULL,
                                            NULL,
                                            &error);
  g_object_unref (fd_list);

  if (!ret)
    goto out;

  if (!xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                       doc_id,
                                                       handle->app_id,
                                                       permissions,
                                                       NULL,
                                                       &error))
     goto out;

  fuse_path = g_build_filename (mountpoint, doc_id, basename, NULL);
  handle->uris = g_slist_prepend (handle->uris, g_strconcat  ("file://", fuse_path, NULL));

out:
  if (error)
    g_warning ("Failed to convert %s: %s", uri, error->message);
}

static gboolean
convert_uris (gpointer data)
{
  DialogHandle *handle = data;

  if (handle->raw_uris)
    {
      GSList *first = handle->raw_uris;
      g_autofree char *uri = first->data;

      handle->raw_uris = first->next;
      first->next = NULL;
      g_slist_free_1 (first);

      convert_one_uri (handle, uri);

      return G_SOURCE_CONTINUE;
    }
  else
    {
      send_response (handle);

      return G_SOURCE_REMOVE;
    }
}

GtkFileFilter *
gtk_file_filter_from_gvariant (GVariant *variant)
{
  GtkFileFilter *filter;
  GVariantIter *iter;
  const char *name;
  int type;
  char *tmp;

  filter = gtk_file_filter_new ();

  g_variant_get (variant, "(&sa(us))", &name, &iter);

  gtk_file_filter_set_name (filter, name);

  while (g_variant_iter_next (iter, "(u&s)", &type, &tmp))
    {
      switch (type)
        {
        case 0:
          gtk_file_filter_add_pattern (filter, tmp);
          break;
        case 1:
          gtk_file_filter_add_mime_type (filter, tmp);
          break;
        default:
          break;
       }
    }
  g_variant_iter_free (iter);

  return filter;
}

static void
handle_file_chooser_open_response (GtkWidget *widget,
                                   int response,
                                   gpointer user_data)
{
  DialogHandle *handle = user_data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      /* Fall through */
    case GTK_RESPONSE_DELETE_EVENT:
      handle->response = 2;
      handle->raw_uris = NULL;
      break;

    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      handle->raw_uris = NULL;
      break;

    case GTK_RESPONSE_OK:
      handle->response = 0;
      handle->raw_uris = gtk_file_chooser_get_uris (GTK_FILE_CHOOSER (widget));
      break;
    }

  g_idle_add (convert_uris, handle);
}

static void
read_only_toggled (GtkToggleButton *button, gpointer user_data)
{
  DialogHandle *handle = user_data;

  handle->allow_write = !gtk_toggle_button_get_active (button);
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
  GVariantIter *iter;
  const char *current_name;
  const char *path;

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

  if (g_variant_lookup (arg_options, "filters", "a(sa(us))", &iter))
    {
      GVariant *variant;

      while (g_variant_iter_next (iter, "@(sa(us))", &variant))
        {
          GtkFileFilter *filter;

          filter = gtk_file_filter_from_gvariant (variant);
          gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);
          g_variant_unref (variant);
        }
      g_variant_iter_free (iter);
    }
  if (g_variant_lookup (arg_options, "current_name", "&s", &current_name))
    gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), current_name);
  /* TODO: is this useful ?
   * In a sandboxed situation, the current folder and current file
   * are likely in the fuse filesystem
   */
  if (g_variant_lookup (arg_options, "current_folder", "&ay", &path))
    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), path);
  if (g_variant_lookup (arg_options, "current_file", "&ay", &path))
    gtk_file_chooser_select_filename (GTK_FILE_CHOOSER (dialog), path);

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

  if (action == GTK_FILE_CHOOSER_ACTION_OPEN)
    {
      GtkWidget *readonly;

      readonly = gtk_check_button_new_with_label ("Open files read-only");
      gtk_widget_show (readonly);

      g_signal_connect (readonly, "toggled",
                        G_CALLBACK (read_only_toggled), handle);

      gtk_file_chooser_set_extra_widget (GTK_FILE_CHOOSER (dialog), readonly);
    }

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

gboolean
file_chooser_init (GDBusConnection *bus,
                   GError **error)
{
  GDBusInterfaceSkeleton *helper;

  outstanding_handles = g_hash_table_new (g_str_hash, g_str_equal);

  documents = xdp_dbus_documents_proxy_new_sync (bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, NULL);
  xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint, NULL, NULL);

  helper = G_DBUS_INTERFACE_SKELETON (flatpak_desktop_file_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-open-file", G_CALLBACK (handle_file_chooser_open), NULL);
  g_signal_connect (helper, "handle-open-files", G_CALLBACK (handle_file_chooser_open), NULL);
  g_signal_connect (helper, "handle-save-file", G_CALLBACK (handle_file_chooser_open), NULL);
  g_signal_connect (helper, "handle-close", G_CALLBACK (handle_file_chooser_close), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         "/org/freedesktop/portal/desktop",
                                         error))
    return FALSE;

  return TRUE;
}
