#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include "local-stats.h"

#define LINE(ts, id, req, model, in, out, cw, cr) \
  "{\"type\":\"assistant\",\"timestamp\":\"" ts "\",\"requestId\":\"" req "\"," \
  "\"message\":{\"id\":\"" id "\",\"model\":\"" model "\",\"usage\":{" \
  "\"input_tokens\":" #in ",\"output_tokens\":" #out "," \
  "\"cache_creation_input_tokens\":" #cw ",\"cache_read_input_tokens\":" #cr "}}}\n"

static gchar *tmpdir;
static gchar *file_a;

static void
setup_tree (void)
{
  gchar *sub;

  tmpdir = g_dir_make_tmp ("stats-XXXXXX", NULL);
  sub = g_build_filename (tmpdir, "proj", "sess", "subagents", NULL);
  g_mkdir_with_parents (sub, 0700);
  file_a = g_build_filename (tmpdir, "proj", "a.jsonl", NULL);

  g_assert_true (g_file_set_contents (file_a,
    LINE ("2026-10-08T11:00:00.000Z", "m1", "r1", "claude-opus-5-5", 1000000, 0, 0, 0)
    LINE ("2026-10-08T11:00:00.000Z", "m1", "r1", "claude-opus-5-5", 1000000, 0, 0, 0) /* doublon */
    LINE ("2026-10-05T12:00:00.000Z", "m2", "r2", "claude-sonnet-5-5", 0, 1000000, 0, 0)
    LINE ("2026-09-18T12:00:00.000Z", "m3", "r3", "claude-haiku-5-5", 1000000, 0, 0, 0)
    LINE ("2026-08-29T12:00:00.000Z", "m4", "r4", "claude-opus-5-5", 1000000, 0, 0, 0) /* > 30 j */
    "{\"type\":\"user\",\"message\":{\"usage\":{}}}\n"
    "garbage{\"usage\"\n",
    -1, NULL));

  gchar *file_b = g_build_filename (sub, "b.jsonl", NULL);
  g_assert_true (g_file_set_contents (file_b,
    LINE ("2026-10-08T10:00:00.000Z", "m9", "r9", "claude-opus-5-5", 0, 0, 0, 1000000),
    -1, NULL));
  g_free (file_b);
  g_free (sub);
}

static GDateTime *
fixed_now (void)
{
  return g_date_time_new_from_iso8601 ("2026-10-08T12:00:00Z", NULL);
}

static void
test_compute (void)
{
  LocalStatsCache *c = local_stats_cache_new (tmpdir);
  GDateTime *now = fixed_now ();
  LocalStats s;

  local_stats_compute (c, now, &s);

  g_assert_cmpuint (s.today.input, ==, 1000000);
  g_assert_cmpuint (s.today.cache_read, ==, 1000000);
  g_assert_cmpfloat_with_epsilon (s.today.cost_usd, 4.20, 1e-6);

  g_assert_cmpuint (s.week.output, ==, 1000000);
  g_assert_cmpfloat_with_epsilon (s.week.cost_usd, 14.20, 1e-6);

  g_assert_cmpuint (s.month.input, ==, 2000000);
  g_assert_cmpfloat_with_epsilon (s.month.cost_usd, 14.30, 1e-6);
  g_assert_cmpuint (token_totals_sum (&s.month), ==, 4000000);

  g_date_time_unref (now);
  local_stats_cache_free (c);
}

static void
test_incremental (void)
{
  LocalStatsCache *c = local_stats_cache_new (tmpdir);
  GDateTime *now = fixed_now ();
  LocalStats s;
  FILE *f;

  local_stats_compute (c, now, &s);
  g_assert_cmpfloat_with_epsilon (s.today.cost_usd, 4.20, 1e-6);

  /* ligne incomplète : ignorée tant qu'elle n'est pas terminée */
  f = g_fopen (file_a, "ab");
  fputs ("{\"type\":\"assistant\",\"timestamp\":\"2026-10-08T11:30:00.000Z\",", f);
  fclose (f);
  local_stats_compute (c, now, &s);
  g_assert_cmpfloat_with_epsilon (s.today.cost_usd, 4.20, 1e-6);

  f = g_fopen (file_a, "ab");
  fputs ("\"requestId\":\"r5\",\"message\":{\"id\":\"m5\",\"model\":\"claude-opus-5-5\","
         "\"usage\":{\"input_tokens\":0,\"output_tokens\":1000000}}}\n", f);
  fclose (f);
  local_stats_compute (c, now, &s);
  g_assert_cmpfloat_with_epsilon (s.today.cost_usd, 24.20, 1e-6);

  g_date_time_unref (now);
  local_stats_cache_free (c);
}

static void
test_prune (void)
{
  LocalStatsCache *c = local_stats_cache_new (tmpdir);
  GDateTime *now = fixed_now ();
  GDateTime *later = g_date_time_add_days (now, 60);
  LocalStats s;

  local_stats_compute (c, now, &s);
  g_assert_cmpuint (local_stats_cache_record_count (c), >, 0);

  local_stats_compute (c, later, &s);
  g_assert_cmpuint (s.month.input + s.month.output + s.month.cache_read, ==, 0);
  g_assert_cmpuint (local_stats_cache_record_count (c), ==, 0);

  g_date_time_unref (later);
  g_date_time_unref (now);
  local_stats_cache_free (c);
}

static void
test_format_tokens (void)
{
  gchar *s;
  s = local_stats_format_tokens (42);        g_assert_cmpstr (s, ==, "42");    g_free (s);
  s = local_stats_format_tokens (850000);    g_assert_cmpstr (s, ==, "850 k"); g_free (s);
  s = local_stats_format_tokens (1234567);   g_assert_cmpstr (s, ==, "1.2 M"); g_free (s);
  s = local_stats_format_tokens (999999);    g_assert_cmpstr (s, ==, "1.0 M"); g_free (s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  setup_tree ();
  g_test_add_func ("/stats/compute", test_compute);
  g_test_add_func ("/stats/incremental", test_incremental);
  g_test_add_func ("/stats/prune", test_prune);
  g_test_add_func ("/stats/format-tokens", test_format_tokens);
  return g_test_run ();
}
