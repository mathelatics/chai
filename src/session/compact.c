#include "compact.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int mp_compact_if_needed(mp_ctx *ctx, long budget) {
    if (!ctx || ctx->est_tokens <= budget) return 0;
    /* keep system + newest half; drop middle, insert summary placeholder */
    int keep_from = ctx->n / 2;
    if (keep_from <= 1) return 0;
    long dropped = 0; size_t bytes = 0;
    for (int i = 1; i < keep_from; i++) { dropped += ctx->v[i].content ? strlen(ctx->v[i].content)/4 : 0;
                                          bytes += strlen(ctx->v[i].content) + 1; }
    char *sum = malloc(bytes + 64);
    snprintf(sum, bytes + 64, "[compacted %d older messages, ~%ld tokens elided]", keep_from - 1, dropped);
    memmove(ctx->v + 2, ctx->v + keep_from, (size_t)(ctx->n - keep_from) * sizeof *ctx->v);
    ctx->n -= keep_from - 2;
    free(ctx->v[1].content); ctx->v[1].content = sum; free(ctx->v[1].tool_call); ctx->v[1].tool_call = NULL;
    ctx->est_tokens -= dropped;
    return 1;
}
