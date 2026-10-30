#ifndef MINIPI_NET_SSE_H
#define MINIPI_NET_SSE_H
/* Server-Sent Events line parser (in-house; spec: WHATWG event-stream).
 * Feed raw bytes; emits complete events via callback. Handles CRLF/LF,
 * multi-line data:, comment ':' lines, id:/event: fields. */
#include <stddef.h>
typedef struct mp_sse mp_sse;
typedef void (*mp_sse_cb)(const char *event, const char *data, void *ud);
mp_sse *mp_sse_new(mp_sse_cb cb, void *ud);
void    mp_sse_free(mp_sse *s);
void    mp_sse_feed(mp_sse *s, const char *bytes, size_t n);
void    mp_sse_flush(mp_sse *s); /* dispatch pending event at EOF */
#endif
