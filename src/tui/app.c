#include "app.h"
#include "agent/loop.h"
#include "session/store.h"
#include "util/utf8.h"
#include "agent/events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct mp_tui { int alt; mp_store *store; };
mp_tui *mp_tui_new(int alt) { mp_tui *t = calloc(1, sizeof *t); t->alt = alt; return t; }
void mp_tui_free(mp_tui *t) { if (!t) return; if (t->store) mp_store_close(t->store); free(t); }
/* Headless loop: read lines from stdin, run agent turn, print events. */
static void print_cb(const mp_event *ev, void *ud) {
    (void)ud;
    switch (ev->kind) {
    case MP_EV_ASSISTANT_DELTA: printf("assistant: %s\n", ev->text ? ev->text : ""); break;
    case MP_EV_TOOL_CALL:       printf("[tool] %s\n", ev->text ? ev->text : "?"); break;
    case MP_EV_TOOL_RESULT:     printf("[result] %.400s\n", ev->text ? ev->text : "(null)"); break;
    default: break;
    }
    fflush(stdout);
}
int mp_tui_run(mp_tui *t, mp_agent *agent) {
    mp_bus *bus = NULL; /* bus lives in agent; subscribe via accessor below */
    extern mp_bus *mp_agent_bus(mp_agent *a);
    bus = mp_agent_bus(agent);
    mp_bus_subscribe(bus, print_cb, NULL);
    char line[8192];
    fputs("minipi (offline stub provider). Ctrl-D or /quit to exit.\n", stdout);
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (!n) continue;
        if (!mp_utf8_valid(line, n)) { puts("error: invalid UTF-8 input"); continue; }
        if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
        if (mp_agent_run_turn(agent, line)) { puts("error: turn failed"); break; }
    }
    return 0;
}
