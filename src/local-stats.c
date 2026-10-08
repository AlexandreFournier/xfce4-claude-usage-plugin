#include "local-stats.h"
#include "pricing.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#define MAX_AGE_DAYS 31
#define DAY 86400

typedef struct {
  gint64 ts;
  guint64 input, output, cache_write, cache_read;
  gdouble cost;
} Record;

struct _LocalStatsCache {
  gchar *projects_dir;
  GHashTable *offsets; /* chemin -> gint64* (octets déjà lus) */
  GHashTable *seen;    /* clé message.id:requestId -> gint64* (ts) */
  GArray *records;
};

LocalStatsCache *
local_stats_cache_new (const gchar *projects_dir)
{
  LocalStatsCache *c = g_new0 (LocalStatsCache, 1);
  c->projects_dir = g_strdup (projects_dir);
  c->offsets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  c->seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  c->records = g_array_new (FALSE, FALSE, sizeof (Record));
  return c;
}

void
local_stats_cache_free (LocalStatsCache *c)
{
  if (c == NULL)
    return;
  g_free (c->projects_dir);
  g_hash_table_unref (c->offsets);
  g_hash_table_unref (c->seen);
  g_array_unref (c->records);
  g_free (c);
}

static JsonObject *
get_object (JsonObject *o, const gchar *name)
{
  JsonNode *n;
  if (o == NULL || !json_object_has_member (o, name))
    return NULL;
  n = json_object_get_member (o, name);
  return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static const gchar *
get_string (JsonObject *o, const gchar *name)
{
  JsonNode *n;
  if (o == NULL || !json_object_has_member (o, name))
    return NULL;
  n = json_object_get_member (o, name);
  if (!JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (n);
}

static guint64
get_uint (JsonObject *o, const gchar *name)
{
  JsonNode *n;
  gint64 v;
  if (o == NULL || !json_object_has_member (o, name))
    return 0;
  n = json_object_get_member (o, name);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return 0;
  v = json_node_get_int (n);
  return v > 0 ? (guint64) v : 0;
}

static void
parse_line (LocalStatsCache *c, const gchar *line, gint64 min_ts)
{
  g_autoptr (JsonParser) parser = NULL;
  g_autoptr (GDateTime) dt = NULL;
  JsonNode *root;
  JsonObject *o, *msg, *usage, *cc;
  const gchar *ts_str, *id, *req;
  guint64 cw, cw1h;
  Record r = { 0 };

  if (strstr (line, "\"usage\"") == NULL)
    return;
  parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, line, -1, NULL))
    return;
  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return;
  o = json_node_get_object (root);
  if (g_strcmp0 (get_string (o, "type"), "assistant") != 0)
    return;
  msg = get_object (o, "message");
  usage = get_object (msg, "usage");
  ts_str = get_string (o, "timestamp");
  if (usage == NULL || ts_str == NULL)
    return;
  dt = g_date_time_new_from_iso8601 (ts_str, NULL);
  if (dt == NULL)
    return;
  r.ts = g_date_time_to_unix (dt);
  if (r.ts < min_ts)
    return;

  id = get_string (msg, "id");
  if (id != NULL)
    {
      req = get_string (o, "requestId");
      gchar *key = g_strconcat (id, ":", req ? req : "", NULL);
      gint64 *ts;
      if (g_hash_table_contains (c->seen, key))
        {
          g_free (key);
          return; /* déjà compté */
        }
      ts = g_new (gint64, 1);
      *ts = r.ts;
      g_hash_table_insert (c->seen, key, ts);
    }

  r.input = get_uint (usage, "input_tokens");
  r.output = get_uint (usage, "output_tokens");
  r.cache_read = get_uint (usage, "cache_read_input_tokens");
  cw = get_uint (usage, "cache_creation_input_tokens");
  cc = get_object (usage, "cache_creation");
  cw1h = MIN (get_uint (cc, "ephemeral_1h_input_tokens"), cw);
  r.cache_write = cw;
  r.cost = pricing_cost (pricing_lookup (get_string (msg, "model")),
                         r.input, r.output, cw - cw1h, cw1h, r.cache_read);
  g_array_append_val (c->records, r);
}

static void
scan_file (LocalStatsCache *c, const gchar *path, gint64 min_ts)
{
  GStatBuf st;
  gint64 *off;
  FILE *f;
  gchar *buf, *line, *nl;
  gsize len, n;

  if (g_stat (path, &st) != 0)
    {
      g_hash_table_remove (c->offsets, path); /* fichier supprimé */
      return;
    }
  if ((gint64) st.st_mtime < min_ts)
    return;
  off = g_hash_table_lookup (c->offsets, path);
  if (off == NULL)
    {
      off = g_new0 (gint64, 1);
      g_hash_table_insert (c->offsets, g_strdup (path), off);
    }
  if ((gint64) st.st_size < *off)
    *off = 0; /* fichier tronqué : relire (les doublons sont filtrés) */
  if ((gint64) st.st_size == *off)
    return;

  f = g_fopen (path, "rb");
  if (f == NULL)
    return;
  if (fseek (f, (long) *off, SEEK_SET) != 0)
    {
      fclose (f);
      return;
    }
  len = (gsize) (st.st_size - *off);
  buf = g_malloc (len + 1);
  n = fread (buf, 1, len, f);
  fclose (f);
  buf[n] = '\0';

  line = buf;
  while ((nl = memchr (line, '\n', (gsize) (buf + n - line))) != NULL)
    {
      *nl = '\0';
      parse_line (c, line, min_ts);
      line = nl + 1;
    }
  *off += line - buf; /* la ligne incomplète éventuelle sera relue */
  g_free (buf);
}

static void
walk (LocalStatsCache *c, const gchar *dir, gint64 min_ts)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *name;

  if (d == NULL)
    return;
  while ((name = g_dir_read_name (d)) != NULL)
    {
      gchar *p = g_build_filename (dir, name, NULL);
      if (g_file_test (p, G_FILE_TEST_IS_DIR) && !g_file_test (p, G_FILE_TEST_IS_SYMLINK))
        walk (c, p, min_ts);
      else if (g_str_has_suffix (name, ".jsonl"))
        scan_file (c, p, min_ts);
      g_free (p);
    }
  g_dir_close (d);
}

static void
add (TokenTotals *t, const Record *r)
{
  t->input += r->input;
  t->output += r->output;
  t->cache_write += r->cache_write;
  t->cache_read += r->cache_read;
  t->cost_usd += r->cost;
}

static gboolean
seen_expired (gpointer key, gpointer value, gpointer user_data)
{
  return *(const gint64 *) value < *(const gint64 *) user_data;
}

void
local_stats_compute (LocalStatsCache *c, GDateTime *now, LocalStats *out)
{
  gint64 now_ts = g_date_time_to_unix (now);
  gint64 week_ts = now_ts - 7 * DAY;
  gint64 month_ts = now_ts - 30 * DAY;
  gint64 today_ts;
  GDateTime *midnight;

  walk (c, c->projects_dir, now_ts - MAX_AGE_DAYS * DAY);

  midnight = g_date_time_new (g_date_time_get_timezone (now),
                              g_date_time_get_year (now),
                              g_date_time_get_month (now),
                              g_date_time_get_day_of_month (now), 0, 0, 0);
  today_ts = g_date_time_to_unix (midnight);
  g_date_time_unref (midnight);

  memset (out, 0, sizeof *out);
  for (guint i = 0; i < c->records->len; i++)
    {
      const Record *r = &g_array_index (c->records, Record, i);
      if (r->ts >= month_ts)
        add (&out->month, r);
      if (r->ts >= week_ts)
        add (&out->week, r);
      if (r->ts >= today_ts)
        add (&out->today, r);
    }

  /* Purge : borne la mémoire d'un processus longue durée. */
  {
    gint64 cutoff = now_ts - MAX_AGE_DAYS * DAY;
    guint w = 0;
    for (guint i = 0; i < c->records->len; i++)
      {
        Record r = g_array_index (c->records, Record, i);
        if (r.ts >= cutoff)
          g_array_index (c->records, Record, w++) = r;
      }
    g_array_set_size (c->records, w);
    g_hash_table_foreach_remove (c->seen, seen_expired, &cutoff);
  }
}

guint
local_stats_cache_record_count (LocalStatsCache *c)
{
  return c->records->len;
}

guint64
token_totals_sum (const TokenTotals *t)
{
  return t->input + t->output + t->cache_write + t->cache_read;
}

gchar *
local_stats_format_tokens (guint64 n)
{
  if (n >= 999500)
    return g_strdup_printf ("%.1f M", n / 1e6);
  if (n >= 1000)
    return g_strdup_printf ("%.0f k", n / 1e3);
  return g_strdup_printf ("%" G_GUINT64_FORMAT, n);
}
