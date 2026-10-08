#pragma once
#include <gtk/gtk.h>

typedef struct {
  const gchar *label;
  gdouble fraction;      /* 0..1, < 0 si inconnu */
  const gchar *remaining; /* temps restant avant reset, NULL = rien */
} Bar;

typedef struct {
  gint bar_height;        /* px par barre, 0 = automatique */
  gboolean show_percent;
  gboolean show_labels;
} BarsStyle;

/* "42m", "2h13", "3j 4h" ; NULL si span <= 0 */
gchar *bars_format_remaining (GTimeSpan span);

/* Hauteur d'une barre en mode automatique (px entiers, 0 si impossible) */
gint bars_auto_bar_height (gint height, guint n);

/* style NULL = automatique, pourcentages et labels affichés */
void bars_draw (cairo_t *cr, gint width, gint height, const Bar *bars, guint n,
                gboolean stale, const GdkRGBA *fg, const BarsStyle *style);
