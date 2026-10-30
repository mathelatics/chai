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
            gbuf_add(&s->reasoning, rs, rs ? strlen(rs) : 0);
            gbuf_add(&s->content, cs, cs ? strlen(cs) : 0);
            if (s->cb) s->cb(rs, cs, s->ud);
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
    cJSON_AddItemToObject(body, "messages", mp_ctx_to_json(ctx));
    cJSON_AddNumberToObject(body, "max_tokens", max_tokens > 0 ? max_tokens : 16384);
    cJSON_AddNumberToObject(body, "temperature", temperature);
    cJSON_AddNumberToObject(body, "top_p", top_p > 0 ? top_p : 0.95);
    cJSON *ctk = cJSON_CreateObject();
    cJSON_AddBoolToObject(ctk, "enable_thinking", enable_thinking ? 1 : 0);
    cJSON_AddItemToObject(body, "chat_template_kwargs", ctk);
    char *txt = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!txt) return NULL;

    /* Broker path encodes the upstream host: /<host>/chat/completions */
    static const char kHost[] = "integrate.api.nvidia.com";
    char path[256];
    snprintf(path, sizeof path, "/%s/chat/completions", kHost);
    char *resp = NULL; size_t rlen = 0;
    int status = mp_broker_post(path, txt, strlen(txt), &resp, &rlen);
    free(txt);
    if (status < 0) { log_error("nemotron: broker unreachable (start tools/tls_broker.py)"); free(resp); return NULL; }
    if (status != 200) {
        log_error("nemotron: HTTP %d: %.512s", status, resp ? resp : "");
        free(resp); return NULL;
    }
    sctx s = { on_delta, ud, {0}, {0}, 0, 0, 0 };
    mp_sse *sse = mp_sse_new(on_sse_event, &s);
    mp_sse_feed(sse, resp, rlen);
    mp_sse_flush(sse);
    mp_sse_free(sse);
    free(resp);

    mp_reply *r = calloc(1, sizeof *r);
    r->text = s.content.buf ? s.content.buf : strdup("");
    r->input_tokens = s.in_tok; r->output_tokens = s.out_tok;
    if (s.reasoning.len) { /* stash reasoning as first field for TUI display */
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "reasoning", s.reasoning.buf);
        r->tool_calls = NULL; /* tool calls parsed in M3 from delta.tool_calls */
        free(meta); /* M3: wire real tool-call accumulation */
    }
    free(s.reasoning.buf);
    return r;
}
