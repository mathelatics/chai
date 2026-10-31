/* Stream-safe terminal output for the REPL (C-1).
 * Sanitizes every byte that comes from providers/tools before it touches the
 * tty; reasoning is visually separated from the answer stream. */
#include "tui/disp.h"
#include "util/utf8.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define OUT_CAP 4096
static mp_utf8_san st[3];
static int last_nl[3];          /* did the previous emitted chunk end in \n? */
static int reasoning_on = 0;
static int reasoning_prefix = 1;/* print "thinking: " marker once per stream */

void mp_disp_set_reasoning_enabled(int on){
    if(on != reasoning_on){
        reasoning_on = on;
        if(!on){ mp_utf8_san_init(&st[MP_DISP_REASONING]); last_nl[MP_DISP_REASONING]=1; }
        reasoning_prefix = 1;
    }
}
int mp_disp_reasoning_enabled(void){ return reasoning_on; }
void mp_disp_reset(void){
    for(int i=0;i<3;i++){ mp_utf8_san_init(&st[i]); last_nl[i]=1; }
    reasoning_prefix = 1;
}

static void emit(mp_disp_stream s, const char *buf, size_t n){
    if(!n) return;
    if(s == MP_DISP_REASONING){
        fputs("\033[2m", stdout);            /* dim */
        fwrite(buf, 1, n, stdout);
        fputs("\033[0m", stdout);
    } else {
        fwrite(buf, 1, n, stdout);
    }
    last_nl[s] = buf[n-1] == '\n';
}

void mp_disp_write(mp_disp_stream s, const char *text, size_t len){
    if(s<0 || s>2 || !text) return;
    if(len == SIZE_MAX) len = strlen(text);
    if(s == MP_DISP_REASONING && !reasoning_on) return;
    if(s == MP_DISP_REASONING && reasoning_prefix){
        reasoning_prefix = 0;
        emit(s, "thinking: ", 10);           /* visual separation of streams */
    }
    char out[OUT_CAP];
    while(len){
        size_t used = 0;
        /* sanitizer never expands input (escapes shrink, invalid -> 3-byte
         * FFFD for <=4-byte seqs), so OUT_CAP-sized chunks always fit. */
        int r = mp_utf8_sanitize(&st[s], text, len > 3072 ? 3072 : len,
                                 out, sizeof out, &used);
        if(r == -1) break;                   /* defensive: cannot happen */
        size_t consumed = (len > 3072 ? 3072 : len);
        emit(s, out, used);
        text += consumed; len -= consumed;
    }
    fflush(stdout);
}

void mp_disp_end(mp_disp_stream s){
    if(s<0 || s>2) return;
    char out[8]; size_t used = 0;
    mp_utf8_flush(&st[s], out, sizeof out, &used);
    emit(s, out, used);
    if(!last_nl[s]){ emit(s, "\n", 1); }
    if(s == MP_DISP_REASONING) reasoning_prefix = 1;
    fflush(stdout);
}
