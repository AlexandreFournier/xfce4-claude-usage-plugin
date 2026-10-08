#pragma once
#include <glib.h>
#include <libsoup/soup.h>

/* Une limite de l'abonnement, telle que listée par l'API (limits[]) */
typedef struct {
  gchar *kind;       /* "session", "weekly_all", "weekly_scoped", ou inconnu */
  gchar *scope;      /* nom du modèle pour une limite ciblée, NULL sinon */
  gdouble pct;       /* 0..100, < 0 si inconnu */
  GDateTime *reset;  /* NULL si inconnu */
} UsageLimit;

/* Part d'un produit dans l'usage de la semaine (seven_day_breakdown.rows) */
typedef struct {
  gchar *name;
  gdouble pct;
} UsageShare;

typedef struct {
  gdouble five_hour_pct;      /* 0..100, < 0 si inconnu */
  GDateTime *five_hour_reset; /* NULL si inconnu */
  gdouble seven_day_pct;
  GDateTime *seven_day_reset;
  GPtrArray *limits;    /* UsageLimit*, non NULL après un parsing réussi */
  GPtrArray *breakdown; /* UsageShare*, non NULL après un parsing réussi */
} ClaudeUsage;

typedef enum {
  USAGE_OK,
  USAGE_ERR_NO_TOKEN,
  USAGE_ERR_EXPIRED,
  USAGE_ERR_HTTP,
  USAGE_ERR_PARSE,
} UsageStatus;

typedef void (*UsageCallback) (UsageStatus status, const ClaudeUsage *usage,
                               const gchar *message, gpointer user_data);

void claude_usage_init (ClaudeUsage *u);
void claude_usage_clear (ClaudeUsage *u); /* libère tout, tableaux compris */
void claude_usage_copy (const ClaudeUsage *src, ClaudeUsage *dst);

gboolean usage_parse_json (const gchar *data, gssize len, ClaudeUsage *out, GError **error);
/* "Max (5x)", "Pro"… ; NULL si type vide ou absent */
gchar *usage_plan_label (const gchar *subscription_type, const gchar *rate_limit_tier);
gchar *usage_read_plan (const gchar *credentials_path);
gchar *usage_read_token (const gchar *credentials_path, gint64 now_ms, UsageStatus *status);
void usage_fetch_async (SoupSession *session, const gchar *token, GCancellable *cancellable,
                        UsageCallback cb, gpointer user_data);
