#ifndef MINIPI_UTIL_UTF8_H
#define MINIPI_UTIL_UTF8_H
#include <stddef.h>
/* Strict UTF-8 validation (rejects overlongs, surrogates, >U+10FFFF, NUL). */
int  mp_utf8_valid(const char *s, size_t n);
size_t mp_utf8_len(const char *s, size_t n);      /* codepoints; 0 if invalid */
size_t mp_utf8_cp_bytes(unsigned char lead);       /* 0 => invalid lead/continuation */

/* --- streaming sanitizer (C-1) ---------------------------------------------
 * Converts arbitrary byte chunks into stream-safe UTF-8 text:
 *  - strips ANSI/VT escape sequences (CSI ... final-byte, OSC BEL|ST, ESC Fe,
 *    two-char escapes like ESC[... mid-sequence truncation is handled by the
 *    state machine across chunk boundaries);
 *  - drops C0 control bytes except \n and \t (also DEL 0x7F);
 *  - replaces invalid/truncated UTF-8 with U+FFFD; a trailing incomplete
 *    multibyte sequence is carried in the state until the next chunk or flush.
 * Usage: while streaming call mp_utf8_sanitize() per delta; when the message
 * ends call mp_utf8_flush() to emit any pending replacement chars.          */
typedef struct {
    unsigned char carry[4];  /* partial UTF-8 sequence from previous chunk */
    size_t   carry_len;
    int      esc;            /* 0 none, 1 ESC seen, 2 CSI params, 3 CSI intermediate, 4 OSC */
    int      osc_term;       /* OSC: saw ESC (possible ST terminator) */
} mp_utf8_san;

void mp_utf8_san_init(mp_utf8_san *s);
/* Appends sanitized output to out[*used..cap); advances *used. Never writes
 * raw control bytes other than \n/\t. Returns 0 on success, -1 if out is full
 * (state stays consistent; caller may grow cap and retry the same chunk). */
int mp_utf8_sanitize(mp_utf8_san *s, const char *in, size_t n,
                     char *out, size_t cap, size_t *used);
/* End of stream: emits U+FFFD for any dangling partial sequence, resets state. */
void mp_utf8_flush(mp_utf8_san *s, char *out, size_t cap, size_t *used);
#endif
