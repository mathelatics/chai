#include "diff.h"
#include <stdlib.h>
#include <string.h>
/* Classic LCS DP over lines; fine for file-edit previews (<~2k lines). */
int mp_diff_lines(const char **a,int na,const char **b,int nb,mp_diff_hunk *out,int max){
    if(na<0||nb<0||!a||!b||!out)return -1;
    size_t cells=(size_t)(na+1)*(nb+1);
    if(cells*2 > 64u*1024*1024) return -1;                 /* cap: 64MB table */
    unsigned short *dp=calloc(cells,sizeof(*dp)); if(!dp)return -1;
    #define L(i,j) dp[(size_t)(i)*(nb+1)+(j)]
    for(int i=na-1;i>=0;i--)for(int j=nb-1;j>=0;j--)
        L(i,j)= !strcmp(a[i],b[j]) ? L(i+1,j+1)+1 : (L(i+1,j)>=L(i,j+1)?L(i+1,j):L(i,j+1));
    int i=0,j=0,n=0;
    while(i<na&&j<nb){
        if(!strcmp(a[i],b[j])){ if(n>=max)goto overflow; out[n++]=(mp_diff_hunk){MP_DIFF_CTX,i+1,j+1,a[i]}; i++;j++; }
        else if(L(i+1,j)>=L(i,j+1)){ if(n>=max)goto overflow; out[n++]=(mp_diff_hunk){MP_DIFF_DEL,i+1,0,a[i]}; i++; }
        else { if(n>=max)goto overflow; out[n++]=(mp_diff_hunk){MP_DIFF_ADD,0,j+1,b[j]}; j++; }
    }
    while(i<na){ if(n>=max)goto overflow; out[n++]=(mp_diff_hunk){MP_DIFF_DEL,i+1,0,a[i]}; i++; }
    while(j<nb){ if(n>=max)goto overflow; out[n++]=(mp_diff_hunk){MP_DIFF_ADD,0,j+1,b[j]}; j++; }
    free(dp); return n;
overflow: free(dp); return -1;
}
