#include "launcher.h"
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n-lib.h>

static const gchar *action_names[] = { "web", "desktop", "command" };

const gchar *
click_action_to_string (ClickAction action)
{
  return action_names[action];
}

ClickAction
click_action_from_string (const gchar *s)
{
  for (guint i = 0; i < G_N_ELEMENTS (action_names); i++)
    if (g_strcmp0 (s, action_names[i]) == 0)
      return (ClickAction) i;
  return CLICK_OPEN_WEB;
}

gboolean
launcher_is_claude_desktop (const gchar *id, const gchar *name)
{
  if (g_strcmp0 (id, "claude-desktop.desktop") == 0)
    return TRUE;
  return g_strcmp0 (name, "Claude") == 0 || g_strcmp0 (name, "Claude Desktop") == 0;
}

static GAppInfo *
find_claude_desktop (void)
{
  GDesktopAppInfo *exact = g_desktop_app_info_new ("claude-desktop.desktop");
  GList *all, *l;
  GAppInfo *found = NULL;

  if (exact != NULL)
    return G_APP_INFO (exact);
  all = g_app_info_get_all ();
  for (l = all; l != NULL && found == NULL; l = l->next)
    if (launcher_is_claude_desktop (g_app_info_get_id (l->data), g_app_info_get_name (l->data)))
      found = g_object_ref (l->data);
  g_list_free_full (all, g_object_unref);
  return found;
}

gboolean
launcher_run (ClickAction action, const gchar *command, GError **error)
{
  switch (action)
    {
    case CLICK_OPEN_DESKTOP:
      {
        GAppInfo *app = find_claude_desktop ();
        gboolean ok;

        if (app == NULL)
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("Claude Desktop not found"));
            return FALSE;
          }
        ok = g_app_info_launch (app, NULL, NULL, error);
        g_object_unref (app);
        return ok;
      }
    case CLICK_RUN_COMMAND:
      {
        gchar *trimmed = g_strstrip (g_strdup (command != NULL ? command : ""));
        gboolean ok;

        if (*trimmed == '\0')
          {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("No command set"));
            g_free (trimmed);
            return FALSE;
          }
        ok = g_spawn_command_line_async (trimmed, error);
        g_free (trimmed);
        return ok;
      }
    case CLICK_OPEN_WEB:
    default:
      return g_app_info_launch_default_for_uri (CLAUDE_URL, NULL, error);
    }
}
