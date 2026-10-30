#ifndef MINIPI_SESSION_STORE_H
#define MINIPI_SESSION_STORE_H
/* JSONL append-only session store. One file per session under
 * $MINIPI_HOME/sessions/ (default ~/.minipi/sessions). Each line:
 *   {"id":uuid,"parent":uuid|null,"ts":unix,"role":...,"content":...}
 * Checkpoint lines embed sha256 of the chain for integrity. */
#include "agent/context.h"
typedef struct mp_store mp_store;
mp_store *mp_store_open(const char *session_dir, const char *session_id);
void      mp_store_close(mp_store *s);
int       mp_store_append(mp_store *s, const char *role, const char *content,
                          const char *tool_call_json /*or NULL*/, const char *parent_id);
/* Replay a whole file into ctx; returns message count or -1. */
int       mp_store_replay(const char *path, mp_ctx *out_ctx);
/* sha256 over all lines so far (checkpoint hash). out_hex[65]. */
void      mp_store_checkpoint(mp_store *s, char out_hex[65]);
#endif
