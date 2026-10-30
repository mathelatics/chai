#include "sha256.h"
#include <string.h>
static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x23048d60,0x2b94b74b,0x33b48b60,0x448ab664,0x793601a1,0x2be3af4e,
0x3bd30c95,0x536844d6,0x60f30981,0x7140842d,0x897a0e95,0x9cd4ea8e,0xbdbf4c8e,0xdff8ba58 };
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void compress(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64], a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i=0;i<16;i++) w[i]=((uint32_t)p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
    for (int i=16;i<64;i++){uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3),
        s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1;}
    for (int i=0;i<64;i++){uint32_t S1=ROR(e,6)^ROR(e,11)^ROR(e,25),
        ch=(e&f)^((~e)&g),t1=hh+S1+ch+K[i]+w[i],
        S0=ROR(a,2)^ROR(a,13)^ROR(a,22),maj=(a&b)^(a&c)^(b&c),t2=S0+maj;
        hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
void mp_sha256_init(mp_sha256 *h){ h->st[0]=0x6a09e667;h->st[1]=0xbb67ae85;h->st[2]=0x3c6ef372;
    h->st[3]=0xa54ff53a;h->st[4]=0x510e527f;h->st[5]=0x9b05688c;h->st[6]=0x1f83d9ab;h->st[7]=0x5be0cd19;
    h->len=0;h->buflen=0; }
void mp_sha256_update(mp_sha256 *h,const void*data,size_t n){
    const unsigned char *p=data; h->len+=n;
    while(n){size_t take=64-h->buflen; if(take>n)take=n; memcpy(h->buf+h->buflen,p,take);
        h->buflen+=take;p+=take;n-=take;
        if(h->buflen==64){compress(h->st,h->buf);h->buflen=0;}} }
void mp_sha256_final(mp_sha256 *h,unsigned char out[32]){
    uint64_t bits=h->len*8;
    /* pad manually (do NOT touch h->len, which counts message bytes only) */
    size_t bl=h->buflen;
    h->buf[bl++]=0x80;
    if(bl>56){ while(bl<64)h->buf[bl++]=0; compress(h->st,h->buf); bl=0; }
    while(bl<56)h->buf[bl++]=0;
    for(int i=0;i<8;i++)h->buf[56+i]=(unsigned char)(bits>>(56-8*i));
    compress(h->st,h->buf);
    for(int i=0;i<8;i++){out[i*4]=h->st[i]>>24;out[i*4+1]=h->st[i]>>16;out[i*4+2]=h->st[i]>>8;out[i*4+3]=h->st[i];}
}
void mp_sha256_hex(const void *data,size_t n,char hx[65]){
    mp_sha256 h; unsigned char d[32]; mp_sha256_init(&h); mp_sha256_update(&h,data,n); mp_sha256_final(&h,d);
    static const char *H="0123456789abcdef";
    for(int i=0;i<32;i++){hx[i*2]=H[d[i]>>4];hx[i*2+1]=H[d[i]&15];} hx[64]='\0';
}
