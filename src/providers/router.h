#ifndef MINIPI_PROVIDERS_ROUTER_H
#define MINIPI_PROVIDERS_ROUTER_H
/* Single routing point from a configured provider to the right wire format and
 * transport:
 *   openai / nemotron  -> OpenAI-compatible chat/completions (cloud via broker)
 *   anthropic          -> native Anthropic messages API (cloud via broker)
 *   openai-local       -> direct loopback HTTP (LM Studio/Ollama/llama.cpp/vLLM)
 * Also owns live model discovery for the TUI pickers. */
#include "config/config.h"
#include "providers/provider.h"

/* Streamed completion. on_delta may be NULL (one-shot: returns full text).
 * tools is the registry schema array [{name,schema}] or NULL. */
mp_reply *mp_provider_stream(const mp_provider *prov, const char *model,
                             const mp_ctx *ctx, int enable_thinking, long max_tokens,
                             mp_stream_fn on_delta, void *ud, const cJSON *tools);

typedef struct { char **v; int n, cap; } mp_model_list;
void mp_model_list_free(mp_model_list *l);
/* Fetch the provider's live model ids into l (malloc'd strings, sorted as
 * returned upstream). Returns 0 on success, <0 on transport/parse error. */
int mp_provider_list_models(const mp_provider *prov, mp_model_list *l);

/* Parse a provider base_url into broker host + path prefix, e.g.
 * "https://api.openai.com/v1" -> host "api.openai.com", path "/v1".
 * Returns 0 on success. */
int mp_url_split_base(const char *url, char *host, size_t hn, char *path, size_t pn);
#endif
