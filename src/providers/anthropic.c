#include "providers/provider.h"
#include "net/broker_client.h"
#include "net/sse.h"
#include "cJSON.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* Anthropic messages response parsing (non-streaming). */
mp_reply *mp_anthropic_parse_response(const cJSON *resp) {
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
    if (!cJSON_IsArray(content)) return NULL;
    mp_reply *r = calloc(1, sizeof *r);
    size_t cap = 1024, len = 0; r->text = malloc(cap); r->text[0] = 0;
    cJSON *tcarr = NULL;
    const cJSON *blk = NULL;
    cJSON_ArrayForEach(blk, content) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(blk, "type");
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(blk, "text");
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "text") && cJSON_IsString(tx)) {
            size_t need = len + strlen(tx->valuestring) + 1;
            if (need > cap) { cap = need * 2; r->text = realloc(r->text, cap); }
            strcpy(r->text + len, tx->valuestring); len += strlen(tx->valuestring);
        } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "tool_use")) {
            if (!tcarr) tcarr = cJSON_CreateArray();
            cJSON *o = cJSON_CreateObject();
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(blk, "id");
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(blk, "name");
            const cJSON *in = cJSON_GetObjectItemCaseSensitive(blk, "input");
            cJSON_AddStringToObject(o, "id", cJSON_IsString(id) ? id->valuestring : "call_x");
            cJSON_AddStringToObject(o, "name", cJSON_IsString(nm) ? nm->valuestring : "");
            cJSON_AddItemToObject(o, "arguments", in ? cJSON_Duplicate(in, 1) : cJSON_CreateObject());
            cJSON_AddItemToArray(tcarr, o);
        }
    }
    r->tool_calls = tcarr;
    const cJSON *sr = cJSON_GetObjectItemCaseSensitive(resp, "stop_reason");
    if (cJSON_IsString(sr)) {
        if (!strcmp(sr->valuestring, "tool_use")) snprintf(r->stop_reason, sizeof r->stop_reason, "tool_calls");
        else if (!strcmp(sr->valuestring, "max_tokens")) snprintf(r->stop_reason, sizeof r->stop_reason, "length");
        else snprintf(r->stop_reason, sizeof r->stop_reason, "stop");
    }
    const cJSON *use = cJSON_GetObjectItemCaseSensitive(resp, "usage");
    if (cJSON_IsObject(use)) {
        const cJSON *pi = cJSON_GetObjectItemCaseSensitive(use, "input_tokens");
        const cJSON *co = cJSON_GetObjectItemCaseSensitive(use, "output_tokens");
        if (cJSON_IsNumber(pi)) r->input_tokens = (long)pi->valuedouble;
        if (cJSON_IsNumber(co)) r->output_tokens = (long)co->valuedouble;
    }
    return r;
}

/* ---------------- native Messages API transport (SSE) -------------------- */
typedef struct { char *buf; size_t len, cap; } abuf;
static void abuf_add(abuf *b, const char *s, size_t n) {
    if (!s || !n) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *nv = realloc(b->buf, nc);
        if (!nv) return;
        b->buf = nv; b->cap = nc;
    }
    memcpy(b->buf + b->len, s, n); b->len += n; b->buf[b->len] = 0;
}
static void abuf_free(abuf *b) { free(b->buf); memset(b, 0, sizeof *b); }

#define ABLK_MAX 16
typedef struct {
    char id[80], name[128], type[24];
    abuf json;
    int present;
} ablock;

typedef struct {
    abuf text, thinking;
    ablock blk[ABLK_MAX];
    int nblk;
    long in_tok, out_tok;
    char stop[32];
    mp_stream_fn cb;
    void *ud;
} actx;

static ablock *block_at(actx *a, int idx) {
    if (idx < 0 || idx >= ABLK_MAX) return NULL;
    ablock *b = &a->blk[idx];
    if (!b->present) { b->present = 1; if (idx + 1 > a->nblk) a->nblk = idx + 1; }
    return b;
}

static void on_anthropic_event(const char *event, const char *data, void *p) {
    (void)event;
    actx *a = p;
    cJSON *j = cJSON_Parse(data);
    if (!j) return;
    const cJSON *ty = cJSON_GetObjectItemCaseSensitive(j, "type");
    const char *type = cJSON_IsString(ty) ? ty->valuestring : "";
    if (!strcmp(type, "content_block_start")) {
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(j, "index");
        const cJSON *cb = cJSON_GetObjectItemCaseSensitive(j, "content_block");
        if (cJSON_IsNumber(idx) && cJSON_IsObject(cb)) {
            ablock *b = block_at(a, (int)idx->valuedouble);
            if (b) {
                const cJSON *bt = cJSON_GetObjectItemCaseSensitive(cb, "type");
                if (cJSON_IsString(bt)) snprintf(b->type, sizeof b->type, "%s", bt->valuestring);
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(cb, "id");
                const cJSON *nm = cJSON_GetObjectItemCaseSensitive(cb, "name");
                if (cJSON_IsString(id)) snprintf(b->id, sizeof b->id, "%s", id->valuestring);
                if (cJSON_IsString(nm)) snprintf(b->name, sizeof b->name, "%s", nm->valuestring);
            }
        }
    } else if (!strcmp(type, "content_block_delta")) {
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(j, "index");
        const cJSON *dl = cJSON_GetObjectItemCaseSensitive(j, "delta");
        if (cJSON_IsNumber(idx) && cJSON_IsObject(dl)) {
            int i = (int)idx->valuedouble;
            const cJSON *dt = cJSON_GetObjectItemCaseSensitive(dl, "type");
            const char *dtype = cJSON_IsString(dt) ? dt->valuestring : "";
            if (!strcmp(dtype, "text_delta")) {
                const cJSON *tx = cJSON_GetObjectItemCaseSensitive(dl, "text");
                if (cJSON_IsString(tx)) {
                    abuf_add(&a->text, tx->valuestring, strlen(tx->valuestring));
                    if (a->cb) a->cb(NULL, tx->valuestring, a->ud);
                }
            } else if (!strcmp(dtype, "thinking_delta")) {
                const cJSON *th = cJSON_GetObjectItemCaseSensitive(dl, "thinking");
                if (cJSON_IsString(th)) {
                    abuf_add(&a->thinking, th->valuestring, strlen(th->valuestring));
                    if (a->cb) a->cb(th->valuestring, NULL, a->ud);
                }
            } else if (!strcmp(dtype, "input_json_delta")) {
                const cJSON *pj = cJSON_GetObjectItemCaseSensitive(dl, "partial_json");
                ablock *b = block_at(a, i);
                if (b && cJSON_IsString(pj)) abuf_add(&b->json, pj->valuestring, strlen(pj->valuestring));
            }
        }
    } else if (!strcmp(type, "message_start")) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(j, "message");
        const cJSON *use = msg ? cJSON_GetObjectItemCaseSensitive(msg, "usage") : NULL;
        const cJSON *pi = use ? cJSON_GetObjectItemCaseSensitive(use, "input_tokens") : NULL;
        if (cJSON_IsNumber(pi)) a->in_tok = (long)pi->valuedouble;
    } else if (!strcmp(type, "message_delta")) {
        const cJSON *dl = cJSON_GetObjectItemCaseSensitive(j, "delta");
        const cJSON *sr = dl ? cJSON_GetObjectItemCaseSensitive(dl, "stop_reason") : NULL;
        if (cJSON_IsString(sr)) snprintf(a->stop, sizeof a->stop, "%s", sr->valuestring);
        const cJSON *use = cJSON_GetObjectItemCaseSensitive(j, "usage");
        const cJSON *co = use ? cJSON_GetObjectItemCaseSensitive(use, "output_tokens") : NULL;
        if (cJSON_IsNumber(co)) a->out_tok = (long)co->valuedouble;
    } else if (!strcmp(type, "error")) {
        const cJSON *er = cJSON_GetObjectItemCaseSensitive(j, "error");
        const cJSON *msg = er ? cJSON_GetObjectItemCaseSensitive(er, "message") : NULL;
        log_error("anthropic: %s", cJSON_IsString(msg) ? msg->valuestring : "upstream error");
    }
    cJSON_Delete(j);
}

static mp_reply *anthropic_finish(actx *a) {
    mp_reply *r = calloc(1, sizeof *r);
    r->text = a->text.buf ? a->text.buf : strdup("");
    cJSON *tcarr = NULL;
    for (int i = 0; i < a->nblk; i++) {
        ablock *b = &a->blk[i];
        if (!b->present || strcmp(b->type, "tool_use")) continue;
        if (!tcarr) tcarr = cJSON_CreateArray();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", b->id[0] ? b->id : "call_x");
        cJSON_AddStringToObject(o, "name", b->name);
        cJSON *args = b->json.buf ? cJSON_Parse(b->json.buf) : NULL;
        cJSON_AddItemToObject(o, "arguments", args ? args : cJSON_CreateObject());
        cJSON_AddItemToArray(tcarr, o);
    }
    r->tool_calls = tcarr;
    if (!strcmp(a->stop, "tool_use") || tcarr) snprintf(r->stop_reason, sizeof r->stop_reason, "tool_calls");
    else if (!strcmp(a->stop, "max_tokens")) snprintf(r->stop_reason, sizeof r->stop_reason, "length");
    else if (a->stop[0]) snprintf(r->stop_reason, sizeof r->stop_reason, "stop");
    r->input_tokens = a->in_tok; r->output_tokens = a->out_tok;
    abuf_free(&a->thinking);
    for (int i = 0; i < a->nblk; i++) abuf_free(&a->blk[i].json);
    return r;
}

static cJSON *tools_to_anthropic(const cJSON *schemas) {
    if (!cJSON_IsArray(schemas) || !schemas->child) return NULL;
    cJSON *arr = cJSON_CreateArray();
    const cJSON *s = NULL;
    cJSON_ArrayForEach(s, schemas) {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(s, "name");
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(s, "schema");
        if (!cJSON_IsString(nm)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", nm->valuestring);
        const cJSON *desc = sc ? cJSON_GetObjectItemCaseSensitive(sc, "description") : NULL;
        if (cJSON_IsString(desc)) cJSON_AddStringToObject(o, "description", desc->valuestring);
        cJSON_AddItemToObject(o, "input_schema", sc ? cJSON_Duplicate(sc, 1) : cJSON_CreateObject());
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static char *build_messages_body(const char *model, const mp_ctx *ctx, int stream,
                                 long max_tokens, double temperature, double top_p,
                                 const cJSON *tools) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", (model && *model) ? model : "claude-sonnet-4-5");
    cJSON_AddNumberToObject(body, "max_tokens", max_tokens > 0 ? max_tokens : 16384);
    cJSON_AddNumberToObject(body, "temperature", temperature);
    cJSON_AddNumberToObject(body, "top_p", top_p > 0 ? top_p : 0.95);
    cJSON_AddBoolToObject(body, "stream", stream ? 1 : 0);

    /* system messages are a top-level string for Anthropic */
    abuf sys = {0};
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; ctx && i < ctx->n; i++) {
        const char *role = ctx->v[i].role ? ctx->v[i].role : "user";
        const char *content = ctx->v[i].content ? ctx->v[i].content : "";
        if (!strcmp(role, "system")) {
            if (sys.len) abuf_add(&sys, "\n\n", 2);
            abuf_add(&sys, content, strlen(content));
            continue;
        }
        if (!strcmp(role, "tool")) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "user");
            cJSON *blocks = cJSON_CreateArray();
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "tool_result");
            const cJSON *idj = ctx->v[i].tool_call ?
                cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "id") : NULL;
            cJSON_AddStringToObject(b, "tool_use_id", cJSON_IsString(idj) ? idj->valuestring : "call_0");
            cJSON_AddStringToObject(b, "content", content);
            cJSON_AddItemToArray(blocks, b);
            cJSON_AddItemToObject(m, "content", blocks);
            cJSON_AddItemToArray(msgs, m);
            continue;
        }
        if (!strcmp(role, "assistant") && ctx->v[i].tool_call) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "assistant");
            cJSON *blocks = cJSON_CreateArray();
            if (*content) {
                cJSON *tb = cJSON_CreateObject();
                cJSON_AddStringToObject(tb, "type", "text");
                cJSON_AddStringToObject(tb, "text", content);
                cJSON_AddItemToArray(blocks, tb);
            }
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "tool_use");
            const cJSON *idj = cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "id");
            const cJSON *nmj = cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "name");
            const cJSON *arj = cJSON_GetObjectItemCaseSensitive(ctx->v[i].tool_call, "arguments");
            cJSON_AddStringToObject(b, "id", cJSON_IsString(idj) ? idj->valuestring : "call_0");
            cJSON_AddStringToObject(b, "name", cJSON_IsString(nmj) ? nmj->valuestring : "");
            cJSON_AddItemToObject(b, "input", arj ? cJSON_Duplicate(arj, 1) : cJSON_CreateObject());
            cJSON_AddItemToArray(blocks, b);
            cJSON_AddItemToObject(m, "content", blocks);
            cJSON_AddItemToArray(msgs, m);
            continue;
        }
        if (!*content) continue; /* Anthropic rejects empty text blocks */
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", !strcmp(role, "assistant") ? "assistant" : "user");
        cJSON_AddStringToObject(m, "content", content);
        cJSON_AddItemToArray(msgs, m);
    }
    if (sys.len) cJSON_AddStringToObject(body, "system", sys.buf);
    abuf_free(&sys);
    cJSON_AddItemToObject(body, "messages", msgs);
    cJSON *t = tools_to_anthropic(tools);
    if (t) cJSON_AddItemToObject(body, "tools", t);
    char *txt = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return txt;
}

mp_reply *mp_anthropic_stream_ex(const char *model, const mp_ctx *ctx,
                                 int enable_thinking, long max_tokens, double temperature,
                                 double top_p, mp_stream_fn on_delta, void *ud,
                                 const cJSON *tools, const char *key_env,
                                 const char *broker_host, const char *upstream_path) {
    (void)enable_thinking; /* extended thinking requires model-specific opt-in */
    char *txt = build_messages_body(model, ctx, on_delta != NULL, max_tokens,
                                    temperature, top_p, tools);
    if (!txt) return NULL;
    const char *host = (broker_host && *broker_host) ? broker_host : "api.anthropic.com";
    char upfx[300]; snprintf(upfx, sizeof upfx, "%s", (upstream_path && *upstream_path) ? upstream_path : "/v1");
    size_t pl = strlen(upfx);
    while (pl > 1 && upfx[pl - 1] == '/') upfx[--pl] = 0;
    char path[600]; snprintf(path, sizeof path, "/%s%s/messages", host, upfx);

    char *resp = NULL; size_t rlen = 0;
    int status = mp_broker_request("POST", path, txt, strlen(txt),
                                   "anthropic", key_env && *key_env ? key_env : "",
                                   &resp, &rlen);
    free(txt);
    if (status < 0) { log_error("anthropic: transport failed (start ./build/minipi-broker)"); free(resp); return NULL; }
    if (status != 200) { log_error("anthropic: HTTP %d: %.512s", status, resp ? resp : ""); free(resp); return NULL; }

    if (!on_delta) {
        cJSON *j = cJSON_Parse(resp);
        mp_reply *r = j ? mp_anthropic_parse_response(j) : NULL;
        if (j) cJSON_Delete(j);
        if (!r) log_error("anthropic: could not parse response");
        free(resp);
        return r;
    }
    actx a; memset(&a, 0, sizeof a);
    a.cb = on_delta; a.ud = ud;
    mp_sse *sse = mp_sse_new(on_anthropic_event, &a);
    if (!sse) { free(resp); return NULL; }
    mp_sse_feed(sse, resp, rlen);
    mp_sse_flush(sse);
    mp_sse_free(sse);
    free(resp);
    return anthropic_finish(&a);
}
