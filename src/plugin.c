#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>
#include "bars.h"
#include "launcher.h"
#include "local-stats.h"
#include "usage-api.h"

#define DEFAULT_INTERVAL 300
#define MIN_INTERVAL 60
#define MAX_INTERVAL 3600
#define MAX_BAR_WIDTH 400
#define MIN_BAR_WIDTH 20
#define MAX_BAR_HEIGHT 40
#define MIN_BAR_HEIGHT 2

typedef struct {
  XfcePanelPlugin *plugin;
  GtkWidget *area;
  SoupSession *session;
  GtkWidget *dialog;
  LocalStatsCache *stats_cache;

  ClaudeUsage usage;
  gboolean have_usage;
  UsageStatus status;
  gchar *error_msg;

  LocalStats stats;
  gboolean have_stats;
  gboolean stats_busy;
  gboolean fetch_busy;
  gboolean dead;

  gint interval;
  guint timer_id;

  gint bar_width;  /* 0 = automatique */
  BarsStyle style;
  gboolean show_remaining;
  guint live_timer_id;

  ClickAction click_action;
  gchar *click_command;
  gchar *click_error; /* dernier échec du clic, affiché dans l'infobulle */

  gchar *plan;            /* "Max (5x)"… ou NULL */
  GDateTime *updated_at;  /* dernière réponse réussie de l'API */
} ClaudePlugin;

static void refresh (ClaudePlugin *cp);

static void
claude_plugin_destroy (ClaudePlugin *cp)
{
  g_object_unref (cp->session);
  local_stats_cache_free (cp->stats_cache);
  claude_usage_clear (&cp->usage);
  g_free (cp->error_msg);
  g_free (cp->click_command);
  g_free (cp->click_error);
  g_free (cp->plan);
  if (cp->updated_at != NULL)
    g_date_time_unref (cp->updated_at);
  g_free (cp);
}

/* --- Infobulle --------------------------------------------------------- */

static gchar *
format_reset (GDateTime *reset)
{
  GDateTime *local, *now;
  gboolean same_day;
  gchar *s;

  if (reset == NULL)
    return g_strdup ("—");
  {
    /* l'API renvoie hh:59:59.8 : on arrondit à la minute la plus proche */
    GDateTime *rounded = g_date_time_add_seconds (reset, 30);
    local = g_date_time_to_local (rounded);
    g_date_time_unref (rounded);
  }
  now = g_date_time_new_now_local ();
  same_day = g_date_time_get_year (local) == g_date_time_get_year (now)
             && g_date_time_get_day_of_year (local) == g_date_time_get_day_of_year (now);
  s = g_date_time_format (local, same_day ? "%H:%M" : "%a %H:%M");
  g_date_time_unref (local);
  g_date_time_unref (now);
  return s;
}

static const gchar *
status_message (ClaudePlugin *cp)
{
  if (cp->error_msg != NULL)
    return cp->error_msg;
  switch (cp->status)
    {
    case USAGE_ERR_NO_TOKEN: return _("No token — log in with claude");
    case USAGE_ERR_EXPIRED:  return _("Token expired — run claude");
    case USAGE_ERR_HTTP:     return _("Network error");
    case USAGE_ERR_PARSE:    return _("Unreadable response");
    default:                 return NULL;
    }
}

/* --- Infobulle -------------------------------------------------------- */

static gboolean
on_mini_bar_draw (GtkWidget *widget, cairo_t *cr, gpointer data)
{
  static const BarsStyle style = { 0, FALSE, FALSE };
  GtkStyleContext *sc = gtk_widget_get_style_context (widget);
  GdkRGBA fg;
  Bar bar = { "", *(gdouble *) g_object_get_data (G_OBJECT (widget), "fraction"), NULL };

  gtk_style_context_get_color (sc, gtk_style_context_get_state (sc), &fg);
  bars_draw (cr, gtk_widget_get_allocated_width (widget), gtk_widget_get_allocated_height (widget),
             &bar, 1, GPOINTER_TO_INT (data), &fg, &style);
  return FALSE;
}

static GtkWidget *
mini_bar_new (gdouble pct, gboolean stale)
{
  GtkWidget *area = gtk_drawing_area_new ();
  gdouble *fraction = g_new (gdouble, 1);

  *fraction = pct >= 0 ? pct / 100.0 : -1;
  g_object_set_data_full (G_OBJECT (area), "fraction", fraction, g_free);
  gtk_widget_set_size_request (area, 140, 10);
  gtk_widget_set_valign (area, GTK_ALIGN_CENTER);
  g_signal_connect (area, "draw", G_CALLBACK (on_mini_bar_draw), GINT_TO_POINTER (stale));
  return area;
}

static GtkWidget *
label_new (const gchar *markup, gfloat xalign)
{
  GtkWidget *l = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (l), markup);
  gtk_label_set_xalign (GTK_LABEL (l), xalign);
  return l;
}

/* Texte secondaire, atténué */
static GtkWidget *
dim_label_new (const gchar *text)
{
  gchar *m = g_markup_printf_escaped ("<small><span alpha=\"65%%\">%s</span></small>", text);
  GtkWidget *l = label_new (m, 0.0);
  g_free (m);
  return l;
}

static GtkWidget *
heading_new (const gchar *text)
{
  gchar *m = g_markup_printf_escaped ("<b>%s</b>", text);
  GtkWidget *l = label_new (m, 0.0);
  g_free (m);
  return l;
}

static gchar *
pct_text (gdouble pct)
{
  return pct >= 0 ? g_strdup_printf (_("%.0f %%"), pct) : g_strdup ("—");
}

static gchar *
limit_title (const UsageLimit *l)
{
  gchar *t;

  if (g_strcmp0 (l->kind, "session") == 0)
    return g_strdup (_("Current session"));
  if (g_strcmp0 (l->kind, "weekly_all") == 0)
    return g_strdup (_("This week"));
  if (g_strcmp0 (l->kind, "weekly_scoped") == 0)
    return l->scope != NULL ? g_strdup_printf (_("%s this week"), l->scope)
                            : g_strdup (_("Model limit this week"));
  /* type inconnu : on l'affiche quand même, lisiblement */
  t = g_strdelimit (g_strdup (l->kind), "_", ' ');
  t[0] = g_ascii_toupper (t[0]);
  return l->scope != NULL ? g_strdup_printf ("%s (%s)", t, l->scope) : t;
}

/* Ligne « titre | barre | pourcentage » ; renvoie la ligne suivante */
static gint
attach_bar_row (GtkGrid *grid, gint row, const gchar *title, gdouble pct, gboolean stale)
{
  gchar *p = pct_text (pct);
  gchar *m = g_markup_escape_text (title, -1);

  gtk_grid_attach (grid, label_new (m, 0.0), 0, row, 1, 1);
  gtk_grid_attach (grid, mini_bar_new (pct, stale), 1, row, 1, 1);
  gtk_grid_attach (grid, label_new (p, 1.0), 2, row, 1, 1);
  g_free (p);
  g_free (m);
  return row + 1;
}

static gint
attach_separator (GtkGrid *grid, gint row)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top (sep, 4);
  gtk_widget_set_margin_bottom (sep, 4);
  gtk_grid_attach (grid, sep, 0, row, 3, 1);
  return row + 1;
}

static gint
attach_warning (GtkGrid *grid, gint row, const gchar *text)
{
  gchar *m = g_markup_printf_escaped ("⚠ %s", text);
  GtkWidget *l = label_new (m, 0.0);
  gtk_label_set_line_wrap (GTK_LABEL (l), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (l), 50);
  gtk_grid_attach (grid, l, 0, row, 3, 1);
  g_free (m);
  return row + 1;
}

static gint
attach_limits (GtkGrid *grid, gint row, ClaudePlugin *cp)
{
  gboolean stale = cp->status != USAGE_OK;

  for (guint i = 0; i < cp->usage.limits->len; i++)
    {
      const UsageLimit *l = g_ptr_array_index (cp->usage.limits, i);
      gchar *title = limit_title (l);

      row = attach_bar_row (grid, row, title, l->pct, stale);
      if (l->reset != NULL)
        {
          gchar *r = format_reset (l->reset);
          gchar *t = g_strdup_printf (_("Resets %s"), r);
          GtkWidget *dim = dim_label_new (t);
          gtk_widget_set_margin_bottom (dim, 4);
          gtk_grid_attach (grid, dim, 0, row++, 3, 1);
          g_free (t);
          g_free (r);
        }
      g_free (title);
    }
  return row;
}

static gint
attach_breakdown (GtkGrid *grid, gint row, ClaudePlugin *cp)
{
  row = attach_separator (grid, row);
  gtk_grid_attach (grid, heading_new (_("This week's usage by product")), 0, row++, 3, 1);
  for (guint i = 0; i < cp->usage.breakdown->len; i++)
    {
      const UsageShare *sh = g_ptr_array_index (cp->usage.breakdown, i);
      row = attach_bar_row (grid, row, sh->name, sh->pct, cp->status != USAGE_OK);
    }
  return row;
}

static gint
attach_local_stats (GtkGrid *grid, gint row, ClaudePlugin *cp)
{
  const gchar *labels[] = { _("Today"), _("7 days"), _("30 days") };
  const TokenTotals *totals[] = { &cp->stats.today, &cp->stats.week, &cp->stats.month };

  row = attach_separator (grid, row);
  gtk_grid_attach (grid, heading_new (_("Local usage")), 0, row, 2, 1);
  gtk_grid_attach (grid, dim_label_new (_("API price estimate")), 2, row++, 1, 1);
  if (!cp->have_stats)
    {
      gtk_grid_attach (grid, dim_label_new (_("Computing local statistics…")), 0, row++, 3, 1);
      return row;
    }
  for (guint i = 0; i < G_N_ELEMENTS (labels); i++)
    {
      gchar *tok = local_stats_format_tokens (token_totals_sum (totals[i]));
      gchar *tok_text = g_strdup_printf (_("%s tokens"), tok);
      gchar *cost = g_strdup_printf ("~%.2f $", totals[i]->cost_usd);
      gchar *m = g_markup_escape_text (labels[i], -1);

      gtk_grid_attach (grid, label_new (m, 0.0), 0, row, 1, 1);
      gtk_grid_attach (grid, label_new (tok_text, 1.0), 1, row, 1, 1);
      gtk_grid_attach (grid, label_new (cost, 1.0), 2, row++, 1, 1);
      g_free (m);
      g_free (cost);
      g_free (tok_text);
      g_free (tok);
    }
  return row;
}

static gint
attach_updated (GtkGrid *grid, gint row, ClaudePlugin *cp)
{
  GDateTime *now;
  gint64 minutes;
  gchar *t;

  if (cp->updated_at == NULL)
    return row;
  now = g_date_time_new_now_utc ();
  minutes = g_date_time_difference (now, cp->updated_at) / G_TIME_SPAN_MINUTE;
  g_date_time_unref (now);
  t = minutes < 1 ? g_strdup (_("Updated less than a minute ago"))
                  : g_strdup_printf (g_dngettext (GETTEXT_PACKAGE, "Updated %d minute ago",
                                                  "Updated %d minutes ago", minutes),
                                     (gint) minutes);
  row = attach_separator (grid, row);
  gtk_grid_attach (grid, dim_label_new (t), 0, row++, 3, 1);
  g_free (t);
  return row;
}

static GtkWidget *
build_tooltip (ClaudePlugin *cp)
{
  GtkWidget *grid = gtk_grid_new ();
  GtkGrid *g = GTK_GRID (grid);
  const gchar *err = status_message (cp);
  gint row = 0;

  gtk_grid_set_row_spacing (g, 2);
  gtk_grid_set_column_spacing (g, 12);
  gtk_container_set_border_width (GTK_CONTAINER (grid), 4);

  gtk_grid_attach (g, heading_new (_("Your usage")), 0, row, 2, 1);
  if (cp->plan != NULL)
    {
      gchar *m = g_markup_printf_escaped ("<span alpha=\"65%%\">%s</span>", cp->plan);
      gtk_grid_attach (g, label_new (m, 1.0), 2, row, 1, 1);
      g_free (m);
    }
  row = attach_separator (g, row + 1);

  if (err != NULL)
    row = attach_warning (g, row, err);
  if (cp->click_error != NULL)
    row = attach_warning (g, row, cp->click_error);

  if (cp->have_usage && cp->usage.limits != NULL && cp->usage.limits->len > 0)
    row = attach_limits (g, row, cp);
  else if (err == NULL)
    gtk_grid_attach (g, dim_label_new (_("Loading…")), 0, row++, 3, 1);

  if (cp->have_usage && cp->usage.breakdown != NULL && cp->usage.breakdown->len > 0)
    row = attach_breakdown (g, row, cp);

  row = attach_local_stats (g, row, cp);
  attach_updated (g, row, cp);

  gtk_widget_show_all (grid);
  return grid;
}

static gboolean
on_query_tooltip (GtkWidget *widget, gint x, gint y, gboolean keyboard, GtkTooltip *tooltip,
                  ClaudePlugin *cp)
{
  gtk_tooltip_set_custom (tooltip, build_tooltip (cp));
  return TRUE;
}

/* L'infobulle est reconstruite à chaque affichage ; on force juste sa mise à jour */
static void
update_tooltip (ClaudePlugin *cp)
{
  gtk_widget_trigger_tooltip_query (cp->area);
}

/* --- Données ----------------------------------------------------------- */

static void
on_usage (UsageStatus status, const ClaudeUsage *usage, const gchar *message, gpointer data)
{
  ClaudePlugin *cp = data;

  cp->status = status;
  g_free (cp->error_msg);
  cp->error_msg = g_strdup (message);
  if (status == USAGE_OK && usage != NULL)
    {
      claude_usage_copy (usage, &cp->usage);
      cp->have_usage = TRUE;
      if (cp->updated_at != NULL)
        g_date_time_unref (cp->updated_at);
      cp->updated_at = g_date_time_new_now_utc ();
    }
  gtk_widget_queue_draw (cp->area);
  update_tooltip (cp);
}

static void
on_fetch_cb (UsageStatus status, const ClaudeUsage *usage, const gchar *message, gpointer data)
{
  ClaudePlugin *cp = data;

  cp->fetch_busy = FALSE;
  if (cp->dead)
    {
      if (!cp->stats_busy)
        claude_plugin_destroy (cp);
      return;
    }
  on_usage (status, usage, message, cp);
}

static void
stats_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  LocalStats *out = g_new0 (LocalStats, 1);
  GDateTime *now = g_date_time_new_now_local ();

  local_stats_compute (data, now, out);
  g_date_time_unref (now);
  g_task_return_pointer (task, out, g_free);
}

static void
on_stats_done (GObject *source, GAsyncResult *res, gpointer data)
{
  ClaudePlugin *cp = data;
  LocalStats *s = g_task_propagate_pointer (G_TASK (res), NULL);

  cp->stats_busy = FALSE;
  if (cp->dead)
    {
      g_free (s);
      if (!cp->fetch_busy)
        claude_plugin_destroy (cp);
      return;
    }
  if (s != NULL)
    {
      cp->stats = *s;
      cp->have_stats = TRUE;
      g_free (s);
    }
  update_tooltip (cp);
}

static void
refresh (ClaudePlugin *cp)
{
  gchar *cred = g_build_filename (g_get_home_dir (), ".claude", ".credentials.json", NULL);
  UsageStatus st;
  gchar *token = usage_read_token (cred, g_get_real_time () / 1000, &st);

  g_free (cp->plan);
  cp->plan = usage_read_plan (cred);
  g_free (cred);
  if (token != NULL)
    {
      if (!cp->fetch_busy)
        {
          cp->fetch_busy = TRUE;
          usage_fetch_async (cp->session, token, NULL, on_fetch_cb, cp);
        }
      g_free (token);
    }
  else
    on_usage (st, NULL, NULL, cp);

  if (!cp->stats_busy)
    {
      GTask *task = g_task_new (NULL, NULL, on_stats_done, cp);
      cp->stats_busy = TRUE;
      g_task_set_task_data (task, cp->stats_cache, NULL);
      g_task_run_in_thread (task, stats_thread);
      g_object_unref (task);
    }
}

static gboolean
on_timer (gpointer data)
{
  refresh (data);
  return G_SOURCE_CONTINUE;
}

static void
restart_timer (ClaudePlugin *cp)
{
  if (cp->timer_id != 0)
    g_source_remove (cp->timer_id);
  cp->timer_id = g_timeout_add_seconds (cp->interval, on_timer, cp);
}

/* Redessine le temps restant en direct, sans appel réseau */
static gboolean
on_live_timer (gpointer data)
{
  ClaudePlugin *cp = data;
  gtk_widget_queue_draw (cp->area);
  return G_SOURCE_CONTINUE;
}

static void
update_live_timer (ClaudePlugin *cp)
{
  if (cp->show_remaining && cp->live_timer_id == 0)
    cp->live_timer_id = g_timeout_add_seconds (30, on_live_timer, cp);
  else if (!cp->show_remaining && cp->live_timer_id != 0)
    {
      g_source_remove (cp->live_timer_id);
      cp->live_timer_id = 0;
    }
}

/* --- Widget ------------------------------------------------------------ */

static gboolean
on_draw (GtkWidget *widget, cairo_t *cr, ClaudePlugin *cp)
{
  GtkStyleContext *sc = gtk_widget_get_style_context (widget);
  GdkRGBA fg;
  gchar *five_left = NULL, *seven_left = NULL;
  Bar bars[2];

  if (cp->show_remaining && cp->have_usage)
    {
      GDateTime *now = g_date_time_new_now_utc ();
      if (cp->usage.five_hour_reset != NULL)
        five_left = bars_format_remaining (g_date_time_difference (cp->usage.five_hour_reset, now));
      if (cp->usage.seven_day_reset != NULL)
        seven_left = bars_format_remaining (g_date_time_difference (cp->usage.seven_day_reset, now));
      g_date_time_unref (now);
    }
  /* TRANSLATORS: short bar label for the 5-hour window */
  bars[0] = (Bar) { _("5h"), cp->have_usage && cp->usage.five_hour_pct >= 0
                            ? cp->usage.five_hour_pct / 100.0 : -1, five_left };
  /* TRANSLATORS: short bar label for the 7-day window */
  bars[1] = (Bar) { _("7d"), cp->have_usage && cp->usage.seven_day_pct >= 0
                            ? cp->usage.seven_day_pct / 100.0 : -1, seven_left };

  gtk_style_context_get_color (sc, gtk_style_context_get_state (sc), &fg);
  bars_draw (cr, gtk_widget_get_allocated_width (widget), gtk_widget_get_allocated_height (widget),
             bars, 2, cp->status != USAGE_OK, &fg, &cp->style);
  g_free (five_left);
  g_free (seven_left);
  return FALSE;
}

static gboolean
on_button_press (GtkWidget *widget, GdkEventButton *event, ClaudePlugin *cp)
{
  if (event->type != GDK_BUTTON_PRESS)
    return FALSE;
  if (event->button == 1)
    {
      GError *err = NULL;

      g_clear_pointer (&cp->click_error, g_free);
      if (!launcher_run (cp->click_action, cp->click_command, &err))
        {
          cp->click_error = g_strdup (err->message);
          g_error_free (err);
        }
      update_tooltip (cp);
      return TRUE;
    }
  if (event->button == 2)
    {
      refresh (cp);
      return TRUE;
    }
  return FALSE;
}

static gboolean
on_size_changed (XfcePanelPlugin *plugin, gint size, ClaudePlugin *cp)
{
  size /= xfce_panel_plugin_get_nrows (plugin);
  if (xfce_panel_plugin_get_orientation (plugin) == GTK_ORIENTATION_HORIZONTAL)
    gtk_widget_set_size_request (GTK_WIDGET (plugin),
                                 cp->bar_width > 0 ? cp->bar_width : MAX (64, size * 2), size);
  else
    gtk_widget_set_size_request (GTK_WIDGET (plugin), size, MAX (24, size * 2 / 3));
  return TRUE;
}

/* --- Configuration ----------------------------------------------------- */

static void
load_config (ClaudePlugin *cp)
{
  gchar *file = xfce_panel_plugin_lookup_rc_file (cp->plugin);
  XfceRc *rc;

  cp->interval = DEFAULT_INTERVAL;
  cp->bar_width = 0;
  cp->style.bar_height = 0;
  cp->style.show_percent = TRUE;
  cp->style.show_labels = TRUE;
  cp->show_remaining = FALSE;
  cp->click_action = CLICK_OPEN_WEB;
  cp->click_command = g_strdup ("");
  if (file == NULL)
    return;
  rc = xfce_rc_simple_open (file, TRUE);
  g_free (file);
  if (rc == NULL)
    return;
  cp->interval = CLAMP (xfce_rc_read_int_entry (rc, "interval", DEFAULT_INTERVAL),
                        MIN_INTERVAL, MAX_INTERVAL);
  cp->bar_width = xfce_rc_read_int_entry (rc, "bar_width", 0);
  if (cp->bar_width != 0)
    cp->bar_width = CLAMP (cp->bar_width, MIN_BAR_WIDTH, MAX_BAR_WIDTH);
  cp->style.bar_height = xfce_rc_read_int_entry (rc, "bar_height", 0);
  if (cp->style.bar_height != 0)
    cp->style.bar_height = CLAMP (cp->style.bar_height, MIN_BAR_HEIGHT, MAX_BAR_HEIGHT);
  cp->style.show_percent = xfce_rc_read_bool_entry (rc, "show_percent", TRUE);
  cp->style.show_labels = xfce_rc_read_bool_entry (rc, "show_labels", TRUE);
  cp->show_remaining = xfce_rc_read_bool_entry (rc, "show_remaining", FALSE);
  cp->click_action = click_action_from_string (xfce_rc_read_entry (rc, "click_action", "web"));
  g_free (cp->click_command);
  cp->click_command = g_strdup (xfce_rc_read_entry (rc, "click_command", ""));
  xfce_rc_close (rc);
}

static void
on_save (XfcePanelPlugin *plugin, ClaudePlugin *cp)
{
  gchar *file = xfce_panel_plugin_save_location (plugin, TRUE);
  XfceRc *rc;

  if (file == NULL)
    return;
  rc = xfce_rc_simple_open (file, FALSE);
  g_free (file);
  if (rc == NULL)
    return;
  xfce_rc_write_int_entry (rc, "interval", cp->interval);
  xfce_rc_write_int_entry (rc, "bar_width", cp->bar_width);
  xfce_rc_write_int_entry (rc, "bar_height", cp->style.bar_height);
  xfce_rc_write_bool_entry (rc, "show_percent", cp->style.show_percent);
  xfce_rc_write_bool_entry (rc, "show_labels", cp->style.show_labels);
  xfce_rc_write_bool_entry (rc, "show_remaining", cp->show_remaining);
  xfce_rc_write_entry (rc, "click_action", click_action_to_string (cp->click_action));
  xfce_rc_write_entry (rc, "click_command", cp->click_command);
  xfce_rc_close (rc);
}

/* Un champ en mode auto affiche la taille réelle mais reste auto tant que
   l'utilisateur ne l'a pas modifié ; 0 = auto ; sinon au moins min */
static gint
spin_value_or_auto (GtkWidget *spin, gint min)
{
  gint v;

  if (g_object_get_data (G_OBJECT (spin), "auto") != NULL)
    return 0;
  v = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (spin));
  return v == 0 ? 0 : MAX (v, min);
}

static void
on_spin_touched (GtkWidget *spin, gpointer data)
{
  g_object_set_data (G_OBJECT (spin), "auto", NULL);
}

/* Pré-remplit un champ auto avec la taille effective */
static void
show_effective_value (GtkWidget *spin, gint configured, gint effective)
{
  if (configured != 0 || effective <= 0)
    return;
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), effective);
  g_object_set_data (G_OBJECT (spin), "auto", GINT_TO_POINTER (1));
}

/* Appliqué à chaque modification, sans attendre la fermeture */
static void
on_option_changed (GtkWidget *widget, ClaudePlugin *cp)
{
  GObject *d = G_OBJECT (cp->dialog);
  gint interval;

  if (cp->dead || cp->dialog == NULL)
    return;
  interval = gtk_spin_button_get_value_as_int (g_object_get_data (d, "interval"));
  cp->bar_width = spin_value_or_auto (g_object_get_data (d, "bar_width"), MIN_BAR_WIDTH);
  cp->style.bar_height = spin_value_or_auto (g_object_get_data (d, "bar_height"), MIN_BAR_HEIGHT);
  cp->style.show_percent = gtk_toggle_button_get_active (g_object_get_data (d, "show_percent"));
  cp->style.show_labels = gtk_toggle_button_get_active (g_object_get_data (d, "show_labels"));
  cp->show_remaining = gtk_toggle_button_get_active (g_object_get_data (d, "show_remaining"));
  cp->click_action = gtk_combo_box_get_active (g_object_get_data (d, "click_action"));
  g_free (cp->click_command);
  cp->click_command = g_strdup (gtk_entry_get_text (g_object_get_data (d, "click_command")));
  gtk_widget_set_sensitive (g_object_get_data (d, "click_command"),
                            cp->click_action == CLICK_RUN_COMMAND);
  g_clear_pointer (&cp->click_error, g_free);
  update_tooltip (cp);
  update_live_timer (cp);

  if (interval != cp->interval)
    {
      cp->interval = interval;
      restart_timer (cp);
    }
  on_size_changed (cp->plugin, xfce_panel_plugin_get_size (cp->plugin), cp);
  gtk_widget_queue_draw (cp->area);
  on_save (cp->plugin, cp);
}

static void
on_configure_response (GtkWidget *dialog, gint response, ClaudePlugin *cp)
{
  if (cp->dead)
    return;
  cp->dialog = NULL;
  xfce_panel_plugin_unblock_menu (cp->plugin);
  gtk_widget_destroy (dialog);
}

static GtkWidget *
add_spin_row (GtkGrid *grid, gint row, const gchar *text, gint min, gint max, gint step,
              gint value, const gchar *key, GtkWidget *dialog, ClaudePlugin *cp)
{
  GtkWidget *label = gtk_label_new (text);
  GtkWidget *spin = gtk_spin_button_new_with_range (min, max, step);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), value);
  gtk_grid_attach (grid, label, 0, row, 1, 1);
  gtk_grid_attach (grid, spin, 1, row, 1, 1);
  g_object_set_data (G_OBJECT (dialog), key, spin);
  /* on_spin_touched d'abord : le marqueur auto est levé avant la lecture */
  g_signal_connect (spin, "value-changed", G_CALLBACK (on_spin_touched), NULL);
  g_signal_connect (spin, "value-changed", G_CALLBACK (on_option_changed), cp);
  return spin;
}

static void
add_check_row (GtkGrid *grid, gint row, const gchar *text, gboolean value,
               const gchar *key, GtkWidget *dialog, ClaudePlugin *cp)
{
  GtkWidget *check = gtk_check_button_new_with_label (text);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), value);
  gtk_grid_attach (grid, check, 0, row, 2, 1);
  g_object_set_data (G_OBJECT (dialog), key, check);
  g_signal_connect (check, "toggled", G_CALLBACK (on_option_changed), cp);
}

static void
add_click_rows (GtkGrid *grid, gint row, GtkWidget *dialog, ClaudePlugin *cp)
{
  GtkWidget *label = gtk_label_new (_("Left click action:"));
  GtkWidget *combo = gtk_combo_box_text_new ();
  GtkWidget *cmd_label = gtk_label_new (_("Command:"));
  GtkWidget *entry = gtk_entry_new ();

  /* même ordre que ClickAction */
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Open claude.ai"));
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Open Claude Desktop"));
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Run a custom command"));
  gtk_combo_box_set_active (GTK_COMBO_BOX (combo), cp->click_action);

  gtk_entry_set_text (GTK_ENTRY (entry), cp->click_command);
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "xfce4-terminal -e claude");
  gtk_widget_set_sensitive (entry, cp->click_action == CLICK_RUN_COMMAND);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_xalign (GTK_LABEL (cmd_label), 0.0);
  gtk_grid_attach (grid, label, 0, row, 1, 1);
  gtk_grid_attach (grid, combo, 1, row, 1, 1);
  gtk_grid_attach (grid, cmd_label, 0, row + 1, 1, 1);
  gtk_grid_attach (grid, entry, 1, row + 1, 1, 1);

  g_object_set_data (G_OBJECT (dialog), "click_action", combo);
  g_object_set_data (G_OBJECT (dialog), "click_command", entry);
  g_signal_connect (combo, "changed", G_CALLBACK (on_option_changed), cp);
  g_signal_connect (entry, "changed", G_CALLBACK (on_option_changed), cp);
}

static void
on_configure (XfcePanelPlugin *plugin, ClaudePlugin *cp)
{
  GtkWidget *dialog, *grid, *width_spin, *height_spin;

  xfce_panel_plugin_block_menu (plugin);
  dialog = xfce_titled_dialog_new_with_mixed_buttons (
    _("Usage Monitor for Claude"), GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (plugin))),
    GTK_DIALOG_DESTROY_WITH_PARENT, "window-close", _("_Close"), GTK_RESPONSE_OK, NULL);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_container_set_border_width (GTK_CONTAINER (grid), 12);

  add_spin_row (GTK_GRID (grid), 0, _("Refresh interval (seconds):"), MIN_INTERVAL, MAX_INTERVAL,
                30, cp->interval, "interval", dialog, cp);
  width_spin = add_spin_row (GTK_GRID (grid), 1, _("Bar width (px, 0 = auto):"), 0,
                             MAX_BAR_WIDTH, 10, cp->bar_width, "bar_width", dialog, cp);
  height_spin = add_spin_row (GTK_GRID (grid), 2, _("Bar height (px, 0 = auto):"), 0,
                              MAX_BAR_HEIGHT, 1, cp->style.bar_height, "bar_height", dialog, cp);
  show_effective_value (width_spin, cp->bar_width,
                        gtk_widget_get_allocated_width (GTK_WIDGET (plugin)));
  show_effective_value (height_spin, cp->style.bar_height,
                        bars_auto_bar_height (gtk_widget_get_allocated_height (cp->area), 2));
  add_check_row (GTK_GRID (grid), 3, _("Show percentages"), cp->style.show_percent,
                 "show_percent", dialog, cp);
  add_check_row (GTK_GRID (grid), 4, _("Show labels (5h / 7d)"), cp->style.show_labels,
                 "show_labels", dialog, cp);
  add_check_row (GTK_GRID (grid), 5, _("Show time left before reset"), cp->show_remaining,
                 "show_remaining", dialog, cp);
  add_click_rows (GTK_GRID (grid), 6, dialog, cp);
  gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), grid);

  /* cp->dialog n'est renseigné qu'après la construction : on_option_changed
     l'ignore tant que les valeurs initiales sont posées */
  cp->dialog = dialog;
  g_signal_connect (dialog, "response", G_CALLBACK (on_configure_response), cp);
  gtk_widget_show_all (dialog);
}

/* --- Cycle de vie ------------------------------------------------------ */

static void
on_free_data (XfcePanelPlugin *plugin, ClaudePlugin *cp)
{
  if (cp->timer_id != 0)
    g_source_remove (cp->timer_id);
  if (cp->live_timer_id != 0)
    g_source_remove (cp->live_timer_id);
  /* Pas d'annulation : usage_fetch_async n'appelle pas le callback sur
     * annulation, alors fetch_busy ne serait jamais remis à FALSE. La requête
     * (timeout 20 s) se termine et on_fetch_cb libère cp. */
  if (cp->dialog != NULL)
    {
      g_signal_handlers_disconnect_by_data (cp->dialog, cp);
      gtk_widget_destroy (cp->dialog);
      cp->dialog = NULL;
      xfce_panel_plugin_unblock_menu (plugin);
    }
  cp->dead = TRUE;
  if (!cp->stats_busy && !cp->fetch_busy)
    claude_plugin_destroy (cp); /* sinon le dernier callback en vol s'en charge */
}

static void
claude_construct (XfcePanelPlugin *plugin)
{
  ClaudePlugin *cp = g_new0 (ClaudePlugin, 1);
  gchar *dir;

  xfce_textdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR, "UTF-8");

  cp->plugin = plugin;
  cp->status = USAGE_OK;
  claude_usage_init (&cp->usage);
  load_config (cp);

  cp->session = soup_session_new_with_options ("user-agent", "xfce4-claude-usage-plugin/0.1",
                                               "timeout", 20, NULL);
  dir = g_build_filename (g_get_home_dir (), ".claude", "projects", NULL);
  cp->stats_cache = local_stats_cache_new (dir);
  g_free (dir);

  cp->area = gtk_drawing_area_new ();
  gtk_widget_add_events (cp->area, GDK_BUTTON_PRESS_MASK);
  gtk_container_add (GTK_CONTAINER (plugin), cp->area);
  xfce_panel_plugin_add_action_widget (plugin, cp->area);
  gtk_widget_set_has_tooltip (cp->area, TRUE);
  g_signal_connect (cp->area, "query-tooltip", G_CALLBACK (on_query_tooltip), cp);

  g_signal_connect (cp->area, "draw", G_CALLBACK (on_draw), cp);
  g_signal_connect (cp->area, "button-press-event", G_CALLBACK (on_button_press), cp);
  g_signal_connect (plugin, "size-changed", G_CALLBACK (on_size_changed), cp);
  g_signal_connect (plugin, "save", G_CALLBACK (on_save), cp);
  g_signal_connect (plugin, "configure-plugin", G_CALLBACK (on_configure), cp);
  g_signal_connect (plugin, "free-data", G_CALLBACK (on_free_data), cp);
  xfce_panel_plugin_menu_show_configure (plugin);

  gtk_widget_show_all (GTK_WIDGET (plugin));
  update_tooltip (cp);
  refresh (cp);
  restart_timer (cp);
  update_live_timer (cp);
}

XFCE_PANEL_PLUGIN_REGISTER (claude_construct);
