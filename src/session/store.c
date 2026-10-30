/* JSONL session store with sha256 hash-chain checkpoints.
 * Every line: {"id","parent","role","content","tool_calls"?,"hash"} where
 * hash = sha256(prev_hash || id || role || content) — tamper-evident chain.
 * All failures log errno + full path (silent store failures hid a session-dir
 * bug; never again). Store failure is non-fatal by design: the session keeps
 * running in memory, but the user always sees WHY nothing was persisted. */
#include "session/store.h"
#include "util/sha256.h"
#include "util/paths.h"
#include "util/uuid.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* fsync */

struct mp_store { char path[4096]; char prev_hash[65]; FILE *f; int n; };

static void chain_hash(const char *prev, const char *id, const char *role,
                       const char *content, char out[65]) {
    mp_sha256 h; mp_sha256_init(&h);
    mp_sha256_update(&h, prev, strlen(prev));
    mp_sha256_update(&h, "|", 1);
    mp_sha256_update(&h, id, strlen(id));
    mp_sha256_update(&h, "|", 1);
    mp_sha256_update(&h, role, strlen(role));
    mp_sha256_update(&h, "|", 1);
    mp_sha256_update(&h, content, strlen(content));
    unsigned char d[32]; mp_sha256_final(&h, d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i*2] = hx[d[i] >> 4]; out[i*2+1] = hx[d[i] & 15]; }
    out[64] = '\0';
}

mp_store *mp_store_open(const char *sessdir, const char *session_id) {
    if (!sessdir || !session_id) return NULL;
    mp_store *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (mp_mkdir_p(sessdir) != 0) {
        log_error("store: cannot create session dir %s: %s", sessdir, strerror(errno));
        free(s); return NULL;
    }
    snprintf(s->path, sizeof s->path, "%s/%s.jsonl", sessdir, session_id);
    strcpy(s->prev_hash, "GENESIS");
    /* replay existing file to reseed chain + count (round-trip resume) */
    FILE *r = fopen(s->path, "r");
    if (r) {
        char line[65536];
        while (fgets(line, sizeof line, r)) {
            cJSON *j = cJSON_Parse(line);
            if (!j) continue;
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(j, "hash");
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
            const cJSON *ro = cJSON_GetObjectItemCaseSensitive(j, "role");
            const cJSON *ct = cJSON_GetObjectItemCaseSensitive(j, "content");
            if (cJSON_IsString(h) && strlen(h->valuestring) == 64) {
                memcpy(s->prev_hash, h->valuestring, 65);
                s->n++;
            } else if (cJSON_IsString(id) && cJSON_IsString(ro) && cJSON_IsString(ct)) {
                char calc[65]; chain_hash(s->prev_hash, id->valuestring, ro->valuestring, ct->valuestring, calc);
                memcpy(s->prev_hash, calc, 65); s->n++;
            }
            cJSON_Delete(j);
        }
        fclose(r);
        log_info("store: resumed %d entries from %s", s->n, s->path);
    }
    s->f = fopen(s->path, "a");
    if (!s->f) {
        log_error("store: cannot open %s for append: %s", s->path, strerror(errno));
        free(s); return NULL;
    }
    return s;
}

int mp_store_append(mp_store *s, const char *role, const char *content,
                    const char *tool_calls_json, const char *meta_json) {
    if (!s || !s->f) return -1;
    char id[37];
    if (mp_uuid_v4(id)) { log_error("store: no entropy for uuid: %s", strerror(errno)); return -1; }
    char hash[65];
    chain_hash(s->prev_hash, id, role ? role : "", content ? content : "", hash);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    /* parent = previous entry id not tracked per-line here; chain hash covers it */
    cJSON_AddStringToObject(o, "parent", s->n ? "" : "root");
    cJSON_AddStringToObject(o, "role", role ? role : "");
    cJSON_AddStringToObject(o, "content", content ? content : "");
    if (tool_calls_json) {
        cJSON *tc = cJSON_Parse(tool_calls_json);
        if (tc) cJSON_AddItemToObject(o, "tool_calls", tc);
    }
    if (meta_json) {
        cJSON *mt = cJSON_Parse(meta_json);
        if (mt) cJSON_AddItemToObject(o, "meta", mt);
    }
    cJSON_AddStringToObject(o, "hash", hash);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return -1;
    size_t tl = strlen(txt);
    int ok = (fwrite(txt, 1, tl, s->f) == tl) && (putc('\n', s->f) != EOF) && (fflush(s->f) == 0);
    free(txt);
    if (!ok) { log_error("store: write to %s failed: %s", s->path, strerror(errno)); return -1; }
    memcpy(s->prev_hash, hash, 65);
    s->n++;
    return 0;
}

int mp_store_checkpoint(mp_store *s) {
    if (!s) return -1;
    if (fflush(s->f)) { log_error("store: flush %s failed: %s", s->path, strerror(errno)); return -1; }
    if (fsync(fileno(s->f)))  { log_error("store: fsync %s failed: %s", s->path, strerror(errno)); return -1; }
    log_info("store: checkpoint %s at entry %d hash=%s…", s->path, s->n,
             s->prev_hash);
    return 0;
}

const char *mp_store_path(const mp_store *s) { return s ? s->path : NULL; }
int mp_store_count(const mp_store *s) { return s ? s->n : 0; }

void mp_store_close(mp_store *s) {
    if (!s) return;
    if (s->f) { fflush(s->f); fclose(s->f); }
    free(s);
}

/* ---- replay + chain verification ---------------------------------------- */
static int store_scan(const char *path, mp_ctx *out_ctx /*may be NULL*/,
                      int *bad_line) {
    FILE *f = fopen(path, "r");
    if (!f) { log_error("store: cannot open %s: %s", path, strerror(errno)); return -2; }
    char prev[65]; strcpy(prev, "GENESIS");
    char line[65536];
    int ln = 0, msgs = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
        if (!l) continue;
        cJSON *j = cJSON_Parse(line);
        if (!j) { log_error("store: %s:%d: not valid JSON", path, ln); if (!bad) bad = ln; cJSON_Delete(j); continue; }
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(j, "id");
        const cJSON *ro = cJSON_GetObjectItemCaseSensitive(j, "role");
        const cJSON *ct = cJSON_GetObjectItemCaseSensitive(j, "content");
        const cJSON *h  = cJSON_GetObjectItemCaseSensitive(j, "hash");
        if (!(cJSON_IsString(id) && cJSON_IsString(ro) && cJSON_IsString(ct) &&
              cJSON_IsString(h) && strlen(h->valuestring) == 64)) {
            log_error("store: %s:%d: missing/invalid chain fields", path, ln);
            if (!bad) bad = ln;
        } else {
            char calc[65];
            chain_hash(prev, id->valuestring, ro->valuestring, ct->valuestring, calc);
            if (memcmp(calc, h->valuestring, 65) != 0) {
                log_error("store: %s:%d: CHAIN MISMATCH (tamper or truncation): want %s got %s",
                          path, ln, calc, h->valuestring);
                if (!bad) bad = ln;
                memcpy(prev, h->valuestring, 65); /* resync so we report ALL breaks */
            } else {
                memcpy(prev, h->valuestring, 65);
                msgs++;
                if (out_ctx) {
                    const cJSON *tc = cJSON_GetObjectItemCaseSensitive(j, "tool_calls");
                    mp_ctx_push(out_ctx, ro->valuestring, ct->valuestring,
                                tc ? cJSON_Duplicate(tc, 1) : NULL);
                }
            }
        }
        cJSON_Delete(j);
    }
    fclose(f);
    if (bad_line) *bad_line = bad;
    if (bad) return -1;
    return msgs;
}

int mp_store_replay(const char *path, mp_ctx *out_ctx) {
    if (!path) return -1;
    int bad = 0;
    int r = store_scan(path, out_ctx, &bad);
    if (r < 0) { log_error("store: replay %s aborted at line %d", path, bad); return -1; }
    log_info("store: replayed %d verified entries from %s", r, path);
    return r;
}

int mp_store_verify(const char *path) {
    int bad = 0;
    int r = store_scan(path, NULL, &bad);
    if (r == -2) return -2;
    if (r < 0) { log_error("store: VERIFY FAILED %s — first break at line %d", path, bad); return -1; }
    log_info("store: verify OK %s (%d entries, chain intact)", path, r);
    return 0;
}
