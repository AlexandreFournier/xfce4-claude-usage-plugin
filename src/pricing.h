#pragma once
#include <glib.h>

/* Prix en USD par million de tokens */
typedef struct {
  gdouble input;
  gdouble output;
  gdouble cache_write_5m;
  gdouble cache_write_1h;
  gdouble cache_read;
} ModelPrice;

const ModelPrice *pricing_lookup (const gchar *model);
gdouble pricing_cost (const ModelPrice *p, guint64 input, guint64 output,
                      guint64 cache_write_5m, guint64 cache_write_1h, guint64 cache_read);
