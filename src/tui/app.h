#ifndef MINIPI_TUI_APP_H
#define MINIPI_TUI_APP_H
/* TUI front-end (termbox2) + headless fallback for pipes/CI. */
#include "agent/loop.h"
typedef struct mp_tui mp_tui;
mp_tui *mp_tui_new(int want_alt_screen);   /* 0 => headless line REPL on stdin */
int     mp_tui_run(mp_tui *t, struct mp_agent *agent); /* drives turns until quit */
void    mp_tui_free(mp_tui *t);
#endif
