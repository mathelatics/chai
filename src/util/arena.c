#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct blk { struct blk *next; } blk;
#define ARENA_ALIGN 16
static void *blk_new(mp_arena *a, size_t need) {
    size_t sz = need + sizeof(blk);
    if (sz < 4096) sz = 4096;
    blk *b = malloc(sz);
    if (!b) return NULL;
    b->next = a->block_head;
    a->block_head = b;
    a->cur = (unsigned char *)b + sizeof(blk);
    a->end = (unsigned char *)b + sz;
    return a->cur;
}
void mp_arena_init(mp_arena *a, size_t initial_bytes) {
    a->cur = a->end = NULL; a->block_head = NULL;
    if (initial_bytes) blk_new(a, initial_bytes);
}
void *mp_arena_alloc(mp_arena *a, size_t n) {
    if (n == 0) n = 1;
    n = (n + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    if ((size_t)(a->end - a->cur) >= n) { unsigned char *p = a->cur; a->cur += n; return p; }
    if (!blk_new(a, n)) return NULL;
    unsigned char *p = a->cur; a->cur += n; return p;
}
char *mp_arena_strndup(mp_arena *a, const char *s, size_t n) {
    char *p = mp_arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n); p[n] = '\0';
    return p;
}
char *mp_arena_strdup(mp_arena *a, const char *s) { return mp_arena_strndup(a, s, strlen(s)); }
void mp_arena_reset(mp_arena *a) {
    for (blk *b = a->block_head; b && b->next; ) { blk *nx = b->next; free(b); a->block_head = nx; }
    if (a->block_head) { a->cur = (unsigned char *)a->block_head + sizeof(blk);
                         a->end = (unsigned char *)a->block_head + 4096; }
}
void mp_arena_destroy(mp_arena *a) {
    for (blk *b = a->block_head; b; ) { blk *nx = b->next; free(b); b = nx; }
    a->block_head = a->cur = a->end = NULL;
}
