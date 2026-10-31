/* grep tool (pi coding-agent parity): recursive literal/regex search confined
 * to the workspace using POSIX ERE. Literal patterns use fast substring
 * matching (case-folded via regex when ignore_case). Skips binary files (NUL
 * byte in first 8 KiB) and heavy dirs; bounded match count. Output lines are
 * "relpath:lineno: text" — exactly what models expect. Read-only. */
#include "tools/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <regex.h>
#include <sys/stat.h>

#define MP_GREP_MAX_MATCHES 400
#define MP_GREP_MAX_DEPTH   12
#define MP_GREP_LINE_MAX    500

const char *MP_SCHEMA_GREP =
 "{\"type\":\"object\",\"required\":[\"pattern\"],\"additionalProperties\":false,"
 "\"properties\":{\"pattern\":{\"type\":\"string\"},"
 "\"root\":{\"type\":\"string\"},"
 "\"ignore_case\":{\"type\":\"boolean\"}}}";

typedef struct { regex_t re; int used_regex; char lit[256]; int icase_lit; } matcher;

static int line_matches(const matcher *m, const char *line) {
    if (m->used_regex) return regexec(&m->re, line, 0, NULL, 0) == 0;
    if (m->icase_lit) {
        /* case-insensitive substring without regex cost */
        size_t nl = strlen(line), pl = strlen(m->lit);
        if (pl > nl) return 0;
        for (size_t i = 0; i + pl <= nl; i++) {
            size_t j = 0;
            for (; j < pl; j++)
                if (tolower((unsigned char)line[i + j]) != m->lit[j]) break;
            if (j == pl) return 1;
        }
        return 0;
    }
    return strstr(line, m->lit) != NULL;
}

static int skipdir(const char *name) {
    static const char *SKIP[] = {".git", "node_modules", "__pycache__", NULL};
    for (int i = 0; SKIP[i]; i++) if (!strcmp(name, SKIP[i])) return 1;
    return 0;
}

typedef struct { char *buf; size_t len, cap; int dropped; } obuf;
static void o_add(obuf *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t nc = o->cap ? o->cap : 4096;
        while (nc < o->len + n + 1) nc *= 2;
        char *nb = realloc(o->buf, nc);
        if (!nb) { o->dropped = 1; return; }
        o->buf = nb; o->cap = nc;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

typedef struct { obuf *out; const matcher *m; int depth; int matches; } gctx;

static void scan_file(gctx *g, const char *full, const char *rel) {
    FILE *f = fopen(full, "rb");
    if (!f) return;
    char probe[8192];
    size_t pn = fread(probe, 1, sizeof probe, f);
    for (size_t i = 0; i < pn; i++)
        if (!probe[i]) { fclose(f); return; }        /* binary: skip */
    fseek(f, 0, SEEK_SET);
    char line[MP_GREP_LINE_MAX + 2];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line_matches(g->m, line)) continue;
        if (g->matches >= MP_GREP_MAX_MATCHES) continue;
        g->matches++;
        char hdr[1024];
        int hl = snprintf(hdr, sizeof hdr, "%s:%d: ", rel, lineno);
        o_add(g->out, hdr, (size_t)hl);
        size_t ll = strlen(line);
        if (ll > MP_GREP_LINE_MAX) ll = MP_GREP_LINE_MAX;
        o_add(g->out, line, ll);
        o_add(g->out, "\n", 1);
    }
    fclose(f);
}

static void walk(gctx *g, const char *dir, const char *rel, int max_depth) {
    if (g->depth > max_depth || g->matches >= MP_GREP_MAX_MATCHES) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[4096], rl[4096];
        if (snprintf(full, sizeof full, "%s/%s", dir, de->d_name) >= (int)sizeof full) continue;
        if (snprintf(rl, sizeof rl, "%s%s/", rel, de->d_name) >= (int)sizeof rl) continue;
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (skipdir(de->d_name)) continue;
            g->depth++;
            walk(g, full, rl, max_depth);
            g->depth--;
        } else if (S_ISREG(st.st_mode)) {
            char one[4096];
            snprintf(one, sizeof one, "%s%s", rel, de->d_name);
            scan_file(g, full, one);
        }
        if (g->matches >= MP_GREP_MAX_MATCHES) break;
    }
    closedir(d);
}

/* escape a literal pattern into an ERE-safe form (for invalid-UTF robustness
 * we only ever build from validated args upstream in the registry) */
static int compile_ere(regex_t *re, const char *pat, int icase) {
    char esc[1024]; size_t e = 0;
    for (size_t k = 0; pat[k] && e + 2 < sizeof esc; k++) {
        char c = pat[k];
        if (strchr("\\^$.|?*+()[]{}", c)) esc[e++] = '\\';
        esc[e++] = c;
    }
    esc[e] = '\0';
    return regcomp(re, esc, REG_EXTENDED | (icase ? REG_ICASE : 0));
}

char *mp_tool_grep(const cJSON *args, void *ud) {
    const char *root = ud;
    const cJSON *pj = cJSON_GetObjectItemCaseSensitive(args, "pattern");
    if (!cJSON_IsString(pj) || !pj->valuestring[0])
        return strdup("error: pattern must be a non-empty string");
    const cJSON *rj = cJSON_GetObjectItemCaseSensitive(args, "root");
    const cJSON *ij = cJSON_GetObjectItemCaseSensitive(args, "ignore_case");
    const char *subroot = (cJSON_IsString(rj) && rj->valuestring[0]) ? rj->valuestring : ".";
    int icase = cJSON_IsBool(ij) ? cJSON_IsTrue(ij) : 0;
    char *canon = mp_path_confine(root, subroot);
    if (!canon) return strdup("error: root escapes workspace or does not exist");

    const char *pat = pj->valuestring;
    matcher m; memset(&m, 0, sizeof m);
    int looks_regex = strpbrk(pat, "^$|\\+*?[]{}()") != NULL;
    if (looks_regex && !icase) {
        if (regcomp(&m.re, pat, REG_EXTENDED)) { free(canon); return strdup("error: invalid regular expression"); }
        m.used_regex = 1;
    } else if (looks_regex || icase) {
        /* regex-with-icase, or literal-with-icase over long pattern: escape and
         * compile once (REG_ICASE handles folding correctly for ASCII) */
        if (strlen(pat) < sizeof m.lit && !looks_regex) {
            size_t i = 0;
            for (; pat[i]; i++) m.lit[i] = (char)tolower((unsigned char)pat[i]);
            m.lit[i] = '\0';
            m.icase_lit = 1;
        } else {
            if (compile_ere(&m.re, pat, icase)) { free(canon); return strdup("error: invalid pattern"); }
            m.used_regex = 1;
        }
    } else {
        snprintf(m.lit, sizeof m.lit, "%.255s", pat);
    }

    obuf out = {0};
    gctx g = { .out = &out, .m = &m, .depth = 0, .matches = 0 };
    struct stat st;
    if (stat(canon, &st) == 0 && S_ISREG(st.st_mode)) {
        const char *slash = strrchr(canon, '/');
        scan_file(&g, canon, slash ? slash + 1 : canon);
    } else {
        walk(&g, canon, "", MP_GREP_MAX_DEPTH);
    }
    if (m.used_regex) regfree(&m.re);
    free(canon);

    char hdr[128];
    int hl = snprintf(hdr, sizeof hdr, "%d match%s%s\n", g.matches,
                      g.matches == 1 ? "" : "es",
                      g.matches >= MP_GREP_MAX_MATCHES ? " (limit reached)" : "");
    char *fin = malloc((size_t)hl + out.len + 1);
    if (!fin) { free(out.buf); return strdup("error: out of memory"); }
    memcpy(fin, hdr, (size_t)hl);
    if (out.len) memcpy(fin + hl, out.buf, out.len);
    fin[hl + out.len] = '\0';
    free(out.buf);
    return fin;
}
