#include "store.h"
#include "util/sha256.h"
#include "util/uuid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
struct mp_store { FILE *f; char path[4096]; mp_sha256 running; };
mp_store *mp_store_open(const char *dir, const char *sid) {
    if (!dir || !sid) return NULL;
    char idbuf[64]; strncpy(idbuf, sid, 63); idbuf[63] = '\0';
    for (char *p = idbuf; *p; p++) if (*p=='/'||*p=='.') *p='_'; /* sanitize */
    mkdir(dir, 0700);
    mp_store *s = calloc(1, sizeof *s);
    snprintf(s->path, sizeof s->path, "%s/%s.jsonl", dir, idbuf);
    s->f = fopen(s->path, "a");
    if (!s->f) { free(s); return NULL; }
    mp_sha256_init(&s->running);
    return s;
}
void mp_store_close(mp_store *s){ if(!s)return; fclose(s->f); free(s); }
int mp_store_append(mp_store *s, const char *role, const char *content,
                    const char *tc, const char *parent) {
    if (!s || !role) return -1;
    char uuid[37]; mp_uuid_v4(uuid);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", uuid);
    if (parent) cJSON_AddStringToObject(o, "parent", parent);
    cJSON_AddNumberToObject(o, "ts", (double)time(NULL));
    cJSON_AddStringToObject(o, "role", role);
    cJSON_AddStringToObject(o, "content", content ? content : "");
    if (tc) { cJSON *t = cJSON_Parse(tc); if (t) cJSON_AddItemToObject(o, "tool_call", t); }
    char *line = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    if (!line) return -1;
    int rc = fprintf(s->f, "%s\n", line);
    if (rc > 0) mp_sha256_update(&s->running, line, strlen(line));
    fflush(s->f); free(line);
    return rc > 0 ? 0 : -1;
}
void mp_store_checkpoint(mp_store *s, char out[65]) {
    mp_sha256 copy = s->running; unsigned char d[32];
    mp_sha256_final(&copy, d);
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i*2]=H[d[i]>>4]; out[i*2+1]=H[d[i]&15]; }
    out[64] = '\0';
}
int mp_store_replay(const char *path, mp_ctx *out_ctx) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char *line = NULL; size_t cap = 0; ssize_t n; int cnt = 0;
    while ((n = getline(&line, &cap, f)) > 0) {
        cJSON *o = cJSON_Parse(line); if (!o) continue;
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(o, "role");
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(o, "content");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "tool_call");
        if (cJSON_IsString(r)) {
            mp_ctx_push(out_ctx, r->valuestring, cJSON_IsString(c) ? c->valuestring : "",
                        t ? cJSON_Duplicate(t, 1) : NULL);
            cnt++;
        }
        cJSON_Delete(o);
    }
    free(line); fclose(f); return cnt;
}
