#ifndef MINIPI_AGENT_LOOP_H
#define MINIPI_AGENT_LOOP_H
/* The agent turn loop: user msg -> provider stream -> deltas -> tool calls ->
 * POLICY GATE (nothing executes without a verdict of ALLOW) -> execute via the
 * tool registry -> feed results back -> repeat until stop. */
#include "agent/context.h"
#include "agent/events.h"
typedef struct mp_agent mp_agent;
typedef struct mp_toolreg mp_toolreg;
struct mp_policy;
typedef struct { const char *model; int max_turns; long token_budget; } mp_agent_cfg;

mp_agent *mp_agent_new(const mp_agent_cfg *cfg, mp_bus *bus);
void      mp_agent_free(mp_agent *a);
/* Runs one user turn to completion (blocking w.r.t. the loop). Returns 0 ok. */
int  mp_agent_run_turn(mp_agent *a, const char *user_text);
/* Policy-gated variant used by --prompt one-shot mode: every tool call gets a
 * verdict from `pol` before execution; DENY feeds a denial notice back to the
 * model instead of running anything. */
int  mp_agent_run_turn_pol(mp_agent *a, const char *user_text, struct mp_policy *pol,
                           mp_toolreg *tools);
mp_ctx *mp_agent_ctx(mp_agent *a);
mp_toolreg *mp_agent_tools(mp_agent *a);
struct mp_bus *mp_agent_bus(mp_agent *a);
/* live-route accessors (used by the REPL streaming path) */
const char *mp_agent_model(mp_agent *a);
int  mp_agent_max_turns(mp_agent *a);
long mp_agent_token_budget(mp_agent *a);
/* Interactive REPL against the streaming provider (broker or local inference).
 * sessdir NULL => $MINIPI_HOME/.minipi/sessions with loud diagnostics. */
int mp_repl_run(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *sessdir);
#endif
