/* find tool (pi coding-agent parity): recursive glob/substring search confined
 * to the workspace. Matches case-insensitive substring OR simple glob (* ? [])
 * against the basename. Skips .git/node_modules/__pycache__, never follows
 * symlinks for directory descent; bounded depth and result count so it always
 * terminates on huge trees. Read-only. */
#include "tools/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define MP_FIND_MAX_RESULTS 512
#define MP_FIND_MAX_DEPTH   12

const char *MP_SCHEMA_FIND =
 "{\"type\":\"object\",\"required\":[\"pattern\"],\"additionalProperties\":false,"
 "\"properties\":{\"pattern\":{\"type\":\"string\"},"
 "\"root\":{\"type\":\"string\"},"
 "\"max_depth\":{\"type\":\"integer\"}}}";

/* shell-style glob on lowercased strings: *, ?, [abc]/[a-z] classes */
static int globmatch(const char *pat, const char *s) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (;; s++) { if (globmatch(pat, s)) return 1; if (!*s) break; }
            return 0;
        }
        if (*pat == '?') { if (!*s) return 0; pat++; s++; continue; }
        if (*pat == '[') {
            const char *q = pat + 1; int neg = (*q == '!'), hit = 0;
            if (neg) q++;
            if (!*q) return 0;                 /* '!' alone is a class member */
            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if (*s >= q[0] && *s <= q[2]) hit = 1;
                    q += 3;
                } else { if (*s == *q) hit = 1; q++; }
            }
            if (!*q) return 0;                 /* unterminated class => no match */
            if (hit == neg) return 0;
            pat = q + 1; s++;
            continue;
        }
        if (*pat != *s) return 0;
        pat++; s++;
    }
    return !*s;
}

static int skipdir(const char *name) {
    static const char *SKIP[] = {".git", "node_modules", "__pycache__", NULL};
    for (int i = 0; SKIP[i]; i++) if (!strcmp(name, SKIP[i])) return 1;
    return 0;
}

typedef struct { char **v; int n, cap; } coll;

static void walk(coll *c, const char *dir, const char *pat, int depth, int max_depth) {
    if (depth > max_depth || c->n >= MP_FIND_MAX_RESULTS) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4096];
        if (snprintf(full, sizeof full, "%s/%s", dir, de->d_name) >= (int)sizeof full) continue;
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!skipdir(de->d_name)) walk(c, full, pat, depth + 1, max_depth);
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            char low[512]; size_t j = 0;
            for (; de->d_name[j] && j + 1 < sizeof low; j++)
                low[j] = (char)tolower((unsigned char)de->d_name[j]);
            low[j] = 0;
            if (globmatch(pat, low) || strstr(low, pat)) {
                if (c->n < MP_FIND_MAX_RESULTS) {
                    if (c->n == c->cap) {
                        int nc = c->cap ? c->cap * 2 : 64;
                        char **nv = realloc(c->v, (size_t)nc * sizeof *nv);
                        if (!nv) break;
                        c->v = nv; c->cap = nc;
                    }
                    c->v[c->n] = strdup(full);
                    if (c->v[c->n]) c->n++;
                }
            }
        }
    }
    closedir(d);
}

char *mp_tool_find(const cJSON *args, void *ud) {
    const char *root = ud;
    const cJSON *pj = cJSON_GetObjectItemCaseSensitive(args, "pattern");
    if (!cJSON_IsString(pj) || !pj->valuestring[0])
        return strdup("error: pattern must be a non-empty string");
    const cJSON *rj = cJSON_GetObjectItemCaseSensitive(args, "root");
    const cJSON *dj = cJSON_GetObjectItemCaseSensitive(args, "max_depth");
    const char *subroot = (cJSON_IsString(rj) && rj->valuestring[0]) ? rj->valuestring : ".";
    int max_depth = cJSON_IsNumber(dj) ? (int)dj->valuedouble : MP_FIND_MAX_DEPTH;
    if (max_depth < 0) max_depth = 0;
    if (max_depth > MP_FIND_MAX_DEPTH) max_depth = MP_FIND_MAX_DEPTH;
    char *canon = mp_path_confine(root, subroot);
    if (!canon) return strdup("error: root escapes workspace or does not exist");
    char pat[512]; size_t i = 0;
    for (; pj->valuestring[i] && i + 1 < sizeof pat; i++)
        pat[i] = (char)tolower((unsigned char)pj->valuestring[i]);
    pat[i] = 0;
    coll c = { NULL, 0, 0 };
    walk(&c, canon, pat, 0, max_depth);
    free(canon);
    size_t need = 64;
    for (int k = 0; k < c.n; k++) need += strlen(c.v[k]) + 2;
    char *out = malloc(need);
    if (!out) { for (int k = 0; k < c.n; k++) free(c.v[k]); free(c.v); return strdup("error: out of memory"); }
    size_t off = (size_t)snprintf(out, need, "%d matches%s\n", c.n,
                                  c.n >= MP_FIND_MAX_RESULTS ? " (truncated)" : "");
    for (int k = 0; k < c.n; k++) {
        off += (size_t)snprintf(out + off, need - off, "%s\n", c.v[k]);
        free(c.v[k]);
    }
    free(c.v);
    return out;
}
