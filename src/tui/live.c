#define _GNU_SOURCE
/* Transient live-region renderer (C-3). Single-threaded; called from the REPL
 * between provider deltas. No termbox dependency: plain VT100 on a tty, and a
 * no-op fallback when stdout is redirected (keeps pipes/CI clean). */
#include "tui/live.h"
#include "util/utf8.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SPIN "|/-\\"
static int active = 0;
static int rows_drawn = 0;         /* lines currently occupying the live area */
static int spin_i = 0;
static char status[256] = "";
static double t_start = 0;
static int is_tty = -1;

static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (double)ts.tv_sec + 1e-9*(double)ts.tv_nsec; }
static int tty_out(void){ if(is_tty<0) is_tty=isatty(1); return is_tty; }

static void sync_begin(void){ if(tty_out()) fputs("\033[?2026h", stdout); }
static void sync_end(void){ if(tty_out()) fputs("\033[?2026l", stdout); }

/* sanitized write of arbitrary text to stdout */
static void swrite(const char *t, size_t n){
    if(n==SIZE_MAX) n=strlen(t);
    mp_utf8_san s; mp_utf8_san_init(&s);
    char out[1024];
    while(n){
        size_t chunk = n>768?768:n, used=0;
        if(mp_utf8_sanitize(&s,t,chunk,out,sizeof out,&used)==0) fwrite(out,1,used,stdout);
        size_t consumed=chunk; t+=consumed; n-=consumed;
    }
}

static void erase_live(void){
    if(!rows_drawn) return;
    /* move up over our rows and clear each */
    for(int i=0;i<rows_drawn;i++) fputs("\r\033[2K\033[1A", stdout);
    fputs("\r", stdout);
    rows_drawn = 0;
}

static void draw_live(void){
    if(!active || !tty_out()) return;
    erase_live();
    sync_begin();
    const char frame[2] = { SPIN[spin_i % 4], 0 };
    printf("%s %s (%s)", frame, status, mp_live_elapsed_str());
    fputc('\n', stdout);
    rows_drawn = 1;
    sync_end();
    fflush(stdout);
}

void mp_live_start(const char *st){
    active = 1; spin_i = 0; t_start = now_s();
    snprintf(status, sizeof status, "%s", st ? st : "working");
    /* strip controls from user-provided status immediately */
    draw_live();
}
void mp_live_update(const char *st){
    if(!active) { mp_live_start(st); return; }
    snprintf(status, sizeof status, "%s", st ? st : "");
    draw_live();
}
void mp_live_tick(void){
    if(!active) return;
    spin_i++;
    draw_live();
}
void mp_live_print_above(const char *text, size_t len){
    int was = active;
    if(was) erase_live();          /* permanent output goes above live region */
    swrite(text, len);
    if(text[len==SIZE_MAX?strlen(text):len-1] != '\n') fputc('\n', stdout);
    fflush(stdout);
    if(was) draw_live();           /* redraw live below it */
}
const char *mp_live_elapsed_str(void){
    static char b[32];
    double e = now_s() - t_start;
    snprintf(b, sizeof b, "%.1fs", e);
    return b;
}
void mp_live_stop(void){
    if(!active) return;
    erase_live();
    active = 0; status[0] = 0;
    fflush(stdout);
}
int mp_live_active(void){ return active; }
