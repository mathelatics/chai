/* TUI entry point. On a real terminal this launches the full-screen termbox2
 * front-end (transcript, streaming, status bar, model picker, approval modals).
 * When stdin/stdout are pipes or the terminal cannot be initialized, it falls
 * back to the line REPL so scripts, CI and `--prompt`-style use keep working. */
#include "app.h"
#include "tui/fullscreen.h"
#include <stdio.h>
#include <unistd.h>

int mp_tui_main(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *sessdir) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return mp_repl_run(agent, model, thinking, max_tokens, sessdir);
    int rc = mp_tui_fullscreen(agent, model, thinking, max_tokens, sessdir);
    if (rc == MP_TUI_FALLBACK) {
        fprintf(stderr, "[tui] full-screen mode unavailable; using line REPL\n");
        return mp_repl_run(agent, model, thinking, max_tokens, sessdir);
    }
    return rc;
}
