/* C-4 raw-mode line editor (port of pi-tui Editor concepts). Keystroke
 * decoding is explicit: canonical-off termios, ESC sequences decoded by us
 * (arrows/home/end/del/bracketed-paste), UTF-8-aware cursor snapped to
 * codepoint boundaries. Non-tty stdin degrades to getline so pipes/CI work.
 * Key codes used internally: printable bytes pass through; control bytes are
 * their own codes; decoded specials use negative pseudo-codes:
 *   -1 left  -2 right  -3 up(hist)  -4 down(hist)  -5 home  -6 end  -7 delete
 *   -8 ctrl-left(word back)   -9 ctrl-right(word fwd)  -10 paste-start
 *   -11 paste-end */
#define _GNU_SOURCE
#include "tui/input.h"
#include "util/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/time.h>

static struct termios saved_ti; static int raw_on = 0;
static void raw_enable(void){ if(raw_on) return; struct termios t; if(tcgetattr(0,&saved_ti)) return;
    t=saved_ti; t.c_lflag &= ~(unsigned)(ICANON|ECHO); t.c_iflag &= ~(unsigned)(IXON|BRKINT|ISTRIP);
    t.c_cc[VMIN]=1; t.c_cc[VTIME]=0; tcsetattr(0,TCSANOW,&t); raw_on=1; }
static void raw_disable(void){ if(raw_on){ tcsetattr(0,TCSANOW,&saved_ti); raw_on=0; } }

/* ---- utf8 helpers on the editor buffer ------------------------------------ */
static size_t prev_cp(const char *b, size_t off){
    if(off==0) return 0; size_t i=off-1;
    while(i>0 && ((unsigned char)b[i]&0xC0)==0x80) i--; return i; }
static size_t next_cp(const char *b, size_t len, size_t off){
    if(off>=len) return len; size_t l=mp_utf8_cp_bytes((unsigned char)b[off]);
    if(!l) l=1; off+=l; return off<len?off:len; }

static void buf_ensure(mp_editor *e, size_t need){
    if(need<=e->cap) return; size_t nc=e->cap?e->cap:256;
    while(nc<need) nc*=2; e->buf=realloc(e->buf,nc); e->cap=nc; }
static void buf_insert(mp_editor *e, const char *s, size_t n){
    buf_ensure(e, e->len+n+1);
    memmove(e->buf+e->cur+n, e->buf+e->cur, e->len-e->cur);
    memcpy(e->buf+e->cur, s, n); e->cur+=n; e->len+=n; e->buf[e->len]=0; }
static void buf_delete_range(mp_editor *e, size_t a, size_t b){
    if(a>b||b>e->len) return;
    memmove(e->buf+a, e->buf+b, e->len-b); e->len-=b-a;
    if(e->cur>b) e->cur=b; if(e->cur<a) e->cur=a; e->buf[e->len]=0; }

static size_t prev_word_start(const mp_editor *e){ size_t i=e->cur;
    while(i>0 && (e->buf[i-1]==' '||e->buf[i-1]=='\t')) i=prev_cp(e->buf,i);
    while(i>0){ size_t p=prev_cp(e->buf,i); unsigned char c=(unsigned char)e->buf[p];
        if(!(c=='_'||c>=0x80||isalnum(c))) break; i=p; } return i; }
static size_t next_word_end(const mp_editor *e){ size_t i=e->cur;
    while(i<e->len && (e->buf[i]==' '||e->buf[i]=='\t')) i=next_cp(e->buf,e->len,i);
    while(i<e->len){ unsigned char c=(unsigned char)e->buf[i];
        if(!(c=='_'||c>=0x80||isalnum(c))) break; i=next_cp(e->buf,e->len,i); } return i; }

mp_ed_action mp_ed_key(mp_editor *e, int key){
    switch(key){
    case '\r': case '\n': return MP_ED_SUBMIT;
    case 0x03: return MP_ED_INTERRUPT;                 /* Ctrl-C */
    case 0x04: if(!e->len) return MP_ED_QUIT;          /* Ctrl-D */
                 buf_delete_range(e,e->cur,next_cp(e->buf,e->len,e->cur)); break;
    case 0x01: case -5: e->cur=0; break;               /* Ctrl-A / Home */
    case 0x05: case -6: e->cur=e->len; break;          /* Ctrl-E / End */
    case 0x02: case -1: e->cur=prev_cp(e->buf,e->cur); break;
    case 0x06: case -2: e->cur=next_cp(e->buf,e->len,e->cur); break;
    case -8: e->cur=prev_word_start(e); break;         /* Ctrl-left */
    case -9: e->cur=next_word_end(e); break;           /* Ctrl-right */
    case 0x08: case 0x7F: { size_t p=prev_cp(e->buf,e->cur); if(p!=e->cur) buf_delete_range(e,p,e->cur); } break;
    case -7: buf_delete_range(e,e->cur,next_cp(e->buf,e->len,e->cur)); break;
    case 0x0B: { size_t k=e->cur; while(k<e->len&&e->buf[k]!='\n')k++;      /* Ctrl-K */
        snprintf(e->killbuf,sizeof e->killbuf,"%.*s",(int)(k-e->cur),e->buf+e->cur);
        buf_delete_range(e,e->cur,k); } break;
    case 0x15: { snprintf(e->killbuf,sizeof e->killbuf,"%.*s",(int)e->cur,e->buf); /* Ctrl-U */
        buf_delete_range(e,0,e->cur); } break;
    case 0x17: { size_t w=prev_word_start(e);                              /* Ctrl-W */
        snprintf(e->killbuf,sizeof e->killbuf,"%.*s",(int)(e->cur-w),e->buf+w);
        buf_delete_range(e,w,e->cur); } break;
    case 0x0C: break;                                                       /* Ctrl-L no-op */
    case 0x11: if(e->killbuf[0]) buf_insert(e,e->killbuf,strlen(e->killbuf)); break; /* Ctrl-Y yank */
    default:
        if(key>=0x20 && key<0x7F){ char c=(char)key; buf_insert(e,&c,1); }
        else if(key>=0x80 && key<=0xFF){ char c=(char)key; buf_insert(e,&c,1); }
        break;
    }
    return (mp_ed_action)0;
}

/* ---- editor lifecycle ------------------------------------------------------- */
mp_editor *mp_ed_new(const char *hist_file){
    mp_editor *e = calloc(1, sizeof *e);
    if(!e) return NULL;
    e->tty = isatty(0);
    e->hist_pos = -1;
    if (hist_file) snprintf(e->hist_file, sizeof e->hist_file, "%s", hist_file);
    buf_ensure(e, 256); e->buf[0] = 0;
    if (e->tty && *e->hist_file) mp_ed_hist_load(e);
    return e;
}
void mp_ed_free(mp_editor *e){
    if(!e) return;
    if(e->tty) raw_disable();
    for(int i=0;i<e->nhist;i++) free(e->hist[i]);
    free(e->hist); free(e->buf); free(e);
}
void mp_ed_hist_add(mp_editor *e, const char *text){
    if(!text||!*text) return;
    if(e->nhist && !strcmp(e->hist[e->nhist-1], text)) return;
    if(e->nhist>=500){
        free(e->hist[0]);
        for(int i=1;i<e->nhist;i++) e->hist[i-1]=e->hist[i];
        e->nhist--;
    }
    if(e->nhist>=e->hcap){ e->hcap=e->hcap?e->hcap*2:32; e->hist=realloc(e->hist,sizeof(char*)*(size_t)e->hcap); }
    e->hist[e->nhist++]=strdup(text);
}
int mp_ed_hist_load(mp_editor *e){
    if(!*e->hist_file) return -1;
    FILE *f=fopen(e->hist_file,"r"); if(!f) return -1;
    char line[8192];
    while(fgets(line,sizeof line,f)){ size_t l=strlen(line);
        while(l&&(line[l-1]=='\n'||line[l-1]=='\r'))line[--l]=0; if(l) mp_ed_hist_add(e,line); }
    fclose(f); return 0;
}
int mp_ed_hist_save(mp_editor *e){
    if(!*e->hist_file) return -1;
    FILE *f=fopen(e->hist_file,"w"); if(!f) return -1;
    for(int i=0;i<e->nhist;i++){ fputs(e->hist[i],f); fputc('\n',f); }
    return fclose(f);
}
static void hist_recall(mp_editor *e,int dir){
    if(!e->nhist) return;
    if(dir<0){
        if(e->hist_pos<0) e->hist_pos=e->nhist;
        if(e->hist_pos>0) e->hist_pos--;
    } else {
        if(e->hist_pos<0) return;
        if(e->hist_pos<e->nhist-1) e->hist_pos++;
        else { e->hist_pos=-1; e->len=0; e->cur=0; e->buf[0]=0; return; }
    }
    const char *h=e->hist[e->hist_pos];
    e->len=0; e->cur=0; buf_insert(e,h,strlen(h));
}

/* ---- rendering --------------------------------------------------------------- */
static void redraw(mp_editor *e, const char *prompt){
    if(!e->tty) return;
    fputs("\r\033[2K", stdout);
    fputs(prompt, stdout);
    fwrite(e->buf,1,e->len,stdout);
    size_t after = e->len - e->cur;
    if(after) printf("\033[%zuD", after);
    fflush(stdout);
}

/* read one decoded key; returns >=0 byte or negative pseudo-code, -100 EOF. */
static int read_key(void){
    unsigned char c; ssize_t r=read(0,&c,1);
    if(r<=0) return -100;
    if(c!=0x1B) return c;              /* incl. continuation bytes >=0x80 */
    struct timeval tv={0,60000}; fd_set fs;
    unsigned char b1,b2;
    FD_ZERO(&fs); FD_SET(0,&fs);
    if(select(1,&fs,NULL,NULL,&tv)<=0) return 0x1B;
    r=read(0,&b1,1); if(r<=0) return 0x1B;
    if(b1=='['){
        FD_ZERO(&fs); FD_SET(0,&fs);
        if(select(1,&fs,NULL,NULL,&tv)<=0) return 0x1B;
        if(read(0,&b2,1)!=1) return 0x1B;
        if(b2>='A'&&b2<='Z'){
            switch(b2){
            case 'A': return -3; case 'B': return -4;
            case 'C': return -2; case 'D': return -1;
            case 'H': return -5; case 'F': return -6;
            }
        }
        if(b2=='2'){ /* bracketed paste marker ESC[200~ / ESC[201~ */
            unsigned char x,y;
            if(read(0,&x,1)==1 && x=='0'){
                if(read(0,&y,1)==1){
                    if(y=='0' && read(0,&x,1)==1 && x=='~') return -10;
                    if(y=='1' && read(0,&x,1)==1 && x=='~') return -11;
                }
            }
            return 0x1B;
        }
        if(b2=='1'){ /* ESC[1;5D/C word ops */
            unsigned char x,y,z;
            if(read(0,&x,1)==1 && x==';' && read(0,&y,1)==1 && (y=='5'||y=='3') && read(0,&z,1)==1){
                if(z=='D') return -8; if(z=='C') return -9;
            }
            return 0x1B;
        }
        if(b2=='3'){ unsigned char x; if(read(0,&x,1)==1&&x=='~') return -7; return 0x1B; }
        return 0x1B;
    }
    if(b1=='O'){ if(read(0,&b2,1)==1){ if(b2=='H')return -5; if(b2=='F')return -6; } return 0x1B; }
    if(b1=='b'||b1=='B') return -8;   /* Alt-b */
    if(b1=='f'||b1=='F') return -9;   /* Alt-f */
    return 0x1B;
}

mp_ed_action mp_ed_readline(mp_editor *e, const char *prompt, char **out){
    *out=NULL;
    e->len=0; e->cur=0; if(e->buf) e->buf[0]=0; e->hist_pos=-1;
    if(!e->tty){ /* plain pipe mode: no escape handling at all */
        fputs(prompt,stdout); fflush(stdout);
        char *lb=NULL; size_t lc=0; ssize_t n=getline(&lb,&lc,stdin);
        if(n<0){ free(lb); return MP_ED_QUIT; }
        while(n&&(lb[n-1]=='\n'||lb[n-1]=='\r')) lb[--n]=0;
        *out=lb; return MP_ED_SUBMIT;
    }
    raw_enable();
    fputs("\033[?2004h", stdout);   /* bracketed paste on */
    redraw(e,prompt);
    int in_paste = 0;
    for(;;){
        int k=read_key();
        if(k==-100){ fputs("\033[?2004l",stdout); raw_disable(); return MP_ED_QUIT; }
        if(k==-10){ in_paste=1; continue; }
        if(k==-11){ in_paste=0; redraw(e,prompt); continue; }
        if(k==0x1B){ continue; }    /* lone ESC: ignore (Esc handled by app via signal path) */
        if(!in_paste && k==-3){ hist_recall(e,-1); redraw(e,prompt); continue; }
        if(!in_paste && k==-4){ hist_recall(e,+1); redraw(e,prompt); continue; }
        /* newlines inside paste become literal \n in buffer (multiline input) */
        if(in_paste && (k=='\r'||k=='\n')){ buf_insert(e,"\n",1); redraw(e,prompt); continue; }
        mp_ed_action a = mp_ed_key(e,k);
        if(a==MP_ED_SUBMIT){
            if(!in_paste && e->len>=1 && e->buf[e->len-1]=='\\'){ /* multiline cont */
                buf_delete_range(e,e->len-1,e->len);
                buf_insert(e,"\n",1);
                redraw(e,prompt); continue;
            }
            fputs("\033[?2004l",stdout);
            fputs("\r\n",stdout);
            raw_disable();
            char *res=strdup(e->buf?e->buf:"");
            e->len=0; e->cur=0; if(e->buf)e->buf[0]=0;
            *out=res; return MP_ED_SUBMIT;
        }
        if(a==MP_ED_INTERRUPT){ fputs("\033[?2004l",stdout); fputs("^C\r\n",stdout);
            raw_disable(); e->len=0;e->cur=0;if(e->buf)e->buf[0]=0; return MP_ED_INTERRUPT; }
        if(a==MP_ED_QUIT){ fputs("\033[?2004l",stdout); fputs("\r\n",stdout); raw_disable(); return MP_ED_QUIT; }
        redraw(e,prompt);
    }
}
