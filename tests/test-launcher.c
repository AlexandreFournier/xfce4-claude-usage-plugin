#include <glib.h>
#include <glib/gstdio.h>
#include "launcher.h"

static void
test_action_strings (void)
{
  g_assert_cmpstr (click_action_to_string (CLICK_OPEN_WEB), ==, "web");
  g_assert_cmpstr (click_action_to_string (CLICK_OPEN_DESKTOP), ==, "desktop");
  g_assert_cmpstr (click_action_to_string (CLICK_RUN_COMMAND), ==, "command");
  g_assert_cmpint (click_action_from_string ("desktop"), ==, CLICK_OPEN_DESKTOP);
  g_assert_cmpint (click_action_from_string ("command"), ==, CLICK_RUN_COMMAND);
  g_assert_cmpint (click_action_from_string ("web"), ==, CLICK_OPEN_WEB);
  g_assert_cmpint (click_action_from_string ("n'importe quoi"), ==, CLICK_OPEN_WEB);
  g_assert_cmpint (click_action_from_string (NULL), ==, CLICK_OPEN_WEB);
}

static void
test_is_claude_desktop (void)
{
  g_assert_true (launcher_is_claude_desktop ("claude-desktop.desktop", "Claude Desktop"));
  g_assert_true (launcher_is_claude_desktop ("com.anthropic.claude.desktop", "Claude"));
  g_assert_false (launcher_is_claude_desktop ("claude-code-url-handler.desktop",
                                              "Claude Code URL Handler"));
  g_assert_false (launcher_is_claude_desktop ("firefox.desktop", "Firefox"));
  g_assert_false (launcher_is_claude_desktop (NULL, NULL));
}

static void
test_run_command (void)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("launcher-XXXXXX", NULL);
  gchar *marker = g_build_filename (dir, "ran", NULL);
  gchar *cmd = g_strdup_printf ("touch %s", marker);

  g_assert_true (launcher_run (CLICK_RUN_COMMAND, cmd, &err));
  g_assert_no_error (err);
  for (int i = 0; i < 100 && !g_file_test (marker, G_FILE_TEST_EXISTS); i++)
    g_usleep (20000);
  g_assert_true (g_file_test (marker, G_FILE_TEST_EXISTS));

  g_assert_false (launcher_run (CLICK_RUN_COMMAND, "   ", &err));
  g_assert_nonnull (err);
  g_clear_error (&err);
  g_assert_false (launcher_run (CLICK_RUN_COMMAND, "commande-inexistante-xyz", &err));
  g_assert_nonnull (err);
  g_clear_error (&err);

  g_remove (marker);
  g_rmdir (dir);
  g_free (cmd);
  g_free (marker);
  g_free (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/launcher/action-strings", test_action_strings);
  g_test_add_func ("/launcher/is-claude-desktop", test_is_claude_desktop);
  g_test_add_func ("/launcher/run-command", test_run_command);
  return g_test_run ();
}
