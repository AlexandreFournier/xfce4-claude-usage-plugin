#include <gtk/gtk.h>
#include "bars.h"

static guint32
pixel (cairo_surface_t *s, int x, int y)
{
  unsigned char *data = cairo_image_surface_get_data (s);
  int stride = cairo_image_surface_get_stride (s);
  return *(guint32 *) (data + y * stride + x * 4);
}

static cairo_surface_t *
render_style (gdouble f1, gdouble f2, gboolean stale, const BarsStyle *style)
{
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 100, 30);
  cairo_t *cr = cairo_create (s);
  GdkRGBA fg = { 1, 1, 1, 1 };
  Bar bars[2] = { { "5h", f1, NULL }, { "7j", f2, NULL } };

  bars_draw (cr, 100, 30, bars, 2, stale, &fg, style);
  cairo_destroy (cr);
  cairo_surface_flush (s);
  return s;
}

static cairo_surface_t *
render (gdouble f1, gdouble f2, gboolean stale)
{
  return render_style (f1, f2, stale, NULL);
}

/* Ligne 2 : y de 16 à 28 ; la piste commence à x = 18 */
static void
test_fill_orange (void)
{
  cairo_surface_t *s = render (0.5, 1.0, FALSE);
  g_assert_cmphex (pixel (s, 20, 22), ==, 0xFFD97757);
  cairo_surface_destroy (s);
}

static void
test_stale_grey (void)
{
  cairo_surface_t *s = render (0.5, 1.0, TRUE);
  g_assert_cmphex (pixel (s, 20, 22), ==, 0xFF999999);
  cairo_surface_destroy (s);
}

static void
test_partial_and_unknown (void)
{
  cairo_surface_t *s = render (0.1, -1, FALSE);
  /* ligne 1 (y 2..14) : 10 % de remplissage (x 18–26) → x = 50 n'est pas orange */
  g_assert_cmphex (pixel (s, 20, 8), ==, 0xFFD97757);
  g_assert_cmphex (pixel (s, 50, 8), !=, 0xFFD97757);
  /* ligne 2 inconnue : aucun orange */
  g_assert_cmphex (pixel (s, 20, 22), !=, 0xFFD97757);
  cairo_surface_destroy (s);
}

/* Sans labels, la piste commence à x = PAD = 2 */
static void
test_no_labels (void)
{
  BarsStyle style = { 0, TRUE, FALSE };
  cairo_surface_t *s = render_style (1.0, 1.0, FALSE, &style);
  g_assert_cmphex (pixel (s, 3, 8), ==, 0xFFD97757);
  cairo_surface_destroy (s);
}

/* Hauteur fixe 4 px : bloc de 4 + 2 + 4 = 10 px centré → lignes y 10..14 et 16..20 */
static void
test_fixed_height (void)
{
  BarsStyle style = { 4, TRUE, TRUE };
  cairo_surface_t *s = render_style (1.0, 1.0, FALSE, &style);
  g_assert_cmphex (pixel (s, 20, 12), ==, 0xFFD97757);
  g_assert_cmphex (pixel (s, 20, 18), ==, 0xFFD97757);
  g_assert_cmphex (pixel (s, 20, 5), ==, 0);
  g_assert_cmphex (pixel (s, 20, 25), ==, 0);
  cairo_surface_destroy (s);
}

/* Sans pourcentage, la fin d'une barre pleine reste entièrement orange */
static void
test_no_percent (void)
{
  BarsStyle style = { 0, FALSE, TRUE };
  cairo_surface_t *s = render_style (1.0, 1.0, FALSE, &style);
  for (int x = 70; x < 98; x++)
    for (int y = 3; y < 14; y++)
      g_assert_cmphex (pixel (s, x, y), ==, 0xFFD97757);
  cairo_surface_destroy (s);
}

static void
check_remaining (GTimeSpan span, const gchar *expected)
{
  gchar *s = bars_format_remaining (span);
  g_assert_cmpstr (s, ==, expected);
  g_free (s);
}

static void
test_format_remaining (void)
{
  check_remaining (59 * G_TIME_SPAN_MINUTE, "59m");
  check_remaining (60 * G_TIME_SPAN_MINUTE, "1h00");
  check_remaining (2 * G_TIME_SPAN_HOUR + 13 * G_TIME_SPAN_MINUTE + 30 * G_TIME_SPAN_SECOND, "2h13");
  check_remaining (3 * G_TIME_SPAN_DAY + 4 * G_TIME_SPAN_HOUR + 5 * G_TIME_SPAN_MINUTE, "3d 4h");
  check_remaining (30 * G_TIME_SPAN_SECOND, "0m");
  check_remaining (0, NULL);
  check_remaining (-G_TIME_SPAN_MINUTE, NULL);
}

/* Le temps restant s'écrit au début de la piste (x 20..44, ligne 1) */
static gboolean
has_text_at_track_start (const gchar *remaining)
{
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 100, 30);
  cairo_t *cr = cairo_create (s);
  GdkRGBA fg = { 1, 1, 1, 1 };
  Bar bars[2] = { { "5h", 0.0, remaining }, { "7j", 0.0, NULL } };
  gboolean found = FALSE;

  bars_draw (cr, 100, 30, bars, 2, FALSE, &fg, NULL);
  cairo_destroy (cr);
  cairo_surface_flush (s);
  for (int x = 20; x < 44 && !found; x++)
    for (int y = 2; y < 14 && !found; y++)
      found = (pixel (s, x, y) >> 24) > 0x80;
  cairo_surface_destroy (s);
  return found;
}

static void
test_remaining_drawn (void)
{
  g_assert_true (has_text_at_track_start ("2h13"));
  g_assert_false (has_text_at_track_start (NULL));
}

/* Hauteur auto = (hauteur - 2*PAD - GAP*(n-1)) / n */
static void
test_auto_bar_height (void)
{
  g_assert_cmpint (bars_auto_bar_height (30, 2), ==, 12);
  g_assert_cmpint (bars_auto_bar_height (40, 2), ==, 17);
  g_assert_cmpint (bars_auto_bar_height (3, 2), ==, 0);
  g_assert_cmpint (bars_auto_bar_height (30, 0), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/bars/fill-orange", test_fill_orange);
  g_test_add_func ("/bars/stale-grey", test_stale_grey);
  g_test_add_func ("/bars/partial-and-unknown", test_partial_and_unknown);
  g_test_add_func ("/bars/no-labels", test_no_labels);
  g_test_add_func ("/bars/fixed-height", test_fixed_height);
  g_test_add_func ("/bars/no-percent", test_no_percent);
  g_test_add_func ("/bars/format-remaining", test_format_remaining);
  g_test_add_func ("/bars/remaining-drawn", test_remaining_drawn);
  g_test_add_func ("/bars/auto-bar-height", test_auto_bar_height);
  return g_test_run ();
}
