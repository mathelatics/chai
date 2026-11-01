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
