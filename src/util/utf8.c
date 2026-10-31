#include "utf8.h"
#include <string.h>
size_t mp_utf8_cp_bytes(unsigned char c){
    if(c<0x80)return 1; if(c>=0xC2&&c<=0xDF)return 2;
    if(c>=0xE0&&c<=0xEF)return 3; if(c>=0xF0&&c<=0xF4)return 4; return 0; }
static int valid_seq(const unsigned char *p,size_t n,size_t *used){
    if(!n)return 0;
    size_t l=mp_utf8_cp_bytes(p[0]);
    if(!l||l>n)return 0;
    unsigned cp=p[0]&((1<<(8-l))-1);
    for(size_t i=1;i<l;i++){ if((p[i]&0xC0)!=0x80)return 0; cp=(cp<<6)|(p[i]&0x3F); }
    if(l==2&&cp<0x80)return 0; if(l==3&&cp<0x800)return 0; if(l==4&&cp<0x10000)return 0;
    if(cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF))return 0;
    *used=l;return 1;
}
int mp_utf8_valid(const char *s,size_t n){
    const unsigned char *p=(const unsigned char*)s; size_t i=0;
    while(i<n){ size_t used; if(p[i]==0)return 0;
        if(!valid_seq(p+i,n-i,&used))return 0; i+=used; }
    return 1;
}
size_t mp_utf8_len(const char *s,size_t n){
    const unsigned char *p=(const unsigned char*)s; size_t i=0,cps=0;
    while(i<n){size_t used;if(!valid_seq(p+i,n-i,&used))return 0;i+=used;cps++;}
    return cps;
}

/* ---- streaming sanitizer (C-1) ------------------------------------------- */
#define REPL "\xEF\xBF\xBD"   /* U+FFFD */
static int fits(size_t need, size_t cap, size_t used){ return need <= cap - used; }
static void putb(char *out, size_t *used, unsigned char c){ out[(*used)++]= (char)c; }
static void puts_(char *out, size_t *used, const char *s, size_t n){
    memcpy(out + *used, s, n); *used += n; }

void mp_utf8_san_init(mp_utf8_san *s){ memset(s,0,sizeof *s); }

/* consumes one byte into escape-state machine; returns 1 if byte was part of
 * (or terminated) an escape sequence, 0 if it is ordinary text. */
static int esc_feed(mp_utf8_san *s, unsigned char c){
    switch(s->esc){
    case 0:
        if(c==0x1B){ s->esc=1; return 1; }
        return 0;
    case 1: /* after ESC */
        if(c=='['){ s->esc=2; return 1; }
        if(c=='P'||c=='X'||c=='^'||c==']'){ s->esc=4; s->osc_term=0; return 1; } /* sos/osc/pm/apc */
        if(c=='\\'){ s->esc=0; return 1; }                    /* ST terminates escape */
        if((c>='@'&&c<='_')||(c>='f'&&c<='n')||c=='\''||c=='('||c==')'||c=='-'||c=='.'){
            s->esc = (c=='('||c==')'||c=='-'||c=='.') ? 5 : 0; return 1; } /* charset: one more byte */
        s->esc=0; return 1; /* unknown: swallow ESC+c */
    case 2: /* CSI parameter bytes 0x30-0x3F */
        if(c>=0x30&&c<=0x3F) return 1;
        if(c>=0x20&&c<=0x2F){ s->esc=3; return 1; }              /* intermediates */
        if(c>=0x40&&c<=0x7E){ s->esc=0; return 1; }              /* final byte */
        s->esc=0; return 1;                                       /* bogus: drop seq */
    case 3: /* CSI intermediates, wait for final 0x40-0x7E */
        if(c>=0x20&&c<=0x2F) return 1;
        s->esc=0; if(c>=0x40&&c<=0x7E) return 1; return 1;
    case 4: /* OSC/DCS string until BEL or ST(ESC \) */
        if(s->osc_term){ s->osc_term=0; if(c=='\\'){ s->esc=0; return 1; }
            /* ESC inside OSC: treat as new escape start */ s->esc=1; return 1; }
        if(c==0x07){ s->esc=0; return 1; }
        if(c==0x1B){ s->osc_term=1; return 1; }
        return 1;
    case 5: /* charset designator final byte */
        s->esc=0; return 1;
    }
    s->esc=0; return 0;
}

static int sanitize_run(mp_utf8_san *s, const unsigned char *p, size_t n,
                        char *out, size_t cap, size_t *used, int final_){
    size_t i=0;
    /* first, try to complete carried partial sequence */
    if(s->carry_len){
        while(i<n && s->carry_len<4){
            s->carry[s->carry_len++]=p[i++];
            size_t want=mp_utf8_cp_bytes(s->carry[0]);
            if(want && s->carry_len==want){
                size_t v; if(valid_seq(s->carry,s->carry_len,&v)){
                    if(!fits(s->carry_len,cap,*used))return -1;
                    puts_(out,used,(char*)s->carry,s->carry_len);
                } else if(!fits(3,cap,*used)) return -1; else puts_(out,used,REPL,3);
                s->carry_len=0; break;
            }
        }
        if(s->carry_len && !final_) return 0;   /* still incomplete, wait for more */
        if(s->carry_len){ /* truncated at EOS */
            if(!fits(3,cap,*used)) return -1; puts_(out,used,REPL,3); s->carry_len=0;
        }
    }
    while(i<n){
        unsigned char c=p[i];
        if(esc_feed(s,c)){ i++; continue; }
        if(c<0x20 && c!='\n' && c!='\t'){ i++; continue; }   /* strip C0 */
        if(c==0x7F){ i++; continue; }
        if(c<0x80){ if(!fits(1,cap,*used))return -1; putb(out,used,c); i++; continue; }
        size_t want=mp_utf8_cp_bytes(c);
        if(!want){ if(!fits(3,cap,*used))return -1; puts_(out,used,REPL,3); i++; continue; }
        if(want>n-i){ /* possibly truncated at chunk boundary */
            if(!final_){ while(i<n && s->carry_len<4) s->carry[s->carry_len++]=p[i++]; return 0; }
            if(!fits(3,cap,*used))return -1; puts_(out,used,REPL,3); i++; continue;
        }
        size_t v;
        if(valid_seq(p+i,want,&v)){ if(!fits(v,cap,*used))return -1; puts_(out,used,(char*)p+i,v); i+=v; }
        else { if(!fits(3,cap,*used))return -1; puts_(out,used,REPL,3); i++; }
    }
    return 0;
}

int mp_utf8_sanitize(mp_utf8_san *s, const char *in, size_t n,
                     char *out, size_t cap, size_t *used){
    size_t u = used ? *used : 0;
    int r = sanitize_run(s,(const unsigned char*)in,n,out,cap,&u,0);
    if(used)*used=u;
    return r;
}

void mp_utf8_flush(mp_utf8_san *s, char *out, size_t cap, size_t *used){
    size_t u = used ? *used : 0;
    if(s->carry_len){ if(fits(3,cap,u)) puts_(out,&u,REPL,3); s->carry_len=0; }
    if(s->esc){ s->esc=0; s->osc_term=0; }   /* dangling escape seq: drop silently */
    if(used)*used=u;
}
