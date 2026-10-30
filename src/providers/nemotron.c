/* NVIDIA Nemotron streaming via the localhost TLS broker (M2).
 * minipi stays MIT-only + plaintext; tools/tls_broker.py owns TLS + API key.
 * Wire: POST /chat/completions to http://127.0.0.1:$MINIPI_BROKER_PORT/
 *       <upstream-host>/chat/completions  -> SSE body parsed with net/sse.c.
 * Handles delta.content AND delta.reasoning_content (nemotron thinking),
 * finish_reason, and [DONE]. */
#include "providers/provider.h"
#include "net/sse.h"
#include "net/broker_client.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *buf; size_t len, cap; } gbuf;
static void gbuf_add(gbuf *g, const char *s, size_t n) {
    if (!s || !n) return;
    if (g->len + n + 1 > g->cap) {
        size_t nc = (g->cap ? g->cap : 4096);
        while (nc < g->len + n + 1) nc *= 2;
        g->buf = realloc(g->buf, nc); g->cap = nc;
    }
    memcpy(g->buf + g->len, s, n); g->len += n; g->buf[g->len] = 0;
}

typedef struct { mp_stream_fn cb; void *ud; gbuf content, reasoning; long in_tok, out_tok; int err; } sctx;

/* Accumulator for streamed tool_calls: delta.tool_calls[] fragments keyed by index. */
#define MAX_TC 16
typedef struct { char id[64]; char name[128]; gbuf args; int present; } tcslot;
static tcslot g_tc[MAX_TC];

static void on_sse_event(const char *event, const char *data, void *p) {
    (void)event;
    sctx *s = p;
    if (!strncmp(data, "[DONE]", 6)) return;
    cJSON *j = cJSON_Parse(data);
    if (!j) return; /* partial/keepalive line */
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(j, "choices");
    if (cJSON_IsArray(ch) && ch->child) {
        const cJSON *delta = cJSON_GetObjectItemCaseSensitive(ch->child, "delta");
        if (delta) {
            const cJSON *rc = cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
            const cJSON *cc = cJSON_GetObjectItemCaseSensitive(delta, "content");
            const char *rs = cJSON_IsString(rc) ? rc->valuestring : NULL;
            const char *cs = cJSON_IsString(cc) ? cc->valuestring : NULL;
            int has_text = (rs && *rs) || (cs && *cs);
            gbuf_add(&s->reasoning, rs, rs ? strlen(rs) : 0);
            gbuf_add(&s->content, cs, cs ? strlen(cs) : 0);
            /* nemotron emits role-only / empty deltas first — don't print those */
            if (has_text && s->cb) s->cb(rs, cs, s->ud);
            /* tool call fragments */
            const cJSON *tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
            if (cJSON_IsArray(tcs)) {
                const cJSON *t = NULL;
                cJSON_ArrayForEach(t, tcs) {
                    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(t, "index");
                    int i = cJSON_IsNumber(idx) ? (int)idx->valuedouble : 0;
                    if (i < 0 || i >= MAX_TC) continue;
                    tcslot *sl = &g_tc[i]; sl->present = 1;
                    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(t, "function");
                    const cJSON *id = cJSON_GetObjectItemCaseSensitive(t, "id");
                    if (cJSON_IsString(id)) snprintf(sl->id, sizeof sl->id, "%s", id->valuestring);
                    if (fn) {
                        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(fn, "name");
                        const cJSON *ar = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
                        if (cJSON_IsString(nm) && *nm->valuestring)
                            snprintf(sl->name, sizeof sl->name, "%s", nm->valuestring);
                        if (cJSON_IsString(ar)) gbuf_add(&sl->args, ar->valuestring, strlen(ar->valuestring));
                    }
                }
            }
        }
    }
    const cJSON *use = cJSON_GetObjectItemCaseSensitive(j, "usage");
    if (cJSON_IsObject(use)) {
        const cJSON *pi = cJSON_GetObjectItemCaseSensitive(use, "prompt_tokens");
        const cJSON *co = cJSON_GetObjectItemCaseSensitive(use, "completion_tokens");
        if (cJSON_IsNumber(pi)) s->in_tok = (long)pi->valuedouble;
        if (cJSON_IsNumber(co)) s->out_tok = (long)co->valuedouble;
    }
    cJSON_Delete(j);
}

mp_reply *mp_nemotron_stream(const char *model, const mp_ctx *ctx,
                             int enable_thinking, long max_tokens, double temperature,
                             double top_p, mp_stream_fn on_delta, void *ud) {
    /* Build OpenAI-compatible request body. NOTE: NVIDIA rejects unknown params;
     * chat_template_kwargs goes at TOP LEVEL (not extra_body — that's SDK-only). */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model ? model : "nvidia/nemotron-3.5-lightning-30b-a3b");
    cJSON_AddBoolToObject(body, "stream", 1);
    /* messages: strip our internal "tool" role -> fold into user msg so the API accepts it */
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; ctx && i < ctx->n; i++) {
        const char *role = ctx->v[i].role;
        if (!strcmp(role, "tool")) role = "user";
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", role);
        cJSON_AddStringToObject(m, "content", ctx->v[i].content ? ctx->v[i].content : "");
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON_AddItemToObject(body, "messages", msgs);
    cJSON_AddNumberToObject(body, "max_tokens", max_tokens > 0 ? max_tokens : 16384);
    cJSON_AddNumberToObject(body, "temperature", temperature);
    cJSON_AddNumberToObject(body, "top_p", top_p > 0 ? top_p : 0.95);
    cJSON *ctk = cJSON_CreateObject();
    cJSON_AddBoolToObject(ctk, "enable_thinking", enable_thinking ? 1 : 0);
    cJSON_AddItemToObject(body, "chat_template_kwargs", ctk);
    char *txt = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!txt) return NULL;

    /* Broker path encodes the upstream host + real API path: /<host>/v1/chat/completions */
    static const char kHost[] = "integrate.api.nvidia.com";
    char path[256];
    snprintf(path, sizeof path, "/%s/v1/chat/completions", kHost);
    char *resp = NULL; size_t rlen = 0;
    int status = mp_broker_post(path, txt, strlen(txt), &resp, &rlen);
    free(txt);
    if (status < 0) { log_error("nemotron: broker unreachable (start tools/tls_broker.py)"); free(resp); return NULL; }
    if (status != 200) {
        log_error("nemotron: HTTP %d: %.512s", status, resp ? resp : "");
        free(resp); return NULL;
    }
    sctx s = { on_delta, ud, {0}, {0}, 0, 0, 0 };
    for (int i = 0; i < MAX_TC; i++) memset(&g_tc[i], 0, sizeof g_tc[i]);
    mp_sse *sse = mp_sse_new(on_sse_event, &s);
    mp_sse_feed(sse, resp, rlen);
    mp_sse_flush(sse);
    mp_sse_free(sse);
    free(resp);

    /* Build tool_calls array from accumulated fragments */
    cJSON *tcarr = NULL;
    for (int i = 0; i < MAX_TC; i++) {
        if (!g_tc[i].present) continue;
        if (!tcarr) tcarr = cJSON_CreateArray();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", g_tc[i].id);
        cJSON_AddStringToObject(o, "name", g_tc[i].name);
        cJSON *args = g_tc[i].args.buf ? cJSON_Parse(g_tc[i].args.buf) : NULL;
        cJSON_AddItemToObject(o, "arguments", args ? args : cJSON_CreateObject());
        cJSON_AddItemToArray(tcarr, o);
        free(g_tc[i].args.buf); g_tc[i].args.buf = NULL;
    }

    mp_reply *r = calloc(1, sizeof *r);
    r->text = s.content.buf ? s.content.buf : strdup("");
    r->tool_calls = tcarr;
    r->input_tokens = s.in_tok; r->output_tokens = s.out_tok;
    free(s.reasoning.buf);
    return r;
}
