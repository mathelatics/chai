#ifndef MINIPI_UTIL_DIFF_H
#define MINIPI_UTIL_DIFF_H
/* Line diff (LCS-based; Myers-style output). Used by the `edit` tool preview. */
typedef enum { MP_DIFF_CTX, MP_DIFF_ADD, MP_DIFF_DEL } mp_diff_op;
typedef struct { mp_diff_op op; int old_line; int new_line; const char *text; } mp_diff_hunk;
/* Computes a unified-ish diff into caller-provided array. Returns hunk count
 * or -1 if exceeds max_hunks / OOM. Lines are arrays of C strings. */
int mp_diff_lines(const char **a, int na, const char **b, int nb,
                  mp_diff_hunk *out, int max_hunks);
#endif
