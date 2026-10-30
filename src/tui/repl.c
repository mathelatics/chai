/* minipi REPL (pure C, no Python anywhere in the agent path).
 * Interactive CLI: reads user lines, streams Nemotron/OpenAI-compatible replies
 * through the localhost TLS broker, executes tool calls via the registry, and
 * persists each turn to the JSONL session store. */
#include "tui/app.h"
#include "agent/loop.h"
#include "providers/provider.h"
#include "tools/registry.h"
#include "session/store.h"
#include "util/utf8.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { mp_ctx *ctx; mp_agent *agent; mp_toolreg *tools; const char *model;
                 int thinking; long max_tokens; mp_store *store; } repl_state;

static void stream_cb(const char *reasoning, const char *content, void *ud) {
    (void)ud;
    if (reasoning) fputs(reasoning, stdout);
    if (content)   fputs(content, stdout);
    fflush(stdout);
}

static void persist(repl_state *st, const char *role, const char *content, const cJSON *tc) {
    if (!st->store) return;
    char *tcs = tc ? cJSON_PrintUnformatted(tc) : NULL;
    mp_store_append(st->store, role, content, tcs, NULL);
    free(tcs);
}

/* One full agent turn against a streaming provider. Returns 0 ok. */
static int repl_turn(repl_state *st, const char *text) {
    mp_ctx_push(st->ctx, "user", text, NULL);
    persist(st, "user", text, NULL);
    for (int turn = 0; turn < 8; turn++) {
        printf("\n");
        mp_reply *r = mp_nemotron_stream(st->model, st->ctx,
                                         st->thinking, st->max_tokens, 1.0, 0.95,
                                         stream_cb, NULL);
        if (!r) { fprintf(stderr, "\n[error: provider call failed — is the broker running?]\n"); return -1; }
        printf("\n");
        mp_ctx_push(st->ctx, "assistant", r->text ? r->text : "", NULL);
        persist(st, "assistant", r->text ? r->text : "", NULL);
        int had_tools = (r->tool_calls && cJSON_IsArray(r->tool_calls) && r->tool_calls->child);
        if (had_tools) {
            const cJSON *tc = NULL;
            cJSON_ArrayForEach(tc, r->tool_calls) {
                const cJSON *nm = cJSON_GetObjectItemCaseSensitive(tc, "name");
                const cJSON *args = cJSON_GetObjectItemCaseSensitive(tc, "arguments");
                const char *name = cJSON_IsString(nm) ? nm->valuestring : "";
                printf("[tool] %s\n", name);
                char *res = mp_tool_execute(st->tools, name, args);
                printf("[result] %.400s\n", res ? res : "(error)");
                mp_ctx_push(st->ctx, "tool", res ? res : "(error)", cJSON_Duplicate(tc, 1));
                persist(st, "tool", res ? res : "(error)", tc);
                free(res);
            }
            mp_reply_free(r);
            continue; /* feed results back to model */
        }
        mp_reply_free(r);
        break;
    }
    return 0;
}

int mp_repl_run(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *session_dir) {
    extern mp_toolreg *mp_agent_tools(mp_agent *a);
    repl_state st = { mp_agent_ctx(agent), agent, mp_agent_tools(agent), model,
                      thinking, max_tokens > 0 ? max_tokens : 16384, NULL };
    /* ensure the system instruction is present exactly once */
    int has_sys = 0;
    for (int i = 0; i < st.ctx->n; i++) if (!strcmp(st.ctx->v[i].role, "system")) { has_sys = 1; break; }
    if (!has_sys) mp_ctx_push(st.ctx, "system",
        "You are MiniPi, a careful coding agent running in C. Be concise.", NULL);
    if (session_dir) st.store = mp_store_open(session_dir, "repl");
    printf("minipi v0.1 — model=%s thinking=%d (TLS broker on 127.0.0.1:$MINIPI_BROKER_PORT or :8777)\n",
           model ? model : "nvidia/nemotron-3.5-lightning-30b-a3b", thinking);
    printf("Type your request. /quit exits. Ctrl-D exits.\n");
    char line[8192];
    for (;;) {
        if (isatty(0)) { fputs("\n> ", stdout); fflush(stdout); }
        if (!fgets(line, sizeof line, stdin)) break;
        size_t n = strlen(line);
        while (n && (line[n-1]=='\n' || line[n-1]=='\r')) line[--n] = '\0';
        if (!n) continue;
        if (!mp_utf8_valid(line, n)) { puts("error: invalid UTF-8 input"); continue; }
        if (!strcmp(line, "/quit") || !strcmp(line, "/exit")) break;
        if (!strcmp(line, "/help")) { puts("/quit /help — other input goes to the model"); continue; }
        if (repl_turn(&st, line)) break;
    }
    if (st.store) mp_store_close(st.store);
    return 0;
}
