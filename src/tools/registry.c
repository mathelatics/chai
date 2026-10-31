#include "registry.h"
#include "tools/output_cap.h"
#include "util/schema.h"
#include "util/utf8.h"
#include "khash.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <sys/time.h>
#include <unistd.h>
KHASH_MAP_INIT_STR(toolmap, int)
typedef struct { char *name; cJSON *schema; mp_tool_fn fn; void *ud; } tool_ent;
struct mp_toolreg { tool_ent *v; int n, cap; khash_t(toolmap) *idx; };
extern char *mp_tool_read(const cJSON*,void*); extern char *mp_tool_write(const cJSON*,void*);
extern char *mp_tool_edit(const cJSON*,void*); extern char *mp_tool_bash(const cJSON*,void*);
extern const char *MP_SCHEMA_READ, *MP_SCHEMA_WRITE, *MP_SCHEMA_EDIT, *MP_SCHEMA_BASH;
static char *dupz(const char *s){ size_t n=strlen(s)+1; char*b=malloc(n); if(b)memcpy(b,s,n); return b; }
mp_toolreg *mp_toolreg_new_(void){
    mp_toolreg *r=calloc(1,sizeof*r); r->idx=kh_init(toolmap); return r; }
int mp_toolreg_add(mp_toolreg *r,const char*name,const cJSON*schema,mp_tool_fn fn,void*ud){
    if(!r||!name||!fn)return -1;
    if((size_t)r->n>=4096)return -1;
    if(r->n==r->cap){int nc=r->cap?r->cap*2:8;tool_ent*v=realloc(r->v,(size_t)nc*sizeof*v);if(!v)return -1;r->v=v;r->cap=nc;}
    char *dn=dupz(name); if(!dn)return -1;
    int inserted=0;
    khint_t kk=kh_put(toolmap,r->idx,dn,&inserted);
    if(!inserted){ free(dn); return -1; } /* duplicate name */
    if(!kh_exist(r->idx,kk)){ free(dn); return -1; }
    kh_val(r->idx,kk)=r->n;
    r->v[r->n]=(tool_ent){dn,schema?schema:cJSON_Parse("{}"),fn,ud};
    r->n++; return 0;
}
char *mp_tool_execute(mp_toolreg *r,const char*name,const cJSON*args){
    if(!r||!name)return NULL;
    khint_t k=kh_get(toolmap,r->idx,name);
    if(k==kh_end(r->idx))return strdup("error: unknown tool");
    tool_ent *e=&r->v[kh_value(r->idx,k)];
    if(!args)args=cJSON_CreateObject();
    char *err=mp_schema_validate(e->schema,args);
    if(err){char *m=malloc(strlen(err)+32); if(m)sprintf(m,"error: invalid arguments: %s",err); free(err); return m;}
    /* reject invalid UTF-8 in any string arg */
    const cJSON *it=NULL;
    cJSON_ArrayForEach(it,args) if(cJSON_IsString(it)&&!mp_utf8_valid(it->valuestring,strlen(it->valuestring)))
        return strdup("error: invalid UTF-8 in arguments");
    char *raw = e->fn(args,e->ud);
    /* C-2: cap tool output BEFORE it re-enters model context (head+tail keep,
     * explicit elision marker). NULL-safe: on alloc failure keep raw. */
    if (raw) { char *capped = mp_output_cap(raw, strlen(raw), 0, 0, 0);
               if (capped) { free(raw); return capped; } }
    return raw;
}
cJSON *mp_toolreg_schemas(const mp_toolreg *r){
    cJSON *arr=cJSON_CreateArray();
    for(int i=0;i<r->n;i++){cJSON*o=cJSON_CreateObject();
        cJSON_AddStringToObject(o,"name",r->v[i].name);
        cJSON_AddItemToObject(o,"schema",cJSON_Duplicate(r->v[i].schema,1));
        cJSON_AddItemToArray(arr,o);}
    return arr;
}
void mp_toolreg_free(mp_toolreg*r){ if(!r)return;
    for(int i=0;i<r->n;i++){free(r->v[i].name);cJSON_Delete(r->v[i].schema);}
    kh_destroy(toolmap,r->idx); free(r->v); free(r); }
char *mp_path_confine(const char *root,const char*p){
    if(!root||!p||!*p)return NULL;
    char joined[PATH_MAX*2]; if(snprintf(joined,sizeof joined,"%s/%s",root,p)>=(int)sizeof joined)return NULL;
    char buf[PATH_MAX]; if(!realpath(joined,buf))return NULL;
    size_t rl=strlen(root);
    if(strncmp(buf,root,rl)!=0||(buf[rl]!='/'&&buf[rl]!='\0'))return NULL;
    return dupz(buf);
}
mp_toolreg *mp_toolreg_default(void){
    mp_toolreg *r=mp_toolreg_new_();
    const char *root=getenv("MINIPI_WORKSPACE"); static char wsbuf[PATH_MAX];
    if(!root){ if(!getcwd(wsbuf,sizeof wsbuf))strcpy(wsbuf,"."); root=wsbuf; }
    char *rootc=dupz(root);
    mp_toolreg_add(r,"read",cJSON_Parse(MP_SCHEMA_READ),(mp_tool_fn)mp_tool_read,rootc);
    mp_toolreg_add(r,"write",cJSON_Parse(MP_SCHEMA_WRITE),mp_tool_write,rootc);
    mp_toolreg_add(r,"edit",cJSON_Parse(MP_SCHEMA_EDIT),mp_tool_edit,rootc);
    mp_toolreg_add(r,"bash",cJSON_Parse(MP_SCHEMA_BASH),mp_tool_bash,rootc);
    return r;
}
