#include "tools/registry.h"
#include "util/diff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const char *MP_SCHEMA_EDIT =
 "{\"type\":\"object\",\"required\":[\"path\",\"old_text\",\"new_text\"],\"additionalProperties\":false,"
 "\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}}}";
static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1024*1024) { fclose(f); return NULL; }
    char *b = malloc((size_t)sz + 1); size_t n = fread(b, 1, (size_t)sz, f); fclose(f);
    b[n] = '\0'; *len = n; return b;
}
char *mp_tool_edit(const cJSON *args, void *ud) {
    const char *root = ud;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(args, "old_text");
    const cJSON *nw = cJSON_GetObjectItemCaseSensitive(args, "new_text");
    if (!cJSON_IsString(p) || !cJSON_IsString(o) || !cJSON_IsString(nw))
        return strdup("error: path/old_text/new_text required");
    char *canon = mp_path_confine(root, p->valuestring);
    if (!canon) return strdup("error: path escapes workspace or missing");
    size_t len; char *text = read_all(canon, &len);
    if (!text) { free(canon); return strdup("error: cannot read file (or >1MiB)"); }
    if (!*o->valuestring) { free(text); free(canon); return strdup("error: old_text empty"); }
    char *hit = strstr(text, o->valuestring);
    if (!hit) { free(text); free(canon); return strdup("error: old_text not found"); }
    if (strstr(hit + 1, o->valuestring)) { free(text); free(canon);
        return strdup("error: old_text ambiguous (multiple matches)"); }
    size_t ol = strlen(o->valuestring), nlw = strlen(nw->valuestring);
    char *merged = malloc(len - ol + nlw + 1);
    size_t pre = (size_t)(hit - text);
    memcpy(merged, text, pre); memcpy(merged + pre, nw->valuestring, nlw);
    strcpy(merged + pre + nlw, text + pre + ol);
    /* produce a small diff preview old vs new (first 400 hunks) */
    static const int MAXH = 400;
    mp_diff_hunk *hunks = malloc(sizeof *hunks * MAXH);
    char *out = malloc(pre + len + nlw + 4096);
    int on = 0;
    /* naive line split for preview */
    char **la = calloc(2048, sizeof *la), **lb = calloc(2048, sizeof *lb);
    int na = 0, nb = 0;
    for (char *s = text; s && *s && na < 2048; ) { char *e = strchr(s, '\n'); size_t l = e ? (size_t)(e - s) : strlen(s);
        la[na] = strndup(s, l); na++; s += l + (e ? 1 : 0); }
    for (char *s = merged; s && *s && nb < 2048; ) { char *e = strchr(s, '\n'); size_t l = e ? (size_t)(e - s) : strlen(s);
        lb[nb] = strndup(s, l); nb++; s += l + (e ? 1 : 0); }
    int nh = hunks ? mp_diff_lines((const char **)la, na, (const char **)lb, nb, hunks, MAXH) : -1;
    on += sprintf(out + on, "edited %s\n", canon);
    if (nh >= 0) for (int i = 0; i < nh && on < 60000; i++) {
        const char *sig = hunks[i].op == MP_DIFF_ADD ? "+" : hunks[i].op == MP_DIFF_DEL ? "-" : " ";
        on += sprintf(out + on, "%s%s\n", sig, hunks[i].text); }
    for (int i = 0; i < na; i++) free(la[i]); for (int i = 0; i < nb; i++) free(lb[i]);
    free(la); free(lb); free(hunks);
    FILE *f = fopen(canon, "wb");
    if (!f) { free(merged); free(text); free(canon); return strdup("error: cannot write file"); }
    fwrite(merged, 1, strlen(merged), f); fclose(f);
    free(merged); free(text); free(canon);
    return out;
}
