#include "provider.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
mp_reply *mp_provider_complete(const char *model, const mp_ctx *ctx) {
    (void)model;
    /* Offline deterministic stub used only when no provider is configured:
     * echoes the last user message so the loop/session/TUI can be exercised
     * without network or keys. */
    mp_reply *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    const char *last = "(no user message)";
    for (int i = ctx->n - 1; i >= 0; i--)
        if (!strcmp(ctx->v[i].role, "user")) { last = ctx->v[i].content; break; }
    r->text = malloc(strlen(last) + 32);
    if (!r->text) { free(r); return NULL; }
    sprintf(r->text, "[stub:offline] %s", last);
    return r;
}
void mp_reply_free(mp_reply *r) { if (!r) return; free(r->text);
    if (r->tool_calls) cJSON_Delete(r->tool_calls); free(r); }
