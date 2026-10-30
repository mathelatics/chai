/* NVIDIA Nemotron / OpenAI-compatible streaming (M2+).
 * Two transports:
 *   - cloud kinds (nemotron/openai/anthropic-hosted): plaintext HTTP to the
 *     localhost TLS broker (tools/tls_broker.py); key lives ONLY in broker env.
 *   - openai-local kind (LM Studio/Ollama/llama.cpp): direct plaintext POST,
 *     loopback-only enforced here; no key involved.
 * Handles delta.content, delta.reasoning_content (nemotron thinking), and
 * streamed tool_call fragments (accumulated per index, emitted as mp_reply).
 * Sends function schemas so models can actually emit tool calls (P2 fix). */
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
static void gbuf_free(gbuf *g) { free(g->buf); g->buf = NULL; g->len = g->cap = 0; }

#define MAX_TC 16
typedef struct { char id[64]; char name[128]; gbuf args; int present; } tcslot;

typedef struct { mp_stream_fn cb; void *ud; gbuf content, reasoning; long in_tok, out_tok; tcslot tc[MAX_TC]; } sctx;

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
            /* role-only / empty deltas first — don't print those */
            if (has_text && s->cb) s->cb(rs, cs, s->ud);
            const cJSON *tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
            if (cJSON_IsArray(tcs)) {
                const cJSON *t = NULL;
                cJSON_ArrayForEach(t, tcs) {
                    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(t, "index");
                    int i = cJSON_IsNumber(idx) ? (int)idx->valuedouble : 0;
                    if (i < 0 || i >= MAX_TC) continue;
                    tcslot *sl = &s->tc[i]; sl->present = 1;
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

/* Convert our registry schema array [{name,schema}] -> OpenAI tools[] format */
static cJSON *tools_to_openai(const cJSON *schemas) {
    if (!cJSON_IsArray(schemas) || !schemas->child) return NULL;
    cJSON *arr = cJSON_CreateArray();
    const cJSON *s = NULL;
    cJSON_ArrayForEach(s, schemas) {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(s, "name");
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(s, "schema");
        if (!cJSON_IsString(nm)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", nm->valuestring);
        cJSON_AddItemToObject(fn, "parameters", sc ? cJSON_Duplicate(sc, 1) : cJSON_CreateObject());
        cJSON_AddItemToObject(o, "function", fn);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* Build the chat-completions body shared by both transports. */
static char *build_body(const char *model, const mp_ctx *ctx, int enable_thinking,
                        long max_tokens, double temperature, double top_p,
                        const cJSON *tools /*[{name,schema}] or NULL*/) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model ? model : "nvidia/nemotron-3.5-lightning-30b-a3b");
    cJSON_AddBoolToObject(body, "stream", 1);
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; ctx && i < ctx->n; i++) {
        const char *role = ctx->v[i].role;
        cJSON *m = cJSON_CreateObject();
        if (!strcmp(role, "assistant") && ctx->v[i].tool_call) {
            /* assistant message carrying a tool call (OpenAI wire format) */
            cJSON_AddStringToObject(m, "role", "assistant");
            cJSON_AddItemToObject(m, "content", cJSON_CreateNull());
            cJSON *tc = cJSON_CreateArray();
            cJSON *one = cJSON_CreateObject();
            cJSON_AddStringToObject(one, "id", "");
            const cJSON *idj = cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "id");
            if (cJSON_IsString(idj)) { free(one->child); one->child = NULL;
                cJSON_AddStringToObject(one, "id", idj->valuestring); }
            const cJSON *nmj = cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "name");
            const cJSON *arj = cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "arguments");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", cJSON_IsString(nmj) ? nmj->valuestring : "");
            char *as = arj ? cJSON_PrintUnformatted(arj) : strdup("{}");
            cJSON_AddStringToObject(fn, "arguments", as ? as : "{}");
            free(as);
            cJSON_AddItemToObject(one, "function", fn);
            cJSON_AddItemToArray(tc, one);
            cJSON_AddItemToObject(m, "tool_calls", tc);
            cJSON_AddItemToArray(msgs, m);
            continue;
        }
        if (!strcmp(role, "tool")) {
            cJSON_AddStringToObject(m, "role", "tool");
            cJSON_AddStringToObject(m, "content", ctx->v[i].content ? ctx->v[i].content : "");
            const cJSON *idj = ctx->v[i].tool_call ?
                cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "id") : NULL;
            cJSON_AddStringToObject(m, "tool_call_id", cJSON_IsString(idj) ? idj->valuestring : "call_0");
            cJSON_AddItemToArray(msgs, m);
            continue;
        }
        if (!strcmp(role, "tool")) role = "user";
        cJSON_AddStringToObject(m, "role", role);
        cJSON_AddStringToObject(m, "content", ctx->v[i].content ? ctx->v[i].content : "");
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON_AddItemToObject(body, "messages", msgs);
    cJSON_AddNumberToObject(body, "max_tokens", max_tokens > 0 ? max_tokens : 16384);
    cJSON_AddNumberToObject(body, "temperature", temperature);
    cJSON_AddNumberToObject(body, "top_p", top_p > 0 ? top_p : 0.95);
    cJSON *t = tools_to_openai(tools);
    if (t) { cJSON_AddItemToObject(body, "tools", t); cJSON_AddStringToObject(body, "tool_choice", "auto"); }
    /* nemotron-specific; harmless-ish elsewhere but only add when asked */
    if (enable_thinking) {
        cJSON *ctk = cJSON_CreateObject();
        cJSON_AddBoolToObject(ctk, "enable_thinking", 1);
        cJSON_AddItemToObject(body, "chat_template_kwargs", ctk);
    }
    char *txt = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return txt;
}

static mp_reply *finish(sctx *s) {
    cJSON *tcarr = NULL;
    for (int i = 0; i < MAX_TC; i++) {
        if (!s->tc[i].present) continue;
        if (!tcarr) tcarr = cJSON_CreateArray();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s->tc[i].id[0] ? s->tc[i].id : "call_x");
        cJSON_AddStringToObject(o, "name", s->tc[i].name);
        cJSON *args = s->tc[i].args.buf ? cJSON_Parse(s->tc[i].args.buf) : NULL;
        cJSON_AddItemToObject(o, "arguments", args ? args : cJSON_CreateObject());
        cJSON_AddItemToArray(tcarr, o);
        gbuf_free(&s->tc[i].args);
    }
    mp_reply *r = calloc(1, sizeof *r);
    r->text = s->content.buf ? s->content.buf : strdup("");
    r->tool_calls = tcarr;
    r->input_tokens = s->in_tok; r->output_tokens = s->out_tok;
    gbuf_free(&s->reasoning);
    return r;
}

static mp_reply *run_parsed(char *resp, size_t rlen, sctx *s) {
    mp_sse *sse = mp_sse_new(on_sse_event, s);
    mp_sse_feed(sse, resp, rlen);
    mp_sse_flush(sse);
    mp_sse_free(sse);
    return finish(s);
}

mp_reply *mp_nemotron_stream(const char *model, const mp_ctx *ctx,
                             int enable_thinking, long max_tokens, double temperature,
                             double top_p, mp_stream_fn on_delta, void *ud) {
    return mp_nemotron_stream_ex(model, ctx, enable_thinking, max_tokens, temperature,
                                 top_p, on_delta, ud, NULL, NULL, 0);
}

/* Extended entry used by the REPL/provider router:
 *  tools:      registry schema array (may be NULL)
 *  local_base: "http://127.0.0.1:PORT/v1" for openai-local providers, else NULL
 *  broker_host: upstream host for cloud path (e.g. integrate.api.nvidia.com) */
mp_reply *mp_nemotron_stream_ex(const char *model, const mp_ctx *ctx,
                                int enable_thinking, long max_tokens, double temperature,
                                double top_p, mp_stream_fn on_delta, void *ud,
                                const cJSON *tools, const char *local_base,
                                const char *broker_host) {
    char *txt = build_body(model, ctx, enable_thinking, max_tokens, temperature, top_p, tools);
    if (!txt) return NULL;
    sctx s; memset(&s, 0, sizeof s);
    s.cb = on_delta; s.ud = ud;
    int status = -1; char *resp = NULL; size_t rlen = 0;

    if (local_base && *local_base) {
        if (strncmp(local_base, "http://127.0.0.1", 16) && strncmp(local_base, "http://localhost", 16)) {
            log_error("provider: refusing non-loopback base_url for local provider: %s", local_base);
            free(txt); return NULL;
        }
        status = mp_http_post_json_raw(local_base, "/chat/completions", txt, &resp, &rlen);
    } else {
        static const char kDefaultHost[] = "integrate.api.nvidia.com";
        const char *host = (broker_host && *broker_host) ? broker_host : kDefaultHost;
        char path[256];
        snprintf(path, sizeof path, "/%s/v1/chat/completions", host);
        status = mp_broker_post(path, txt, strlen(txt), &resp, &rlen);
    }
    free(txt);
    if (status < 0) { log_error("provider: transport failed (cloud? start tools/tls_broker.py; local? is the server up?)"); free(resp); return NULL; }
    if (status != 200) { log_error("provider: HTTP %d: %.512s", status, resp ? resp : ""); free(resp); return NULL; }
    mp_reply *r = run_parsed(resp, rlen, &s);
    free(resp);
    return r;
}
