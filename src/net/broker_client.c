#define _GNU_SOURCE
/* Raw-socket HTTP/1.1 POST to the localhost TLS broker. No TLS here. */
#include "broker_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

static int connect_local(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

/* Env-var NAME advertised to the broker via X-Minipi-Key-Env (name only,
 * never the secret). Set by mp_broker_set_key_env() from the REPL/config. */
static char g_key_env[64];

void mp_broker_set_key_env(const char *env_name) {
    if (!env_name || !*env_name) { g_key_env[0] = 0; return; }
    snprintf(g_key_env, sizeof g_key_env, "%s", env_name);
}

int mp_broker_post(const char *path, const char *body, size_t body_len,
                   char **out, size_t *out_len) {
    const char *p = getenv("MINIPI_BROKER_PORT"); 
    int port = (p && *p) ? atoi(p) : 8777;
    int fd = connect_local(port);
    if (fd < 0) return -1;
    /* Absolute-form URI: broker's handler parses netloc as the upstream host. */
    char uri[512];
    snprintf(uri, sizeof uri, "http://127.0.0.1:%d%s", port, path);
    char hdr[1024];
    int hl = snprintf(hdr, sizeof hdr,
        "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json"
        "\r\nContent-Length: %zu\r\nConnection: close%s%s%s\r\n\r\n", uri, port, body_len,
        g_key_env[0] ? "\r\nX-Minipi-Key-Env: " : "", g_key_env, "");
    if (hl < 0 || (size_t)hl >= sizeof hdr) { close(fd); return -1; }
    if (write(fd, hdr, hl) != hl) { close(fd); return -1; }
    size_t off = 0;
    while (off < body_len) {
        ssize_t w = write(fd, body + off, body_len - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    /* read everything until EOF */
    size_t cap = 65536, len = 0; char *buf = malloc(cap);
    if (!buf) { close(fd); return -1; }
    for (;;) {
        if (len + 8192 > cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { free(buf); close(fd); return -1; } buf = nb; }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -1; }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    /* split headers/body at \r\n\r\n */
    char *sep = memmem(buf, len, "\r\n\r\n", 4);
    if (!sep) { free(buf); return -1; }
    size_t status_line_len = strcspn(buf, "\r\n");
    int status = 0;
    if (status_line_len >= 12) sscanf(buf + 9, "%d", &status);
    char *bstart = sep + 4; size_t blen = len - (size_t)(bstart - buf);
    /* handle chunked transfer-encoding (broker sets content-length, but be safe) */
    if (memmem(buf, (size_t)(sep - buf), "Transfer-Encoding: chunked", 24)) {
        /* Locate the FIRST hex chunk-size line at the start of the body. If it
         * doesn't parse as a valid hex length, the upstream used identity
         * framing and we must return the raw bytes verbatim — otherwise SSE
         * payloads beginning with hex-like text ("27A\n{...}") get shredded. */
        int looks_chunked = 0;
        {
            char *nl = memchr(bstart, '\n', blen);
            if (nl) {
                size_t sn = (size_t)(nl - bstart);
                if (bstart[sn-1] == '\r') sn--;
                if (sn > 0 && sn < 16) {
                    for (size_t k = 0; k < sn; k++)
                        if (!isxdigit((unsigned char)bstart[k])) { sn = 0; break; }
                    if (sn) {
                        errno = 0;
                        char tmp[20]; memcpy(tmp, bstart, sn); tmp[sn] = 0;
                        long csz = strtol(tmp, NULL, 16);
                        if (errno == 0 && csz >= 0 && (size_t)csz <= blen) looks_chunked = 1;
                    }
                }
            }
        }
        if (!looks_chunked) goto identity;
        size_t rl = 0, i = 0; char *src = bstart; char *dst = malloc(blen + 1);
        if (!dst) { free(buf); return -1; }
        while (i < blen) {
            int csz = -1;
            /* parse hex size up to CRLF */
            char *nl = memchr(src + i, '\r', blen - i);
            if (!nl) break;
            char szbuf[16] = {0}; size_t sn = (size_t)(nl - (src + i));
            if (sn >= sizeof szbuf) break;
            memcpy(szbuf, src + i, sn); csz = (int)strtol(szbuf, NULL, 16);
            i += sn + 2; /* skip CRLF */
            if (csz <= 0) break;
            if (i + (size_t)csz <= blen) { memcpy(dst + rl, src + i, (size_t)csz); rl += (size_t)csz; }
            i += (size_t)csz + 2; /* skip data + CRLF */
        }
        free(buf);
        *out = dst; *out_len = rl; *out = realloc(*out, rl ? rl : 1);
        return status;
    }
identity:
    {
    char *res = malloc(blen + 1);
    if (!res) { free(buf); return -1; }
    memcpy(res, bstart, blen); res[blen] = 0;
    free(buf);
    *out = res; *out_len = blen;
    return status;
    }
}

/* ---- loopback HTTP for local inference (LM Studio/Ollama/llama.cpp) ---- */
static int split_url(const char *url, const char *path, char *hostbuf, size_t hn,
                     int *port, char *uri, size_t un) {
    const char *base = NULL;
    int p = 80;
    if (!strncmp(url, "http://127.0.0.1", 16)) base = "127.0.0.1";
    else if (!strncmp(url, "http://localhost", 16)) base = "localhost";
    else return -2;
    const char *rest = url + (base[0] == 'l' ? 16 : 16);
    /* skip past scheme+host prefix length properly */
    rest = url + strlen("http://") + strlen(base);
    if (*rest == ':') { p = atoi(rest + 1); while (*rest && *rest != '/') rest++; }
    while (*rest == '/') rest++;
    snprintf(hostbuf, hn, "%s", base);
    *port = p;
    /* uri = /<remaining-base>/<path> */
    char tail[512];
    if (*rest) snprintf(tail, sizeof tail, "/%s", rest); else tail[0] = 0;
    if (tail[strlen(tail)-1] == '/') tail[strlen(tail)-1] = 0;
    snprintf(uri, un, "%s%s", tail, path);
    return 0;
}

int mp_local_http(const char *url_base, const char *path, const char *method,
                  const char *body, size_t body_len, char **out, size_t *out_len) {
    if (!url_base || !path || !method || !out || !out_len) return -1;
    char host[64], uri[1024]; int port = 0;
    if (split_url(url_base, path, host, sizeof host, &port, uri, sizeof uri) < 0) {
        return -2; /* refused: not loopback */
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char portstr[8]; snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { close(fd); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);
    char hdr[1024];
    int hl = snprintf(hdr, sizeof hdr,
        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nAccept: application/json\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        method, uri, host, port, body ? body_len : 0);
    if (hl < 0 || (size_t)hl >= sizeof hdr) { close(fd); return -1; }
    if (write(fd, hdr, hl) != hl) { close(fd); return -1; }
    size_t off = 0;
    while (body && off < body_len) {
        ssize_t w = write(fd, body + off, body_len - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    size_t cap = 65536, len = 0; char *buf = malloc(cap);
    if (!buf) { close(fd); return -1; }
    for (;;) {
        if (len + 8192 > cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { free(buf); close(fd); return -1; } buf = nb; }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return -1; }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    char *sep = memmem(buf, len, "\r\n\r\n", 4);
    if (!sep) { free(buf); return -1; }
    size_t status_line_len = strcspn(buf, "\r\n");
    int status = 0;
    if (status_line_len >= 12) sscanf(buf + 9, "%d", &status);
    char *bstart = sep + 4; size_t blen = len - (size_t)(bstart - buf);
    char *res2 = malloc(blen + 1);
    if (!res2) { free(buf); return -1; }
    memcpy(res2, bstart, blen); res2[blen] = 0;
    free(buf);
    *out = res2; *out_len = blen;
    return status;
}
