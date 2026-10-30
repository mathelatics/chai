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
    /* 4-char quantum decoder: skip whitespace, stop at padding. */
    size_t o=0,i=0;
    while(i<n){
        unsigned char q[4]; int qi=0;
        while(i<n&&qi<4){
            char c=in[i++];
            if(c=='\n'||c=='\r'||c==' '||c=='\t')continue;
            if(c=='=')break;
            int v=val((unsigned char)c); if(v<0)return -1;
            q[qi++]=(unsigned char)v;
        }
        if(qi==0)break;                 /* trailing pad/whitespace only */
        if(qi==1)return -1;             /* lone char is invalid */
        uint32_t acc=((uint32_t)q[0]<<18)|((uint32_t)q[1]<<12)|
                     ((qi>2?(uint32_t)q[2]:0)<<6)|((qi>3?(uint32_t)q[3]:0));
        int bytes=(qi==4)?3:(qi==3?2:1);
        for(int b=16;b>=0&&bytes>0;b-=8,bytes--){
            if(o>=cap)return -1;
            out[o++]=(unsigned char)((acc>>b)&0xFF);
        }
        if(qi<4)break;                  /* padded quantum ends the data */
    }
    return (int)o;
}
