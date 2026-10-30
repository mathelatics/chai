/* tls_broker — M2 TLS isolation boundary (docs/ARCHITECTURE.md §7).
 * The minipi binary stays MIT-only and never performs TLS itself: it speaks
 * plaintext HTTP/1.1 to a localhost broker, which is the ONLY component that
 * may use an approved non-MIT TLS backend (python stdlib ssl / OpenSSL).
 * Usage:  minipi ... | python3 tools/tls_broker.py  (broker serves :8777)
 * This file provides the client-side plumbing used by net/http dev mode. */
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const char *mp_broker_url(void) {
    static char url[256];
    const char *port = getenv("MINIPI_BROKER_PORT");
    snprintf(url, sizeof url, "http://127.0.0.1:%s", (port && *port) ? port : "8777");
    return url;
}
