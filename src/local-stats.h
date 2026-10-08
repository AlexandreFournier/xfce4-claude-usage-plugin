#pragma once
#include <glib.h>

typedef struct {
  guint64 input;
  guint64 output;
  guint64 cache_write;
  guint64 cache_read;
  gdouble cost_usd;
} TokenTotals;

typedef struct {
  TokenTotals today;
  TokenTotals week;   /* 7 jours glissants */
  TokenTotals month;  /* 30 jours glissants */
} LocalStats;

typedef struct _LocalStatsCache LocalStatsCache;

LocalStatsCache *local_stats_cache_new (const gchar *projects_dir);
void local_stats_cache_free (LocalStatsCache *cache);
guint local_stats_cache_record_count (LocalStatsCache *cache);
void local_stats_compute (LocalStatsCache *cache, GDateTime *now, LocalStats *out);

guint64 token_totals_sum (const TokenTotals *t);
gchar *local_stats_format_tokens (guint64 n);
