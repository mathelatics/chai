#include "http.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef MINIPI_WITH_UREQ
#include "ureq.h"
#endif
int mp_http_post_json(const char *url, const char *key, const char *body,
                      mp_http_cb cb, void *ud) {
    if (!url || !body || !cb) return -1;
#ifdef MINIPI_WITH_UREQ
    /* DEV MODE ONLY: plaintext HTTP to a localhost mock server. The URL must
     * start with http://127.0.0.1 or http://localhost — enforced here. */
    if (strncmp(url, "http://127.0.0.1", 16) && strncmp(url, "http://localhost", 16)) {
        cb(&(mp_http_resp){0,NULL,NULL,NULL}, -1, ud);
        return -2;
    }
    struct ureq_response r = ureq_post().url(url).header("Content-Type","application/json")
        .body(body,-1).timeout(30000).call();
    if (r.error) { log_error("http: %s", r.error); cb(&(mp_http_resp){r.code,NULL,NULL,NULL}, -1, ud); return -1; }
    cb(&(mp_http_resp){r.code, r.body, r.body_size, r.header ? "" : ""}, 0, ud);
    return 0;
#else
    (void)key;(void)ud;
    log_warn("http: compiled without transport (M2 will add libuv+broker path)");
    return -3;
#endif
}
int mp_http_post_stream(const char *url, const char *key, const char *body,
                        mp_http_cb on_chunk, mp_http_cb on_done, void *ud) {
    /* M2: implement over libuv tcp + http_parser callbacks feeding sse.c.
     * For now, buffer whole response and deliver once. */
    (void)on_chunk;
    return mp_http_post_json(url, key, body, on_done, ud);
}
