#ifndef MINIPI_PROVIDERS_PROVIDER_H
#define MINIPI_PROVIDERS_PROVIDER_H
/* Provider abstraction: mp_ctx -> wire request -> HTTP(S) -> streamed reply.
 * M1: offline stub (deterministic echo) so the loop/TUI/session can be built
 * before HTTPS is wired. M2: openai.c/anthropic.c over net/http + net/sse. */
#include "agent/context.h"
#include "cJSON.h"
typedef struct { char *text; cJSON *tool_calls /* array of {id,name,arguments} or NULL */;
                 long input_tokens, output_tokens; } mp_reply;
mp_reply *mp_provider_complete(const char *model, const mp_ctx *ctx);
void      mp_reply_free(mp_reply *r);
/* provider registry (khash in later milestones) */
int mp_provider_set_base_url(const char *url);   /* dev mode: http:// localhost mock */
/* NVIDIA Nemotron streaming variant: parses SSE chunks incl. delta.reasoning_content.
 * Returns reply->text = content, reply->tool_calls may hold parsed tool calls. */
typedef void (*mp_stream_fn)(const char *reasoning_delta, const char *content_delta, void *ud);
mp_reply *mp_nemotron_stream(const char *model, const mp_ctx *ctx,
                             int enable_thinking, long max_tokens, double temperature,
                             double top_p, mp_stream_fn on_delta, void *ud);
#endif
