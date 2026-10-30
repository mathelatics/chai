#ifndef MINIPI_SESSION_TREE_H
#define MINIPI_SESSION_TREE_H
/* Parent-linked session tree enabling branching/forking (pi-style):
 * every entry has an id and optional parent id; current leaf pointer moves. */
#include "khash.h"
KHASH_MAP_INIT_STR(sess_idx, int)
typedef struct { char *id, *parent, *role, *content; } mp_entry;
typedef struct { mp_entry *v; int n, cap; khash_t(sess_idx) *idx; int leaf; } mp_tree;
void mp_tree_init(mp_tree *t);
int  mp_tree_add(mp_tree *t, const char *id, const char *parent, const char *role, const char *content);
const mp_entry *mp_tree_get(mp_tree *t, const char *id);
/* Walk from leaf to root; writes pointers (root-first) up to max; returns count. */
int  mp_tree_path(mp_tree *t, const mp_entry **out, int max);
void mp_tree_free(mp_tree *t);
#endif
