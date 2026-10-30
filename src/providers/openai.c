#include "providers/provider.h"
/* OpenAI chat-completions wire format helpers (request build + response parse).
 * Transport arrives in M2 (net/http + SSE). Kept pure-JSON here for unit tests. */
cJSON *mp_openai_build_request(const mp_ctx *ctx, const char *model, int stream) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model ? model : "gpt-4o-mini");
    cJSON_AddBoolToObject(body, "stream", stream);
    cJSON_AddItemToObject(body, "messages", mp_ctx_to_json(ctx));
    return body;
}
/* Parses a non-streamed chat.completions response into an mp_reply. */
mp_reply *mp_openai_parse_response(const cJSON *resp) {
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(resp, "choices");
    if (!cJSON_IsArray(ch) || !cJSON_GetArraySize(ch)) return NULL;
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(ch->child, "message");
    const cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    mp_reply *r = calloc(1, sizeof *r);
    r->text = strdup(cJSON_IsString(content) ? content->valuestring : "");
    const cJSON *tcs = msg ? cJSON_GetObjectItemCaseSensitive(msg, "tool_calls") : NULL;
    if (cJSON_IsArray(tcs)) r->tool_calls = cJSON_Duplicate(tcs, 1);
    const cJSON *use = cJSON_GetObjectItemCaseSensitive(resp, "usage");
    if (cJSON_IsObject(use)) {
        const cJSON *pi = cJSON_GetObjectItemCaseSensitive(use, "prompt_tokens");
        const cJSON *co = cJSON_GetObjectItemCaseSensitive(use, "completion_tokens");
        if (cJSON_IsNumber(pi)) r->input_tokens = (long)pi->valuedouble;
        if (cJSON_IsNumber(co)) r->output_tokens = (long)co->valuedouble;
    }
    return r;
}
