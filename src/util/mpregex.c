/* util/mpregex.c — backtracking regex over a compiled node list. See header. */
#include "util/mpregex.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- AST ---------------------------------------------------- */
enum { N_CHAR, N_ANY, N_CLASS, N_SEQ, N_ALT, N_REP, N_BOL, N_EOL, N_EMPTY };
typedef struct Node {
    int type;
    /* N_CHAR */ unsigned char ch;
    /* N_CLASS */ unsigned char *set; size_t setlen; int neg;
    /* children */ struct Node *a, *b;
    /* N_REP bounds */ int lo, hi;   /* hi == -1 => unbounded */
} Node;

struct mp_regex { Node *root; int icase; };

static void node_free(Node *n) {
    if (!n) return;
    node_free(n->a); node_free(n->b);
    free(n->set);
    free(n);
}

/* ---------------- parser --------------------------------------------------- */
typedef struct { const char *p; int ok; } P;

static Node *parse_alt(P *ps);

static int fold(int c, int icase) { return icase ? tolower(c) : c; }

static Node *mk(int t) { Node *n = calloc(1, sizeof *n); if (n) n->type = t; return n; }

static Node *parse_class(P *ps) {
    Node *n = mk(N_CLASS);
    if (!n) { ps->ok = 0; return NULL; }
    size_t cap = 64, len = 0;
    n->set = malloc(cap);
    if (!n->set) { ps->ok = 0; node_free(n); return NULL; }
    if (*ps->p == '^') { n->neg = 1; ps->p++; }
    int first = 1;
    while (*ps->p && (*ps->p != ']' || first)) {
        first = 0;
        unsigned char lo = (unsigned char)*ps->p;
        unsigned char hi;
        if (ps->p[1] == '-' && ps->p[2] && ps->p[2] != ']') {
            hi = (unsigned char)ps->p[2];
            ps->p += 3;
        } else {
            hi = lo;
            ps->p += 1;
        }
        for (int c = lo; ; c++) {
            if (len + 2 > cap) {
                cap *= 2;
                unsigned char *ns = realloc(n->set, cap);
                if (!ns) { ps->ok = 0; node_free(n); return NULL; }
                n->set = ns;
            }
            n->set[len++] = (unsigned char)c;
            if (c == hi) break;
        }
    }
    if (*ps->p != ']') { ps->ok = 0; node_free(n); return NULL; }
    ps->p++;
    n->setlen = len;
    return n;
}

static Node *parse_atom(P *ps) {
    char c = *ps->p;
    switch (c) {
    case '(': {
        ps->p++;
        Node *inner = parse_alt(ps);
        if (!inner) return NULL;
        if (*ps->p != ')') { node_free(inner); ps->ok = 0; return NULL; }
        ps->p++;
        return inner;
    }
    case '[':
        ps->p++;
        return parse_class(ps);
    case '.':
        ps->p++; { Node *n = mk(N_ANY); if (!n) ps->ok = 0; return n; }
    case '^':
        ps->p++; { Node *n = mk(N_BOL); if (!n) ps->ok = 0; return n; }
    case '$':
        ps->p++; { Node *n = mk(N_EOL); if (!n) ps->ok = 0; return n; }
    case '\\': {
        if (!ps->p[1]) { ps->ok = 0; return NULL; }
        ps->p += 2;
        Node *n = mk(N_CHAR);
        if (!n) { ps->ok = 0; return NULL; }
        n->ch = (unsigned char)ps->p[-1];
        return n;
    }
    default:
        if (c == '\0' || c == '|' || c == ')' || c == '*' || c == '+' || c == '?' || c == '{') {
            ps->ok = 0; return NULL;
        }
        ps->p++;
        Node *n = mk(N_CHAR);
        if (!n) { ps->ok = 0; return NULL; }
        n->ch = (unsigned char)c;
        return n;
    }
}

static Node *append_seq(Node *seq, Node *item) {
    if (!seq) return item;
    if (!item) { node_free(seq); return NULL; }
    /* flatten: seq is left-leaning binary tree of N_SEQ */
    Node *s = mk(N_SEQ);
    if (!s) { node_free(seq); node_free(item); return NULL; }
    s->a = seq; s->b = item;
    return s;
}

static Node *parse_rep(P *ps) {
    Node *atom = parse_atom(ps);
    if (!atom) return NULL;
    int lo = 1, hi = 1;
    char c = *ps->p;
    if (c == '*')      { lo = 0; hi = -1; ps->p++; }
    else if (c == '+') { lo = 1; hi = -1; ps->p++; }
    else if (c == '?') { lo = 0; hi = 1;  ps->p++; }
    else if (c == '{') {
        char *end;
        long v = strtol(ps->p + 1, &end, 10);
        if (end == ps->p + 1 || v < 0 || v > 1000) { node_free(atom); ps->ok = 0; return NULL; }
        lo = (int)v;
        if (*end == ',') {
            if (end[1] == '}') { hi = -1; end += 2; }
            else {
                long w = strtol(end + 1, &end, 10);
                if (w < lo || w > 1000) { node_free(atom); ps->ok = 0; return NULL; }
                hi = (int)w;
            }
        } else hi = lo;
        if (*end != '}') { node_free(atom); ps->ok = 0; return NULL; }
        ps->p = end + 1;
    }
    if (lo == 1 && hi == 1) return atom;
    /* consume another quantifier? treat "**" etc as error via next loop check */
    Node *rep = mk(N_REP);
    if (!rep) { node_free(atom); ps->ok = 0; return NULL; }
    rep->a = atom; rep->lo = lo; rep->hi = hi;
    return rep;
}

static Node *parse_concat(P *ps) {
    Node *seq = NULL;
    for (;;) {
        char c = *ps->p;
        if (c == '\0' || c == '|' || c == ')') return seq ? seq : mk(N_EMPTY);
        Node *item = parse_rep(ps);
        if (!item) { node_free(seq); return NULL; }
        Node *ns = append_seq(seq, item);
        if (!ns) { node_free(seq); node_free(item); ps->ok = 0; return NULL; }
        seq = ns;
    }
}

static Node *parse_alt(P *ps) {
    Node *left = parse_concat(ps);
    if (!left) return NULL;
    if (*ps->p != '|') return left;
    ps->p++;
    Node *right = parse_alt(ps);
    if (!right) { node_free(left); return NULL; }
    Node *alt = mk(N_ALT);
    if (!alt) { node_free(left); node_free(right); ps->ok = 0; return NULL; }
    alt->a = left; alt->b = right;
    return alt;
}

mp_regex *mp_regcomp(const char *pattern, int icase) {
    if (!pattern) return NULL;
    P ps = { pattern, 1 };
    Node *root = parse_alt(&ps);
    if (!root || !ps.ok || *ps.p != '\0') { node_free(root); return NULL; }
    mp_regex *re = calloc(1, sizeof *re);
    if (!re) { node_free(root); return NULL; }
    re->root = root; re->icase = icase;
    return re;
}

void mp_regfree(mp_regex *re) { if (re) { node_free(re->root); free(re); } }

/* ---------------- matcher --------------------------------------------------- */
/* Continuation-passing backtracking matcher — correct greedy semantics with
 * full backtracking across quantifier boundaries (e.g. "ab*c" vs "ac").
 * m_cont(re, node, text, len, i, cont, u): match `node` at i, then invoke
 * cont(u, matched_end). Returns 1 on overall success. */
typedef int (*ContFn)(void *u, size_t end);

static int cls_hit(const Node *n, int c) {
    for (size_t k = 0; k < n->setlen; k++)
        if (n->set[k] == (unsigned char)c) return 1;
    return 0;
}

static int m_cont(const mp_regex *re, const Node *n, const char *text, size_t len,
                  size_t i, ContFn cont, void *u);

static int seq_k(int idx, const Node *const *v, int n, const mp_regex *re,
                 const char *text, size_t len, size_t pos, ContFn cont, void *u);
typedef struct { int idx, n; const Node *const *v; const mp_regex *re;
                 const char *text; size_t len; ContFn cont; void *u; } SeqCtx;

static int step_cont(void *vp, size_t pos) {
    SeqCtx *c = vp;
    return seq_k(c->idx + 1, c->v, c->n, c->re, c->text, c->len, pos, c->cont, c->u);
}

static int seq_k(int idx, const Node *const *v, int n, const mp_regex *re,
                 const char *text, size_t len, size_t pos, ContFn cont, void *u) {
    if (idx == n) return cont(u, pos);
    SeqCtx ctx = { idx, n, v, re, text, len, cont, u };
    return m_cont(re, v[idx], text, len, pos, step_cont, &ctx);
}

typedef struct { ContFn outer; void *ou; } Tail;
static int tail_cont(void *tp, size_t end) { (void)end; Tail *t = tp; return t->outer(t->ou, end); }

typedef struct { const Node *rep; int cnt; size_t *pos; const mp_regex *re;
                 const char *text; size_t len; ContFn cont; void *u; } RepCtx;
static int rep_more(void *vp, size_t end);
static int rep_item_cont(void *vp, size_t end) {
    RepCtx *rc = vp;
    if (end == rc->pos[rc->cnt]) return 0;      /* zero-width: stop growing */
    rc->cnt++;
    rc->pos[rc->cnt] = end;
    return rep_more(vp, end);
}
static int rep_try(void *vp, size_t end) {
    RepCtx *rc = vp;
    if (rc->cnt < rc->rep->lo) return 0;
    return rc->cont(rc->u, end);
}
static int rep_more(void *vp, size_t end) {
    RepCtx *rc = vp;
    if (rc->rep->hi >= 0 && rc->cnt >= rc->rep->hi) return rep_try(vp, end);
    if (rc->cnt >= 512) return rep_try(vp, end);
    /* greedy: attempt one more item first; on failure fall back to try */
    if (m_cont(rc->re, rc->rep->a, rc->text, rc->len, end, rep_item_cont, vp)) return 1;
    return rep_try(vp, end);
}

static int m_cont(const mp_regex *re, const Node *n, const char *text, size_t len,
                  size_t i, ContFn cont, void *u) {
    if (!n) return cont(u, i);
    switch (n->type) {
    case N_EMPTY: return cont(u, i);
    case N_BOL:
        if (i != 0) return 0;
        return cont(u, i);
    case N_EOL:
        if (i != len) return 0;
        return cont(u, i);
    case N_ANY:
        if (i >= len) return 0;
        return cont(u, i + 1);
    case N_CHAR: {
        if (i >= len) return 0;
        int tc = (unsigned char)text[i], mc = n->ch;
        if (re->icase) { tc = tolower(tc); mc = tolower(mc); }
        if (tc != mc) return 0;
        return cont(u, i + 1);
    }
    case N_CLASS: {
        if (i >= len) return 0;
        int c = (unsigned char)text[i];
        int hit = cls_hit(n, c);
        if (!hit && re->icase) hit = cls_hit(n, tolower(c));
        if (hit == n->neg) return 0;
        return cont(u, i + 1);
    }
    case N_SEQ: {
        /* flatten left-leaning tree into a vector (max depth 63) */
        const Node *v[64]; int n_ = 0;
        const Node *cur = n;
        while (cur && cur->type == N_SEQ && n_ < 63) { v[n_++] = cur->b; cur = cur->a; }
        if (!cur || n_ >= 63) return 0;         /* malformed or too deep */
        v[n_++] = cur;
        /* reverse so order is left-to-right */
        for (int a = 0, b = n_ - 1; a < b; a++, b--) { const Node *t = v[a]; v[a] = v[b]; v[b] = t; }
        return seq_k(0, v, n_, re, text, len, i, cont, u);
    }
    case N_ALT:
        if (m_cont(re, n->a, text, len, i, cont, u)) return 1;
        return m_cont(re, n->b, text, len, i, cont, u);
    case N_REP: {
        size_t pos[513]; pos[0] = i;
        RepCtx rc = { n, 0, pos, re, text, len, cont, u };
        return rep_more(&rc, i);
    }
    }
    return 0;
}

static int final_cont(void *u, size_t end) { (void)end; (void)u; return 1; }

int mp_regexec(const mp_regex *re, const char *text) {
    if (!re || !text) return -1;
    size_t len = strlen(text);
    /* If pattern can match empty anywhere, trivially true; scan starts. */
    for (size_t i = 0; i <= len; i++) {
        if (m_cont(re, re->root, text, len, i, final_cont, NULL)) return 0;
        /* ^ anchor optimization: if root begins with BOL only start 0 matters */
        const Node *r = re->root;
        int bol_first = 0;
        while (r && r->type == N_SEQ) r = r->a;
        if (r && r->type == N_BOL) bol_first = 1;
        if (bol_first) break;
    }
    return 1;
}
