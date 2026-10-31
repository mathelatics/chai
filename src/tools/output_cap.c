/* C-2: tool-output capping applied before results re-enter model context.
 * See output_cap.h for policy. Implementation notes:
 *  - single pass to count lines & total bytes; then decide;
 *  - head/tail split at line boundaries (always codepoint boundaries);
 *  - over-wide lines are trimmed keeping head+tail of the line, cutting on a
 *    UTF-8 lead byte (scan back from candidate offset past continuation bytes);
 *  - every kept line passes through mp_utf8_sanitize as a belt-and-braces
 *    filter so nothing that leaves here can corrupt a terminal or context. */
#include "tools/output_cap.h"
#include "util/utf8.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Largest <= off that starts a UTF-8 codepoint (not a continuation byte). */
static size_t cp_boundary(const char *t, size_t start, size_t off) {
    if (off <= start) return start;
    size_t b = off;
    while (b > start && ((unsigned char)t[b] & 0xC0) == 0x80) b--;
    return b;
}

/* Trim one logical line to maxw bytes approx, keeping both ends with marker.
 * out must have room for len + 4. Returns bytes written. */
static size_t trim_line(const char *line, size_t len, int maxw, char *out, size_t cap) {
    if (maxw <= 0 || (size_t)maxw >= len) {
        size_t n = len < cap ? len : cap;
        memcpy(out, line, n); return n;
    }
    size_t head = cp_boundary(line, 0, (size_t)(maxw / 2));
    size_t tstart = cp_boundary(line, (size_t)maxw / 2, len - (size_t)(maxw - maxw / 2));
    size_t used = 0;
    if (head + 3 + (len - tstart) <= cap) {
        memcpy(out + used, line, head); used += head;
        memcpy(out + used, "...", 3); used += 3;
        memcpy(out + used, line + tstart, len - tstart); used += len - tstart;
    } else {
        memcpy(out, line, (size_t)maxw < cap ? (size_t)maxw : cap);
        used = (size_t)maxw < cap ? (size_t)maxw : cap;
    }
    return used;
}

typedef struct { const char *p; size_t n; } lspan;

char *mp_output_cap(const char *text, size_t len,
                    size_t max_bytes, int max_lines_per_half, int max_width) {
    if (!text) return NULL;
    if (len == SIZE_MAX) len = strlen(text);
    if (max_bytes == 0) max_bytes = MP_CAP_DEFAULT_BYTES;
    if (max_lines_per_half <= 0) max_lines_per_half = MP_CAP_DEFAULT_LINES;
    if (max_width <= 0) max_width = MP_CAP_DEFAULT_WIDTH;

    /* Count lines: newline-delimited, trailing \n does not create empty line. */
    int lines = (len > 0);
    for (size_t i = 0; i < len; i++) if (text[i] == '\n' && i + 1 < len) lines++;

    int need_cap = (len > max_bytes) || (lines > 2 * max_lines_per_half);
    if (!need_cap && max_width > 0) {
        size_t ls = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || text[i] == '\n') {
                if (i - ls > (size_t)max_width) { need_cap = 1; break; }
                ls = i + 1;
            }
        }
    }
    if (!need_cap) {
        char *c = malloc(len + 1);
        if (c) { memcpy(c, text, len); c[len] = 0; }
        return c;
    }

    lspan *v = malloc(sizeof(lspan) * (size_t)(lines + 1));
    if (!v) return NULL;
    int nl = 0; size_t s = 0;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') { v[nl].p = text + s; v[nl].n = i - s; nl++; s = i + 1; }
    if (s < len) { v[nl].p = text + s; v[nl].n = len - s; nl++; }

    int keep_head = max_lines_per_half, keep_tail = max_lines_per_half;
    if (nl <= keep_head + keep_tail) { keep_head = nl / 2; keep_tail = nl - keep_head; }
    int elided_lines = nl - keep_head - keep_tail;
    if (elided_lines < 0) elided_lines = 0;

    size_t scratchcap = (size_t)max_width * 2 + 64;
    char *scratch = malloc(scratchcap);
    size_t obuf_cap = (size_t)(keep_head + keep_tail + 4) * (scratchcap) + 512;
    char *obuf = malloc(obuf_cap);
    if (!scratch || !obuf) { free(scratch); free(obuf); free(v); return NULL; }
    size_t used = 0, emitted = 0;
    int marker_written = 0;

    /* helper macro-ish: emit the elision marker once */
    #define EMIT_MARKER(NL_EXTRA) do {                                        \
        if (!marker_written) {                                                \
            marker_written = 1;                                               \
            char mk[160];                                                     \
            int mm = snprintf(mk, sizeof mk,                                  \
                "[... %d lines elided by minipi output cap ...]\n",           \
                elided_lines + (NL_EXTRA));                                   \
            if (mm > 0 && used + (size_t)mm < obuf_cap) {                     \
                memcpy(obuf + used, mk, (size_t)mm); used += (size_t)mm;      \
            }                                                                 \
        }                                                                     \
    } while (0)

        for (int i = 0; i < nl; i++) {
        int in_head = i < keep_head;
        int in_tail = i >= nl - keep_tail;
        if (!in_head && !in_tail) continue;
        /* byte budget: stop early; remaining kept lines count as elided */
        if (emitted >= max_bytes) {
            /* conservative: all not-yet-emitted kept lines are dropped now */
            EMIT_MARKER(0);
            break;
        }
        size_t w = trim_line(v[i].p, v[i].n, max_width, scratch, scratchcap);
        mp_utf8_san zn; mp_utf8_san_init(&zn);
        size_t zused = 0;
        if (mp_utf8_sanitize(&zn, scratch, w, scratch, scratchcap, &zused) == 0) w = zused;
        size_t fu = 0;
        if (scratchcap > w) mp_utf8_flush(&zn, scratch + w, scratchcap - w, &fu);
        w += fu;
        if (used + w + 1 >= obuf_cap) { EMIT_MARKER(0); break; }
        if (i == keep_head && elided_lines > 0) EMIT_MARKER(0);
        memcpy(obuf + used, scratch, w); used += w; obuf[used++] = '\n';
        emitted += w + 1;
    }
    if (!marker_written && elided_lines > 0) EMIT_MARKER(0);
    #undef EMIT_MARKER
    free(scratch); free(v);
    obuf[used] = 0;
    return obuf;
}
