#ifndef MINIPI_SESSION_STORE_H
#define MINIPI_SESSION_STORE_H
/* JSONL append-only session store. One file per session under
 * $MINIPI_HOME/sessions/ (default ~/.minipi/sessions). Each line:
 *   {"id":uuid,"parent":...,"role":...,"content":...,"tool_calls"?:...,"meta"?:...,"hash":...}
 * where hash = sha256(prev_hash || id || role || content) — a tamper-evident
 * chain. mp_store_checkpoint() fsyncs; mp_store_verify() recomputes the whole
 * chain and reports the first broken line (round-trip integrity check). */
#include <stddef.h>
#include "agent/context.h"

typedef struct mp_store mp_store;

/* sessdir NULL => "<mp_minipi_dir()>/sessions" with loud errno logging on any
 * mkdir/open failure (silent persistence loss was a real bug; never again).
 * Store failure is non-fatal by design: returns NULL, session runs in memory. */
mp_store *mp_store_open(const char *sessdir, const char *session_id);
void      mp_store_close(mp_store *s);
/* tool_call_json / meta_json: raw JSON text or NULL. Returns 0 ok, -1 failed. */
int       mp_store_append(mp_store *s, const char *role, const char *content,
                          const char *tool_call_json, const char *meta_json);
/* Flush + fsync to durable storage. 0 ok, -1 error (logged). */
int       mp_store_checkpoint(mp_store *s);
const char *mp_store_path(const mp_store *s);
int       mp_store_count(const mp_store *s);
/* Replay a stored file into ctx (resume); returns message count or -1. */
int       mp_store_replay(const char *path, mp_ctx *out_ctx);
/* Recompute the sha256 chain over `path`; 0 = intact, -1 = broken/tampered
 * (first bad line number logged), -2 = cannot open. */
int       mp_store_verify(const char *path);
#endif
