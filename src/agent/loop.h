#ifndef MINIPI_AGENT_LOOP_H
#define MINIPI_AGENT_LOOP_H
/* The agent turn loop: user msg -> provider stream -> deltas -> tool calls ->
 * execute (via tools/registry) -> feed results -> repeat until stop. */
#include "agent/context.h"
#include "agent/events.h"
typedef struct mp_agent mp_agent;
typedef struct { const char *model; int max_turns; long token_budget; } mp_agent_cfg;
mp_agent *mp_agent_new(const mp_agent_cfg *cfg, mp_bus *bus);
void      mp_agent_free(mp_agent *a);
/* Runs one user turn to completion (blocking w.r.t. the loop; async under libuv).
 * Returns 0 ok, nonzero error code. */
int  mp_agent_run_turn(mp_agent *a, const char *user_text);
mp_ctx *mp_agent_ctx(mp_agent *a);
struct mp_toolreg; struct mp_toolreg *mp_agent_tools(mp_agent *a);
/* Interactive REPL against the streaming provider (broker-backed). */
int mp_repl_run(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *session_dir);
struct mp_bus; struct mp_bus *mp_agent_bus(mp_agent *a);
#endif
