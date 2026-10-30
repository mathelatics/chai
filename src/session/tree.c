#include "tree.h"
#include <stdlib.h>
#include <string.h>
static char *dz(const char *s){ if(!s)return NULL; size_t n=strlen(s)+1; char*b=malloc(n); if(b)memcpy(b,s,n); return b; }
void mp_tree_init(mp_tree*t){ memset(t,0,sizeof*t); t->idx=kh_init(sess_idx); t->leaf=-1; }
int mp_tree_add(mp_tree*t,const char*id,const char*parent,const char*role,const char*content){
    if(!t||!id||!role)return -1;
    if((size_t)t->n>=65536)return -1;
    if(t->n==t->cap){int nc=t->cap?t->cap*2:32;mp_entry*v=realloc(t->v,(size_t)nc*sizeof*v);if(!v)return -1;t->v=v;t->cap=nc;}
    int ins=0; khint_t k=kh_put(sess_idx,t->idx,id,&ins);   /* khash copies the key string */
    if(!ins)return -1;                                        /* duplicate id */
    kh_val(t->idx,k)=t->n;
    t->v[t->n]=(mp_entry){dz(id),dz(parent),dz(role),dz(content)};
    t->leaf=t->n; return t->n++;
}
const mp_entry *mp_tree_get(mp_tree*t,const char*id){
    khint_t k=kh_get(sess_idx,t->idx,id);
    return k==kh_end(t->idx)?NULL:&t->v[kh_value(t->idx,k)];
}
int mp_tree_path(mp_tree*t,const mp_entry**out,int max){
    if(!t||!out||t->leaf<0)return 0;
    const mp_entry *stack[256]; int n=0, cur=t->leaf, guard=0;
    while(cur>=0&&n<256&&guard++<65536){ stack[n++]=&t->v[cur];
        const char*p=t->v[cur].parent; if(!p)break;
        khint_t k=kh_get(sess_idx,t->idx,p); if(k==kh_end(t->idx))break; cur=kh_value(t->idx,k); }
    int w=0; for(int i=n-1;i>=0&&w<max;i--) out[w++]=stack[i];
    return w;
}
void mp_tree_free(mp_tree*t){ if(!t)return;
    for(int i=0;i<t->n;i++){free(t->v[i].id);free(t->v[i].parent);free(t->v[i].role);free(t->v[i].content);}
    free(t->v); kh_destroy(sess_idx,t->idx); memset(t,0,sizeof*t); }
