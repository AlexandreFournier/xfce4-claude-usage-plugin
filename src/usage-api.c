#include "usage-api.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <glib/gi18n-lib.h>
#include <string.h>

#define USAGE_URL "https://api.anthropic.com/api/oauth/usage"

void
claude_usage_init (ClaudeUsage *u)
{
  u->five_hour_pct = -1;
  u->five_hour_reset = NULL;
  u->seven_day_pct = -1;
  u->seven_day_reset = NULL;
  u->limits = NULL;
  u->breakdown = NULL;
}

static void
usage_limit_free (gpointer p)
{
  UsageLimit *l = p;
  g_free (l->kind);
  g_free (l->scope);
  if (l->reset != NULL)
    g_date_time_unref (l->reset);
  g_free (l);
}

static void
usage_share_free (gpointer p)
{
  UsageShare *s = p;
  g_free (s->name);
  g_free (s);
}

static UsageLimit *
usage_limit_new (const gchar *kind, const gchar *scope, gdouble pct, GDateTime *reset)
{
  UsageLimit *l = g_new0 (UsageLimit, 1);
  l->kind = g_strdup (kind);
  l->scope = g_strdup (scope);
  l->pct = pct;
  l->reset = reset != NULL ? g_date_time_ref (reset) : NULL;
  return l;
}

void
claude_usage_clear (ClaudeUsage *u)
{
  g_clear_pointer (&u->five_hour_reset, g_date_time_unref);
  g_clear_pointer (&u->seven_day_reset, g_date_time_unref);
  u->five_hour_pct = -1;
  u->seven_day_pct = -1;
  g_clear_pointer (&u->limits, g_ptr_array_unref);
  g_clear_pointer (&u->breakdown, g_ptr_array_unref);
}

void
claude_usage_copy (const ClaudeUsage *src, ClaudeUsage *dst)
{
  claude_usage_clear (dst);
  dst->five_hour_pct = src->five_hour_pct;
  dst->seven_day_pct = src->seven_day_pct;
  dst->five_hour_reset = src->five_hour_reset ? g_date_time_ref (src->five_hour_reset) : NULL;
  dst->seven_day_reset = src->seven_day_reset ? g_date_time_ref (src->seven_day_reset) : NULL;
  if (src->limits != NULL)
    {
      dst->limits = g_ptr_array_new_with_free_func (usage_limit_free);
      for (guint i = 0; i < src->limits->len; i++)
        {
          const UsageLimit *l = g_ptr_array_index (src->limits, i);
          g_ptr_array_add (dst->limits, usage_limit_new (l->kind, l->scope, l->pct, l->reset));
        }
    }
  if (src->breakdown != NULL)
    {
      dst->breakdown = g_ptr_array_new_with_free_func (usage_share_free);
      for (guint i = 0; i < src->breakdown->len; i++)
        {
          const UsageShare *sh = g_ptr_array_index (src->breakdown, i);
          UsageShare *c = g_new0 (UsageShare, 1);
          c->name = g_strdup (sh->name);
          c->pct = sh->pct;
          g_ptr_array_add (dst->breakdown, c);
        }
    }
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

/* Nombre JSON (entier ou flottant) ; -1 si absent ou d'un autre type */
static gdouble
get_number (JsonObject *o, const gchar *name)
{
  JsonNode *n;
  GType t;

  if (o == NULL || !json_object_has_member (o, name))
    return -1;
  n = json_object_get_member (o, name);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return -1;
  t = json_node_get_value_type (n);
  if (t != G_TYPE_INT64 && t != G_TYPE_DOUBLE)
    return -1;
  return json_node_get_double (n);
}

static GDateTime *
get_datetime (JsonObject *o, const gchar *name)
{
  const gchar *r = get_string (o, name);
  return r != NULL ? g_date_time_new_from_iso8601 (r, NULL) : NULL;
}

static void
parse_window (JsonObject *root, const gchar *name, gdouble *pct, GDateTime **reset)
{
  JsonObject *w = get_object (root, name);

  *pct = get_number (w, "utilization");
  *reset = get_datetime (w, "resets_at");
}

static JsonArray *
get_array (JsonObject *o, const gchar *name)
{
  JsonNode *n;
  if (o == NULL || !json_object_has_member (o, name))
    return NULL;
  n = json_object_get_member (o, name);
  return JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
}

static JsonObject *
array_object (JsonArray *a, guint i)
{
  JsonNode *n = json_array_get_element (a, i);
  return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

/* limits[] : chaque entrée doit au moins avoir un kind ; le reste est facultatif */
static void
parse_limits (JsonObject *root, GPtrArray *out)
{
  JsonArray *a = get_array (root, "limits");

  for (guint i = 0; a != NULL && i < json_array_get_length (a); i++)
    {
      JsonObject *e = array_object (a, i);
      const gchar *kind = get_string (e, "kind");
      JsonObject *model = get_object (get_object (e, "scope"), "model");
      GDateTime *reset;

      if (kind == NULL)
        continue;
      reset = get_datetime (e, "resets_at");
      g_ptr_array_add (out, usage_limit_new (kind, get_string (model, "display_name"),
                                             get_number (e, "percent"), reset));
      if (reset != NULL)
        g_date_time_unref (reset);
    }
}

static void
parse_breakdown (JsonObject *root, GPtrArray *out)
{
  JsonArray *a = get_array (get_object (root, "seven_day_breakdown"), "rows");

  for (guint i = 0; a != NULL && i < json_array_get_length (a); i++)
    {
      JsonObject *e = array_object (a, i);
      const gchar *name = get_string (e, "display_name");
      UsageShare *sh;

      if (name == NULL)
        continue;
      sh = g_new0 (UsageShare, 1);
      sh->name = g_strdup (name);
      sh->pct = get_number (e, "percent");
      g_ptr_array_add (out, sh);
    }
}

static const UsageLimit *
find_limit (GPtrArray *limits, const gchar *kind)
{
  for (guint i = 0; i < limits->len; i++)
    {
      const UsageLimit *l = g_ptr_array_index (limits, i);
      if (g_strcmp0 (l->kind, kind) == 0)
        return l;
    }
  return NULL;
}

/* Complète une fenêtre (barres du panneau) à partir de limits[] si elle manque */
static void
fill_window_from_limits (GPtrArray *limits, const gchar *kind, gdouble *pct, GDateTime **reset)
{
  const UsageLimit *l = find_limit (limits, kind);

  if (*pct >= 0 || l == NULL)
    return;
  *pct = l->pct;
  if (*reset == NULL && l->reset != NULL)
    *reset = g_date_time_ref (l->reset);
}

gboolean
usage_parse_json (const gchar *data, gssize len, ClaudeUsage *out, GError **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonObject *o;

  if (!json_parser_load_from_data (parser, data, len, error))
    return FALSE;
  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("unexpected response"));
      return FALSE;
    }
  o = json_node_get_object (root);
  claude_usage_clear (out);
  parse_window (o, "five_hour", &out->five_hour_pct, &out->five_hour_reset);
  parse_window (o, "seven_day", &out->seven_day_pct, &out->seven_day_reset);

  out->limits = g_ptr_array_new_with_free_func (usage_limit_free);
  out->breakdown = g_ptr_array_new_with_free_func (usage_share_free);
  parse_limits (o, out->limits);
  parse_breakdown (o, out->breakdown);

  if (out->limits->len == 0)
    {
      /* ancien format : on reconstruit la liste depuis les fenêtres */
      if (out->five_hour_pct >= 0)
        g_ptr_array_add (out->limits, usage_limit_new ("session", NULL, out->five_hour_pct,
                                                       out->five_hour_reset));
      if (out->seven_day_pct >= 0)
        g_ptr_array_add (out->limits, usage_limit_new ("weekly_all", NULL, out->seven_day_pct,
                                                       out->seven_day_reset));
    }
  else
    {
      fill_window_from_limits (out->limits, "session", &out->five_hour_pct, &out->five_hour_reset);
      fill_window_from_limits (out->limits, "weekly_all", &out->seven_day_pct, &out->seven_day_reset);
    }
  return TRUE;
}

gchar *
usage_plan_label (const gchar *subscription_type, const gchar *rate_limit_tier)
{
  gchar *name, *mult = NULL, *label;
  const gchar *last;

  if (subscription_type == NULL || *subscription_type == '\0')
    return NULL;
  name = g_strdup (subscription_type);
  name[0] = g_ascii_toupper (name[0]);

  /* "default_claude_max_5x" → "5x" */
  last = rate_limit_tier != NULL ? strrchr (rate_limit_tier, '_') : NULL;
  if (last != NULL && g_str_has_suffix (last, "x") && g_ascii_isdigit (last[1]))
    mult = g_strdup (last + 1);

  label = mult != NULL ? g_strdup_printf ("%s (%s)", name, mult) : g_strdup (name);
  g_free (name);
  g_free (mult);
  return label;
}

gchar *
usage_read_plan (const gchar *credentials_path)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonObject *oauth;

  if (!json_parser_load_from_file (parser, credentials_path, NULL))
    return NULL;
  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;
  oauth = get_object (json_node_get_object (root), "claudeAiOauth");
  return usage_plan_label (get_string (oauth, "subscriptionType"),
                           get_string (oauth, "rateLimitTier"));
}

gchar *
usage_read_token (const gchar *credentials_path, gint64 now_ms, UsageStatus *status)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonObject *oauth;
  const gchar *token;
  JsonNode *expires;

  *status = USAGE_ERR_NO_TOKEN;
  if (!json_parser_load_from_file (parser, credentials_path, NULL))
    return NULL;
  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    return NULL;
  oauth = get_object (json_node_get_object (root), "claudeAiOauth");
  token = get_string (oauth, "accessToken");
  if (token == NULL || *token == '\0')
    return NULL;
  /* expiresAt d'un type inattendu : on l'ignore, le 401 tranchera */
  expires = json_object_get_member (oauth, "expiresAt");
  if (expires != NULL && JSON_NODE_HOLDS_VALUE (expires)
      && json_node_get_value_type (expires) == G_TYPE_INT64
      && json_node_get_int (expires) <= now_ms)
    {
      *status = USAGE_ERR_EXPIRED;
      return NULL;
    }
  *status = USAGE_OK;
  return g_strdup (token);
}

typedef struct {
  UsageCallback cb;
  gpointer user_data;
  SoupMessage *msg;
} FetchCtx;

static void
on_fetch_done (GObject *source, GAsyncResult *res, gpointer data)
{
  FetchCtx *ctx = data;
  GError *err = NULL;
  GBytes *body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &err);

  if (body == NULL)
    {
      if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        ctx->cb (USAGE_ERR_HTTP, NULL, err->message, ctx->user_data);
      g_error_free (err);
    }
  else
    {
      guint status = soup_message_get_status (ctx->msg);

      if (status == SOUP_STATUS_UNAUTHORIZED)
        ctx->cb (USAGE_ERR_EXPIRED, NULL, _("Token rejected (401) — run claude"), ctx->user_data);
      else if (status != SOUP_STATUS_OK)
        {
          gchar *m = g_strdup_printf ("HTTP %u", status);
          ctx->cb (USAGE_ERR_HTTP, NULL, m, ctx->user_data);
          g_free (m);
        }
      else
        {
          ClaudeUsage u;
          gsize n;
          const gchar *d = g_bytes_get_data (body, &n);

          claude_usage_init (&u);
          if (usage_parse_json (d, n, &u, &err))
            ctx->cb (USAGE_OK, &u, NULL, ctx->user_data);
          else
            {
              ctx->cb (USAGE_ERR_PARSE, NULL, err->message, ctx->user_data);
              g_error_free (err);
            }
          claude_usage_clear (&u);
        }
      g_bytes_unref (body);
    }
  g_object_unref (ctx->msg);
  g_free (ctx);
}

void
usage_fetch_async (SoupSession *session, const gchar *token, GCancellable *cancellable,
                   UsageCallback cb, gpointer user_data)
{
  FetchCtx *ctx = g_new0 (FetchCtx, 1);
  SoupMessageHeaders *h;
  gchar *auth;

  ctx->cb = cb;
  ctx->user_data = user_data;
  ctx->msg = soup_message_new ("GET", USAGE_URL);
  h = soup_message_get_request_headers (ctx->msg);
  auth = g_strconcat ("Bearer ", token, NULL);
  soup_message_headers_replace (h, "Authorization", auth);
  g_free (auth);
  soup_message_headers_replace (h, "anthropic-beta", "oauth-2025-04-20");
  soup_message_headers_replace (h, "Accept", "application/json");
  soup_session_send_and_read_async (session, ctx->msg, G_PRIORITY_DEFAULT, cancellable,
                                    on_fetch_done, ctx);
}
