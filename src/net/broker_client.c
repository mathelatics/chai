#define _GNU_SOURCE
/* Raw-socket HTTP/1.1 POST to the localhost TLS broker. No TLS here. */
#include "broker_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
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
        "\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", uri, port, body_len);
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
    char *res = malloc(blen + 1);
    if (!res) { free(buf); return -1; }
    memcpy(res, bstart, blen); res[blen] = 0;
    free(buf);
    *out = res; *out_len = blen;
    return status;
}
