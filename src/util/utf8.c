#include "utf8.h"
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
