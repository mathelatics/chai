#ifndef MINIPI_NET_HTTP_H
#define MINIPI_NET_HTTP_H
/* HTTP client facade. Modes:
 *   MP_HTTP_DEV   — plaintext via ureq (localhost mock only; NEVER for prod)
 *   MP_HTTP_BROKER— TLS via external broker process (docs/ARCHITECTURE.md §7)
 * Callbacks run on the caller thread after mp_http_poll()/blocking call. */
#include <stddef.h>
typedef struct { int status; const char *body; size_t body_len;
                 const char *content_type; } mp_http_resp;
typedef void (*mp_http_cb)(const mp_http_resp *resp, int error, void *ud);
int mp_http_post_json(const char *url, const char *bearer_or_key,
                      const char *json_body, mp_http_cb cb, void *ud);
/* streaming variant: cb invoked per chunk with partial body (SSE lines) */
int mp_http_post_stream(const char *url, const char *key, const char *json_body,
                        mp_http_cb on_chunk, mp_http_cb on_done, void *ud);
#endif
