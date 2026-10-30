#ifndef MINIPI_SESSION_COMPACT_H
#define MINIPI_SESSION_COMPACT_H
/* Context compaction: when est_tokens exceeds budget, summarize the oldest
 * messages with the model (M3) and replace them with one summary message.
 * v0: naive truncation marker so budgets are respected even offline. */
#include "agent/context.h"
int mp_compact_if_needed(mp_ctx *ctx, long token_budget);
#endif
