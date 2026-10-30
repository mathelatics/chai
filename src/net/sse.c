#include "sse.h"
#include <stdlib.h>
#include <string.h>
#define SSE_BUF_MAX (256*1024)
struct mp_sse { char *buf; size_t len, cap; char ev[64]; char *data; size_t dlen, dcap; mp_sse_cb cb; void *ud; };
mp_sse *mp_sse_new(mp_sse_cb cb, void *ud) {
    mp_sse *s = calloc(1, sizeof *s); s->cb = cb; s->ud = ud;
    s->cap = 8192; s->buf = malloc(s->cap); s->dcap = 8192; s->data = malloc(s->dcap);
    return s; }
void mp_sse_free(mp_sse *s){ if(!s)return; free(s->buf); free(s->data); free(s); }
static void dispatch(mp_sse *s) {
    if (s->dlen) {
        if (s->data[s->dlen-1] == '\n') s->dlen--;   /* spec: strip trailing LF */
        s->data[s->dlen] = '\0';
        s->cb(s->ev[0]?s->ev:"message", s->data, s->ud);
    }
    s->dlen = 0; s->ev[0] = '\0';
}
static void handle_line(mp_sse *s, const char *line, size_t n) {
    if (n == 0) { dispatch(s); return; }
    if (line[0] == ':') return;                              /* comment */
    const char *colon = memchr(line, ':', n);
    size_t klen = colon ? (size_t)(colon - line) : n;
    const char *val = colon ? colon + 1 : line + n;
    size_t vlen = n - (klen + 1); if (vlen && val[0] == ' ') { val++; vlen--; }
    if (klen == 4 && !memcmp(line, "data", 4)) {
        if (s->dlen + vlen + 2 > s->dcap) { size_t nc = (s->dlen+vlen+2)*2; if(nc>SSE_BUF_MAX)return;
            s->data = realloc(s->data, nc); s->dcap = nc; }
        memcpy(s->data + s->dlen, val, vlen); s->dlen += vlen; s->data[s->dlen++] = '\n';
    } else if (klen == 5 && !memcmp(line, "event", 5)) {
        size_t c = vlen > 63 ? 63 : vlen; memcpy(s->ev, val, c); s->ev[c] = '\0';
    }
}
void mp_sse_feed(mp_sse *s, const char *bytes, size_t n) {
    while (n--) {
        char c = *bytes++;
        if (c == '\r') continue;
        if (c == '\n') { handle_line(s, s->buf, s->len); s->len = 0; continue; }
        if (s->len + 1 < s->cap) s->buf[s->len++] = c;
        else { handle_line(s, s->buf, s->len); s->len = 0; } /* overlong line: flush */
    }
}
void mp_sse_flush(mp_sse *s) { if (s->len) { handle_line(s, s->buf, s->len); s->len = 0; } dispatch(s); }
