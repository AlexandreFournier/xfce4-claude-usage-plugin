#include <glib.h>
#include "pricing.h"

static void
test_lookup_exact (void)
{
  const ModelPrice *p = pricing_lookup ("claude-opus-5-5");
  g_assert_nonnull (p);
  g_assert_cmpfloat_with_epsilon (p->input, 4.0, 1e-9);
  g_assert_cmpfloat_with_epsilon (p->output, 20.0, 1e-9);
  g_assert_cmpfloat_with_epsilon (p->cache_read, 0.20, 1e-9);
}

static void
test_lookup_prefix_order (void)
{
  /* "claude-opus-5" ne doit pas capturer "claude-opus-5-5" et inversement */
  g_assert_cmpfloat_with_epsilon (pricing_lookup ("claude-opus-5")->input, 5.0, 1e-9);
  g_assert_cmpfloat_with_epsilon (pricing_lookup ("claude-sonnet-4-6")->input, 3.0, 1e-9);
  g_assert_cmpfloat_with_epsilon (pricing_lookup ("claude-sonnet-5-5")->input, 2.0, 1e-9);
  g_assert_cmpfloat_with_epsilon (pricing_lookup ("claude-haiku-5-5")->input, 0.10, 1e-9);
}

static void
test_lookup_unknown (void)
{
  g_assert_null (pricing_lookup ("gpt-4"));
  g_assert_null (pricing_lookup (NULL));
}

static void
test_cost (void)
{
  const ModelPrice *p = pricing_lookup ("claude-opus-5-5");
  /* 4 + 20 + 4*1.25 + 4*2 + 0.20 */
  g_assert_cmpfloat_with_epsilon (pricing_cost (p, 1000000, 1000000, 1000000, 1000000, 1000000),
                                  37.2, 1e-9);
  g_assert_cmpfloat_with_epsilon (pricing_cost (NULL, 1000000, 0, 0, 0, 0), 0.0, 1e-9);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/pricing/lookup-exact", test_lookup_exact);
  g_test_add_func ("/pricing/lookup-prefix-order", test_lookup_prefix_order);
  g_test_add_func ("/pricing/lookup-unknown", test_lookup_unknown);
  g_test_add_func ("/pricing/cost", test_cost);
  return g_test_run ();
}
