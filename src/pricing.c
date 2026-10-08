#include "pricing.h"

#define P(in, out, cr) { (in), (out), (in) * 1.25, (in) * 2.0, (cr) }

/* Ordre important : préfixes les plus longs d'abord */
static const struct {
  const gchar *prefix;
  ModelPrice price;
} table[] = {
  { "claude-fable-5-1",  P (10.0, 50.0, 0.25) },
  { "claude-mythos",     P (10.0, 50.0, 0.25) },
  { "claude-fable",      P (10.0, 50.0, 1.00) },
  { "claude-opus-5-5",   P (4.0,  20.0, 0.20) },
  { "claude-opus-5",     P (5.0,  25.0, 0.50) },
  { "claude-opus-4-8",   P (5.0,  25.0, 0.50) },
  { "claude-opus-4-7",   P (5.0,  25.0, 0.50) },
  { "claude-opus-4-6",   P (5.0,  25.0, 0.50) },
  { "claude-opus-4-5",   P (5.0,  25.0, 0.50) },
  { "claude-opus-4",     P (15.0, 75.0, 1.50) },
  { "claude-sonnet-5",   P (2.0,  10.0, 0.20) },
  { "claude-sonnet-4",   P (3.0,  15.0, 0.30) },
  { "claude-haiku-5-5",  P (0.10, 0.50, 0.01) },
  { "claude-haiku-4-5",  P (1.0,  5.0,  0.10) },
};

const ModelPrice *
pricing_lookup (const gchar *model)
{
  if (model == NULL)
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (table); i++)
    if (g_str_has_prefix (model, table[i].prefix))
      return &table[i].price;
  return NULL;
}

gdouble
pricing_cost (const ModelPrice *p, guint64 input, guint64 output,
              guint64 cache_write_5m, guint64 cache_write_1h, guint64 cache_read)
{
  if (p == NULL)
    return 0.0;
  return (input * p->input
          + output * p->output
          + cache_write_5m * p->cache_write_5m
          + cache_write_1h * p->cache_write_1h
          + cache_read * p->cache_read) / 1e6;
}
