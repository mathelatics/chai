#ifndef MINIPI_TUI_FULLSCREEN_H
#define MINIPI_TUI_FULLSCREEN_H
/* Full-screen termbox2 front-end (--tui on a real terminal).
 * Falls back to the line REPL when the terminal cannot be initialized or when
 * stdin/stdout are not ttys (pipes/CI keep working). */
#include "agent/loop.h"

#define MP_TUI_FALLBACK (-100)

int mp_tui_fullscreen(mp_agent *agent, const char *model, int thinking,
                      long max_tokens, const char *sessdir);
#endif
