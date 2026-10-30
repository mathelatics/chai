#include "provider.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static char g_base[512] = "";  /* empty => offline stub mode */
int mp_provider_set_base_url(const char *url) {
    if (!url || strlen(url) >= sizeof g_base) return -1;
    snprintf(g_base, sizeof g_base, "%s", url); return 0;
}
mp_reply *mp_provider_complete(const char *model, const mp_ctx *ctx) {
    (void)model;
    if (!*g_base) {
        /* Offline deterministic stub: echoes last user message. Lets M1-M4 run
         * end-to-end without network. Real providers land in M2/M3. */
        mp_reply *r = calloc(1, sizeof *r);
        const char *last = "(no user message)";
        for (int i = ctx->n - 1; i >= 0; i--)
            if (!strcmp(ctx->v[i].role, "user")) { last = ctx->v[i].content; break; }
        r->text = malloc(strlen(last) + 32);
        sprintf(r->text, "[stub:%s] %s", "offline", last);
        return r;
    }
    return NULL; /* TODO(M2): dispatch to openai/anthropic via base URL */
}
void mp_reply_free(mp_reply *r) { if (!r) return; free(r->text);
    if (r->tool_calls) cJSON_Delete(r->tool_calls); free(r); }
