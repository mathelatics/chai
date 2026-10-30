#include "uuid.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
static int rnd(unsigned char *b,size_t n){
#ifdef __linux__
    ssize_t got=getrandom(b,n,0); if(got==(ssize_t)n)return 0;
#endif
    int fd=open("/dev/urandom",O_RDONLY); if(fd<0)return -1;
    size_t off=0; while(off<n){ssize_t r=read(fd,b+off,n-off); if(r<=0){close(fd);return -1;}off+=(size_t)r;}
    close(fd);return 0;
}
int mp_uuid_v4(char out[37]){
    unsigned char b[16]; if(rnd(b,16))return -1;
    b[6]=(b[6]&0x0f)|0x40; b[8]=(b[8]&0x3f)|0x80;
    snprintf(out,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return 0;
}
