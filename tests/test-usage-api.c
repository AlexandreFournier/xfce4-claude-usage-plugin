#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include "usage-api.h"

static const gchar *RESPONSE =
  "{\"five_hour\":{\"utilization\":24.0,\"resets_at\":\"2026-10-08T12:40:00.233097+00:00\"},"
  "\"seven_day\":{\"utilization\":47,\"resets_at\":\"2026-10-12T08:00:00.233122+00:00\"},"
  "\"seven_day_opus\":null}";

static void
test_parse_nominal (void)
{
  ClaudeUsage u;
  GError *err = NULL;
  claude_usage_init (&u);
  g_assert_true (usage_parse_json (RESPONSE, -1, &u, &err));
  g_assert_no_error (err);
  g_assert_cmpfloat_with_epsilon (u.five_hour_pct, 24.0, 1e-9);
  g_assert_cmpfloat_with_epsilon (u.seven_day_pct, 47.0, 1e-9);
  g_assert_nonnull (u.five_hour_reset);
  g_assert_cmpint (g_date_time_get_hour (u.five_hour_reset), ==, 12);
  g_assert_cmpint (g_date_time_get_minute (u.five_hour_reset), ==, 40);
  g_assert_cmpint (g_date_time_get_day_of_month (u.seven_day_reset), ==, 12);
  claude_usage_clear (&u);
  g_assert_null (u.five_hour_reset);
}

static void
test_parse_missing (void)
{
  ClaudeUsage u;
  claude_usage_init (&u);
  g_assert_true (usage_parse_json ("{\"five_hour\":null,\"seven_day\":{\"utilization\":5,\"resets_at\":null}}",
                                   -1, &u, NULL));
  g_assert_cmpfloat (u.five_hour_pct, <, 0);
  g_assert_null (u.five_hour_reset);
  g_assert_cmpfloat_with_epsilon (u.seven_day_pct, 5.0, 1e-9);
  g_assert_null (u.seven_day_reset);
  claude_usage_clear (&u);
}

static void
test_parse_invalid (void)
{
  ClaudeUsage u;
  GError *err = NULL;
  claude_usage_init (&u);
  g_assert_false (usage_parse_json ("pas du json", -1, &u, &err));
  g_assert_nonnull (err);
  g_clear_error (&err);
  g_assert_false (usage_parse_json ("[1,2]", -1, &u, &err));
  g_assert_nonnull (err);
  g_clear_error (&err);
}

static void
test_copy (void)
{
  ClaudeUsage a, b;
  claude_usage_init (&a);
  claude_usage_init (&b);
  usage_parse_json (RESPONSE, -1, &a, NULL);
  claude_usage_copy (&a, &b);
  claude_usage_clear (&a);
  g_assert_cmpfloat_with_epsilon (b.five_hour_pct, 24.0, 1e-9);
  g_assert_cmpint (g_date_time_get_hour (b.five_hour_reset), ==, 12);
  claude_usage_clear (&b);
}

static gchar *
write_creds (const gchar *content)
{
  gchar *path = NULL;
  gint fd = g_file_open_tmp ("creds-XXXXXX.json", &path, NULL);
  g_assert_cmpint (fd, >=, 0);
  close (fd);
  g_assert_true (g_file_set_contents (path, content, -1, NULL));
  return path;
}

static void
test_token (void)
{
  UsageStatus st;
  gchar *path = write_creds ("{\"claudeAiOauth\":{\"accessToken\":\"tok\",\"expiresAt\":2000}}");

  gchar *tok = usage_read_token (path, 1000, &st);
  g_assert_cmpstr (tok, ==, "tok");
  g_assert_cmpint (st, ==, USAGE_OK);
  g_free (tok);

  g_assert_null (usage_read_token (path, 3000, &st));
  g_assert_cmpint (st, ==, USAGE_ERR_EXPIRED);
  g_unlink (path);
  g_free (path);

  path = write_creds ("{\"other\":1}");
  g_assert_null (usage_read_token (path, 1000, &st));
  g_assert_cmpint (st, ==, USAGE_ERR_NO_TOKEN);
  g_unlink (path);
  g_free (path);

  g_assert_null (usage_read_token ("/nonexistent/creds.json", 1000, &st));
  g_assert_cmpint (st, ==, USAGE_ERR_NO_TOKEN);
}

static void
load_fixture (ClaudeUsage *u)
{
  gchar *data = NULL;
  gsize len;
  GError *err = NULL;

  g_assert_true (g_file_get_contents (FIXTURE_DIR "/usage-full.json", &data, &len, &err));
  g_assert_true (usage_parse_json (data, len, u, &err));
  g_assert_no_error (err);
  g_free (data);
}

static void
test_parse_full (void)
{
  ClaudeUsage u;
  const UsageLimit *l;
  const UsageShare *sh;

  claude_usage_init (&u);
  load_fixture (&u);
  g_assert_cmpfloat_with_epsilon (u.five_hour_pct, 38.0, 1e-9);
  g_assert_cmpfloat_with_epsilon (u.seven_day_pct, 49.0, 1e-9);

  g_assert_cmpuint (u.limits->len, ==, 3);
  l = g_ptr_array_index (u.limits, 0);
  g_assert_cmpstr (l->kind, ==, "session");
  g_assert_null (l->scope);
  g_assert_cmpfloat_with_epsilon (l->pct, 38.0, 1e-9);
  g_assert_cmpint (g_date_time_get_minute (l->reset), ==, 39);
  l = g_ptr_array_index (u.limits, 2);
  g_assert_cmpstr (l->kind, ==, "weekly_scoped");
  g_assert_cmpstr (l->scope, ==, "Fable");
  g_assert_cmpfloat_with_epsilon (l->pct, 2.0, 1e-9);

  g_assert_cmpuint (u.breakdown->len, ==, 4);
  sh = g_ptr_array_index (u.breakdown, 0);
  g_assert_cmpstr (sh->name, ==, "Claude Code");
  g_assert_cmpfloat_with_epsilon (sh->pct, 97.0, 1e-9);
  claude_usage_clear (&u);
  g_assert_null (u.limits);
  g_assert_null (u.breakdown);
}

/* Sans limits[] : repli sur five_hour / seven_day */
static void
test_limits_fallback (void)
{
  ClaudeUsage u;
  const UsageLimit *l;

  claude_usage_init (&u);
  g_assert_true (usage_parse_json (RESPONSE, -1, &u, NULL));
  g_assert_cmpuint (u.limits->len, ==, 2);
  l = g_ptr_array_index (u.limits, 0);
  g_assert_cmpstr (l->kind, ==, "session");
  g_assert_cmpfloat_with_epsilon (l->pct, 24.0, 1e-9);
  g_assert_nonnull (l->reset);
  l = g_ptr_array_index (u.limits, 1);
  g_assert_cmpstr (l->kind, ==, "weekly_all");
  g_assert_cmpfloat_with_epsilon (l->pct, 47.0, 1e-9);
  g_assert_cmpuint (u.breakdown->len, ==, 0);
  claude_usage_clear (&u);
}

/* Entrées inconnues ou abîmées : on garde ce qui a un kind */
static void
test_limits_tolerant (void)
{
  ClaudeUsage u;
  const UsageLimit *l;

  claude_usage_init (&u);
  g_assert_true (usage_parse_json (
    "{\"limits\":[{\"kind\":\"daily_magic\",\"percent\":12},42,{\"percent\":\"x\"},"
    "{\"kind\":\"session\",\"percent\":\"oops\",\"scope\":{\"model\":7}}],"
    "\"seven_day_breakdown\":{\"rows\":[{\"display_name\":\"A\",\"percent\":5},{\"percent\":3},7]}}",
    -1, &u, NULL));
  g_assert_cmpuint (u.limits->len, ==, 2);
  l = g_ptr_array_index (u.limits, 0);
  g_assert_cmpstr (l->kind, ==, "daily_magic");
  g_assert_cmpfloat_with_epsilon (l->pct, 12.0, 1e-9);
  l = g_ptr_array_index (u.limits, 1);
  g_assert_cmpfloat (l->pct, <, 0);
  g_assert_null (l->scope);
  g_assert_cmpuint (u.breakdown->len, ==, 1);
  claude_usage_clear (&u);
}

/* Sans five_hour / seven_day : les barres se rabattent sur limits[] */
static void
test_bars_from_limits (void)
{
  ClaudeUsage u;

  claude_usage_init (&u);
  g_assert_true (usage_parse_json (
    "{\"limits\":[{\"kind\":\"session\",\"percent\":10,\"resets_at\":\"2026-10-08T12:00:00Z\"},"
    "{\"kind\":\"weekly_all\",\"percent\":20}]}", -1, &u, NULL));
  g_assert_cmpfloat_with_epsilon (u.five_hour_pct, 10.0, 1e-9);
  g_assert_nonnull (u.five_hour_reset);
  g_assert_cmpfloat_with_epsilon (u.seven_day_pct, 20.0, 1e-9);
  g_assert_null (u.seven_day_reset);
  claude_usage_clear (&u);
}

static void
test_copy_deep (void)
{
  ClaudeUsage a, b;
  const UsageLimit *l;

  claude_usage_init (&a);
  claude_usage_init (&b);
  load_fixture (&a);
  claude_usage_copy (&a, &b);
  claude_usage_clear (&a);
  g_assert_cmpuint (b.limits->len, ==, 3);
  l = g_ptr_array_index (b.limits, 2);
  g_assert_cmpstr (l->scope, ==, "Fable");
  g_assert_cmpuint (b.breakdown->len, ==, 4);
  claude_usage_clear (&b);
}

static void
check_plan (const gchar *type, const gchar *tier, const gchar *expected)
{
  gchar *s = usage_plan_label (type, tier);
  g_assert_cmpstr (s, ==, expected);
  g_free (s);
}

static void
test_plan_label (void)
{
  gchar *path, *plan;

  check_plan ("max", "default_claude_max_5x", "Max (5x)");
  check_plan ("max", "default_claude_max_20x", "Max (20x)");
  check_plan ("pro", NULL, "Pro");
  check_plan ("team", "whatever", "Team");
  check_plan (NULL, NULL, NULL);
  check_plan ("", "x", NULL);

  path = write_creds ("{\"claudeAiOauth\":{\"subscriptionType\":\"max\",\"rateLimitTier\":\"default_claude_max_5x\"}}");
  plan = usage_read_plan (path);
  g_assert_cmpstr (plan, ==, "Max (5x)");
  g_free (plan);
  g_unlink (path);
  g_free (path);
  g_assert_null (usage_read_plan ("/nonexistent/creds.json"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/usage/parse-nominal", test_parse_nominal);
  g_test_add_func ("/usage/parse-missing", test_parse_missing);
  g_test_add_func ("/usage/parse-invalid", test_parse_invalid);
  g_test_add_func ("/usage/copy", test_copy);
  g_test_add_func ("/usage/token", test_token);
  g_test_add_func ("/usage/parse-full", test_parse_full);
  g_test_add_func ("/usage/limits-fallback", test_limits_fallback);
  g_test_add_func ("/usage/limits-tolerant", test_limits_tolerant);
  g_test_add_func ("/usage/bars-from-limits", test_bars_from_limits);
  g_test_add_func ("/usage/copy-deep", test_copy_deep);
  g_test_add_func ("/usage/plan-label", test_plan_label);
  return g_test_run ();
}
