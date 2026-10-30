#ifndef MINIPI_UTIL_ARENA_H
#define MINIPI_UTIL_ARENA_H
/* Arena allocator: bump allocation, free-all reset. Never trust model lengths;
 * allocate through the arena with explicit caps (docs/ARCHITECTURE.md §8). */
#include <stddef.h>
typedef struct mp_arena { unsigned char *cur, *end; void *block_head; } mp_arena;
void  mp_arena_init(mp_arena *a, size_t initial_bytes);
void *mp_arena_alloc(mp_arena *a, size_t n);          /* NULL on OOM */
char *mp_arena_strdup(mp_arena *a, const char *s);
char *mp_arena_strndup(mp_arena *a, const char *s, size_t n);
void  mp_arena_reset(mp_arena *a);                    /* keeps first block */
void  mp_arena_destroy(mp_arena *a);
#endif
