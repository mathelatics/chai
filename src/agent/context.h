#ifndef MINIPI_AGENT_CONTEXT_H
#define MINIPI_AGENT_CONTEXT_H
/* Conversation context: ordered messages (role/content/tool_call), token budget,
 * serialization to provider wire formats happens in src/providers/. */
#include "cJSON.h"
typedef struct { char *role; char *content; cJSON *tool_call; /* id,name,args */ } mp_msg;
typedef struct { mp_msg *v; int n, cap; long est_tokens; } mp_ctx;
void mp_ctx_init(mp_ctx *c);
void mp_ctx_free(mp_ctx *c);
void mp_ctx_push(mp_ctx *c, const char *role, const char *content, cJSON *tool_call /*takes ownership*/);
cJSON *mp_ctx_to_json(const mp_ctx *c);          /* cJSON array; caller deletes */
int   mp_ctx_from_json(mp_ctx *c, const cJSON *arr); /* for session reload */
#endif
