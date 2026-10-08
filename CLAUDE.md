# CLAUDE.md

This is an Xfce panel plugin written in C (GTK 3, meson). It shows Claude subscription usage as bars in the panel and in a detailed tooltip.

## Build and test

```sh
meson setup build --prefix=/usr   # once; must match xfce4-panel's prefix (add --libdir=lib64 if the system uses it)
ninja -C build && meson test -C build
```

On multilib systems, if linking fails with "file in wrong format", export a `PKG_CONFIG_PATH` that points at the 64-bit `.pc` files before running meson or ninja.

- The build must stay warning-free (`warning_level=2`).
- After changing `src/plugin.c`, check that the module resolves all of its symbols:
  `python3 -c "import ctypes,os; ctypes.CDLL('build/libclaude-usage.so', mode=os.RTLD_NOW)"`.
- Installing (`sudo meson install -C build`) needs root. Leave it to the user.

## Layout

| File | Role |
|---|---|
| `src/pricing.c` | per-model price table, matched by prefix (longest prefix first) |
| `src/usage-api.c` | reads the OAuth token and plan from `~/.claude/.credentials.json`; tolerant parser for `GET /api/oauth/usage` (`limits[]`, falling back to `five_hour`/`seven_day`, plus `seven_day_breakdown`) |
| `src/local-stats.c` | incremental scan of `~/.claude/projects/**/*.jsonl`; runs in a `GTask` thread, one scan at a time |
| `src/bars.c` | Cairo drawing of the bars (Claude orange `#D97757`), shared by the panel and the tooltip |
| `src/launcher.c` | left-click action: claude.ai, Claude Desktop, or a custom command |
| `src/plugin.c` | Xfce glue: timers, custom tooltip, properties dialog, config in the rc file |

`pricing`, `usage-api` and `local-stats` don't depend on GTK. Each module has a test in `tests/`, and `tests/fixtures/usage-full.json` holds a real API response.

## Conventions

- **Language:** code comments are in French. User-visible strings are in English, wrapped in `_()`.
- **Changing user-visible strings:** add any new source file to `po/POTFILES`, then run `ninja -C build xfce4-claude-usage-plugin-update-po`, then translate `po/fr.po` and `po/es.po`. In `po/en.po`, each translation is the source string itself. Never leave fuzzy entries.
- **Format specifiers in `_()`:** `xgettext` can't parse `G_GINT64_FORMAT` concatenation inside `_()`. Use `%d` with a cast.
- **Robustness:** the usage endpoint is undocumented. Parse defensively: check types and skip malformed entries. Never make an extra API call.
- **Plugin lifetime:** `ClaudePlugin` is freed by the last pending callback (`fetch_busy`, `stats_busy`, `dead`). Keep this invariant when adding async work.

## Testing in the real panel

- `xfce4-panel -r` restarts the panel but keeps its environment. To test a language, run `xfce4-panel -q; LANGUAGE=fr xfce4-panel`. Afterwards, restart without it: `env -u LANGUAGE setsid xfce4-panel &`. Otherwise the whole panel stays in that language.
- To check the tooltip visually, build a scratch program that `#include`s `src/plugin.c`, calls `build_tooltip()` on a filled `ClaudePlugin`, and saves it with `gtk_offscreen_window_get_pixbuf()`.
