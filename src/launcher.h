#pragma once
#include <glib.h>

#define CLAUDE_URL "https://claude.ai/"

typedef enum {
  CLICK_OPEN_WEB,
  CLICK_OPEN_DESKTOP,
  CLICK_RUN_COMMAND,
} ClickAction;

const gchar *click_action_to_string (ClickAction action);
ClickAction click_action_from_string (const gchar *s); /* inconnu → CLICK_OPEN_WEB */

/* Vrai si le lanceur (id .desktop, nom affiché) est Claude Desktop */
gboolean launcher_is_claude_desktop (const gchar *id, const gchar *name);

/* Exécute l'action ; command n'est utilisé que pour CLICK_RUN_COMMAND */
gboolean launcher_run (ClickAction action, const gchar *command, GError **error);
