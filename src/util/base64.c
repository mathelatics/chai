#include "base64.h"
#include <stdint.h>
static const char T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int mp_b64_encode(const unsigned char *in,size_t n,char *out,size_t cap){
    size_t need=((n+2)/3)*4; if(cap<need+1)return -1;
    size_t o=0,i=0;
    for(;i+2<n;i+=3){uint32_t v=(in[i]<<16)|(in[i+1]<<8)|in[i+2];
        out[o++]=T[v>>18];out[o++]=T[(v>>12)&63];out[o++]=T[(v>>6)&63];out[o++]=T[v&63];}
    if(i<n){uint32_t v=in[i]<<16; if(i+1<n)v|=in[i+1]<<8;
        out[o++]=T[v>>18];out[o++]=T[(v>>12)&63];
        out[o++]=(i+1<n)?T[(v>>6)&63]:'=';out[o++]='=';}
    out[o]='\0';return (int)o;
}
static int val(unsigned char c){
    if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26;
    if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1;}
int mp_b64_decode(const char *in,size_t n,unsigned char *out,size_t cap){
    size_t o=0; uint32_t acc=0; int bits=0;
    for(size_t i=0;i<n;i++){
        char c=in[i]; if(c=='\n'||c=='\r'||c==' ')continue;
        if(c=='='){if(bits>=18)return -1;acc<<=6;bits+=6;
            if(bits>=8){bits-=8;if(o>=cap)return -1;out[o++]=(acc>>bits)&0xFF;}continue;}
        int v=val((unsigned char)c); if(v<0)return -1;
        acc=(acc<<6)|v;bits+=6;
        if(bits>=8){bits-=8;if(o>=cap)return -1;out[o++]=(acc>>bits)&0xFF;}}
    if(bits>7)return -1; return (int)o;
}
