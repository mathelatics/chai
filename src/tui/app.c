/* TUI application entry (--tui). Design decision (chai/docs/PLAN.md C-4):
 * ONE interactive implementation — repl.c provides the raw-mode UTF-8 editor
 * with history/tab-completion (input.c), the transient live region with
 * CSI-2026 synced frames (live.c), sanitizer-guarded stream output (disp.c),
 * and the policy-gated tool loop with approval prompts. This file is the
 * single wiring point for --tui and the future termbox2 alt-screen overlay
 * (render.c), replacing the old dead headless loop that duplicated repl.c. */
#include "app.h"
#include <stdio.h>

int mp_tui_main(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *sessdir) {
    return mp_repl_run(agent, model, thinking, max_tokens, sessdir);
}
