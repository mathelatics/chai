#include "providers/provider.h"
#include <stdlib.h>
#include <string.h>
/* Anthropic messages API wire-format helpers (pure JSON; transport in M2). */
cJSON *mp_anthropic_build_request(const mp_ctx *ctx, const char *model, int stream) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model ? model : "claude-sonnet-4-5");
    cJSON_AddNumberToObject(body, "max_tokens", 8192);
    cJSON_AddBoolToObject(body, "stream", stream);
    /* system prompt goes first (extract from ctx) */
    cJSON *msgs = cJSON_CreateArray();
    for (int i = 0; i < ctx->n; i++) {
        if (!strcmp(ctx->v[i].role, "system")) continue;
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", !strcmp(ctx->v[i].role, "assistant") ? "assistant" : "user");
        cJSON_AddStringToObject(m, "content", ctx->v[i].content);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON_AddItemToObject(body, "messages", msgs);
    return body;
}
mp_reply *mp_anthropic_parse_response(const cJSON *resp) {
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
    if (!cJSON_IsArray(content)) return NULL;
    mp_reply *r = calloc(1, sizeof *r);
    size_t cap = 1024, len = 0; r->text = malloc(cap); r->text[0] = 0;
    const cJSON *blk = NULL;
    cJSON_ArrayForEach(blk, content) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(blk, "type");
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(blk, "text");
        if (cJSON_IsString(type) && !strcmp(type->valuestring, "text") && cJSON_IsString(tx)) {
            size_t need = len + strlen(tx->valuestring) + 1;
            if (need > cap) { cap = need * 2; r->text = realloc(r->text, cap); }
            strcpy(r->text + len, tx->valuestring); len += strlen(tx->valuestring);
        }
    }
    return r;
}
