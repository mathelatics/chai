#ifndef MINIPI_TUI_APP_H
#define MINIPI_TUI_APP_H
/* TUI front-end entry point. --tui runs the same event-driven REPL core over
 * a tty (raw-mode editor + live region + sanitized streaming); when stdout is
 * not a tty it degrades gracefully to piped-line mode, so CI and pipes keep
 * working. The interactive core lives in repl.c; this header is the wiring
 * surface used by main.c. */
#include "agent/loop.h"

/* Defined in repl.c: the shared interactive driver (also the default path). */
int mp_repl_run(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *sessdir);

/* Defined in app.c: full-screen entry for --tui. Delegates to mp_repl_run
 * (ANSI live-region stack); termbox2 overlay activates via render.c if the
 * build defines MINIPI_WITH_TERMBOX2. */
int mp_tui_main(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *sessdir);
#endif
