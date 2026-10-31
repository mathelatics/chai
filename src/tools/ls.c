/* ls tool (pi coding-agent parity): list directory entries, confined to the
 * workspace root via mp_path_confine. Output format: one entry per line,
 * "d|r|l name size" — deterministic ordering (dirs first, then lexical) so
 * model feedback is stable and diffable. Read-only: never mutates anything. */
#include "tools/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

const char *MP_SCHEMA_LS =
 "{\"type\":\"object\",\"required\":[\"path\"],\"additionalProperties\":false,"
 "\"properties\":{\"path\":{\"type\":\"string\"}}}";

typedef struct { char *name; int isdir; int islink; long size; } lent;
static int cmpent(const void *a, const void *b) {
    const lent *x = a, *y = b;
    if (x->isdir != y->isdir) return y->isdir - x->isdir;   /* dirs first */
    return strcmp(x->name, y->name);
}

char *mp_tool_ls(const cJSON *args, void *ud) {
    const char *root = ud;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!cJSON_IsString(p)) return strdup("error: path must be a string");
    char *canon = mp_path_confine(root, p->valuestring);
    if (!canon) return strdup("error: path escapes workspace or does not exist");
    DIR *d = opendir(canon);
    if (!d) {
        /* Not a directory: report it as a single file entry. */
        struct stat st;
        if (stat(canon, &st) == 0 && S_ISREG(st.st_mode)) {
            char *out = malloc(strlen(canon) + 64);
            if (out) sprintf(out, "%s (%lld bytes)", canon, (long long)st.st_size);
            free(canon);
            return out ? out : strdup("error: out of memory");
        }
        free(canon);
        return strdup("error: cannot open directory");
    }
    lent *v = NULL; int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n >= 2048) break;                       /* hard entry cap */
        char full[4096];
        if (snprintf(full, sizeof full, "%s/%s", canon, de->d_name) >= (int)sizeof full) continue;
        struct stat st; int isdir = 0, islink = 0; long sz = -1;
        if (lstat(full, &st) == 0) {
            islink = S_ISLNK(st.st_mode);
            isdir = S_ISDIR(st.st_mode);
            if (S_ISREG(st.st_mode)) sz = (long)st.st_size;
        }
        if (n == cap) { cap = cap ? cap * 2 : 64; v = realloc(v, (size_t)cap * sizeof *v); }
        v[n++] = (lent){strdup(de->d_name), isdir, islink, sz};
    }
    closedir(d);
    free(canon);
    qsort(v, (size_t)n, sizeof *v, cmpent);
    /* size output conservatively; registry caps anyway (C-2) but keep tight */
    size_t need = 32;
    for (int i = 0; i < n; i++) need += strlen(v[i].name) + 32;
    char *out = malloc(need);
    if (!out) { for (int i = 0; i < n; i++) free(v[i].name); free(v); return strdup("error: out of memory"); }
    size_t off = 0;
    off += (size_t)snprintf(out + off, need - off, "%d entries\n", n);
    for (int i = 0; i < n && off + 1 < need; i++) {
        off += (size_t)snprintf(out + off, need - off, "%c%c %-48s %ld\n",
                                v[i].isdir ? 'd' : '-', v[i].islink ? 'l' : 'r',
                                v[i].name, v[i].size);
        free(v[i].name);
    }
    free(v);
    return out;
}
