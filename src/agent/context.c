#include "context.h"
#include <stdlib.h>
#include <string.h>
static int grow(mp_ctx*c){int nc=c->cap?c->cap*2:8;
    mp_msg*v=realloc(c->v,(size_t)nc*sizeof*v);if(!v)return -1;c->v=v;c->cap=nc;return 0;}
void mp_ctx_init(mp_ctx*c){memset(c,0,sizeof*c);}
void mp_ctx_free(mp_ctx*c){for(int i=0;i<c->n;i++){free(c->v[i].role);free(c->v[i].content);
    if(c->v[i].tool_call)cJSON_Delete(c->v[i].tool_call);}free(c->v);memset(c,0,sizeof*c);}
void mp_ctx_push(mp_ctx*c,const char*role,const char*content,cJSON*tc){
    if(grow(c))return;
    c->v[c->n].role=strdup(role); c->v[c->n].content=content?strdup(content):strdup("");
    c->v[c->n].tool_call=tc; c->n++;
    c->est_tokens += content?(long)(strlen(content)/4):0; }
cJSON *mp_ctx_to_json(const mp_ctx*c){
    cJSON *arr=cJSON_CreateArray();
    for(int i=0;i<c->n;i++){cJSON *o=cJSON_CreateObject();
        cJSON_AddStringToObject(o,"role",c->v[i].role);
        cJSON_AddStringToObject(o,"content",c->v[i].content);
        if(c->v[i].tool_call)cJSON_AddItemToObject(o,"tool_call",cJSON_Duplicate(c->v[i].tool_call,1));
        cJSON_AddItemToArray(arr,o);}
    return arr; }
int mp_ctx_from_json(mp_ctx*c,const cJSON*arr){
    if(!cJSON_IsArray(arr))return -1; const cJSON*m=NULL;
    cJSON_ArrayForEach(m,arr){const cJSON*r=cJSON_GetObjectItemCaseSensitive(m,"role"),
        *ct=cJSON_GetObjectItemCaseSensitive(m,"content"),
        *tc=cJSON_GetObjectItemCaseSensitive(m,"tool_call");
        if(!cJSON_IsString(r))return -1;
        mp_ctx_push(c,r->valuestring,cJSON_IsString(ct)?ct->valuestring:"",
                    tc?cJSON_Duplicate(tc,1):NULL);}
    return 0; }
