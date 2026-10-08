#include "bars.h"
#include <pango/pangocairo.h>
#include <glib/gi18n-lib.h>

#define PAD 2.0
#define GAP 2.0
#define LABEL_W 16.0

/* Orange Claude #D97757 */
#define ORANGE_R (0xD9 / 255.0)
#define ORANGE_G (0x77 / 255.0)
#define ORANGE_B (0x57 / 255.0)

static void
show_text (cairo_t *cr, PangoLayout *layout, const gchar *text,
           gdouble x, gdouble y, gdouble row_h, gboolean align_right)
{
  int w, h;
  pango_layout_set_text (layout, text, -1);
  pango_layout_get_pixel_size (layout, &w, &h);
  cairo_move_to (cr, align_right ? x - w : x, y + (row_h - h) / 2.0);
  pango_cairo_show_layout (cr, layout);
}

gchar *
bars_format_remaining (GTimeSpan span)
{
  gint minutes;

  if (span <= 0)
    return NULL;
  minutes = (gint) MIN (span / G_TIME_SPAN_MINUTE, G_MAXINT);
  if (minutes < 60)
    /* TRANSLATORS: time left before reset, in minutes (e.g. "42m") */
    return g_strdup_printf (_("%dm"), minutes);
  if (minutes < 24 * 60)
    /* TRANSLATORS: time left before reset, hours and minutes (e.g. "2h13") */
    return g_strdup_printf (_("%dh%02d"), minutes / 60, minutes % 60);
  /* TRANSLATORS: time left before reset, days and hours (e.g. "3d 4h") */
  return g_strdup_printf (_("%dd %dh"), minutes / (24 * 60), (minutes / 60) % 24);
}

gint
bars_auto_bar_height (gint height, guint n)
{
  gdouble h;

  if (n == 0)
    return 0;
  h = (height - 2 * PAD - GAP * (n - 1)) / n;
  return h > 0 ? (gint) h : 0;
}

void
bars_draw (cairo_t *cr, gint width, gint height, const Bar *bars, guint n,
           gboolean stale, const GdkRGBA *fg, const BarsStyle *style)
{
  static const BarsStyle default_style = { 0, TRUE, TRUE };
  gdouble row_h, x0, y0, track_w;
  PangoLayout *layout;
  PangoFontDescription *fd;

  if (style == NULL)
    style = &default_style;
  if (n == 0 || width <= 0 || height <= 0)
    return;
  row_h = bars_auto_bar_height (height, n);
  if (style->bar_height > 0)
    row_h = MIN (row_h, style->bar_height);
  /* bloc de barres centré verticalement */
  y0 = (height - (row_h * n + GAP * (n - 1))) / 2.0;
  x0 = style->show_labels ? PAD + LABEL_W : PAD;
  track_w = width - x0 - PAD;
  if (row_h <= 0 || track_w <= 0)
    return;

  layout = pango_cairo_create_layout (cr);
  fd = pango_font_description_new ();
  pango_font_description_set_family (fd, "Sans");
  pango_font_description_set_weight (fd, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size (fd, CLAMP (row_h * 0.75, 6.0, 10.0) * PANGO_SCALE);
  pango_layout_set_font_description (layout, fd);

  for (guint i = 0; i < n; i++)
    {
      gdouble y = y0 + i * (row_h + GAP);
      gchar pct[8];

      if (style->show_labels)
        {
          cairo_set_source_rgba (cr, fg->red, fg->green, fg->blue, fg->alpha);
          show_text (cr, layout, bars[i].label, PAD, y, row_h, FALSE);
        }

      cairo_set_source_rgba (cr, fg->red, fg->green, fg->blue, 0.15);
      cairo_rectangle (cr, x0, y, track_w, row_h);
      cairo_fill (cr);

      if (bars[i].fraction >= 0)
        {
          gdouble f = CLAMP (bars[i].fraction, 0.0, 1.0);
          if (stale)
            cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
          else
            cairo_set_source_rgb (cr, ORANGE_R, ORANGE_G, ORANGE_B);
          cairo_rectangle (cr, x0, y, track_w * f, row_h);
          cairo_fill (cr);
          g_snprintf (pct, sizeof pct, "%d%%", (gint) (f * 100 + 0.5));
        }
      else
        g_strlcpy (pct, "?", sizeof pct);

      if (bars[i].remaining != NULL)
        {
          cairo_set_source_rgba (cr, fg->red, fg->green, fg->blue, fg->alpha);
          show_text (cr, layout, bars[i].remaining, x0 + 2, y, row_h, FALSE);
        }

      if (style->show_percent)
        {
          cairo_set_source_rgba (cr, fg->red, fg->green, fg->blue, fg->alpha);
          show_text (cr, layout, pct, x0 + track_w - 2, y, row_h, TRUE);
        }
    }

  pango_font_description_free (fd);
  g_object_unref (layout);
}
