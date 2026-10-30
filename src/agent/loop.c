#include "loop.h"
#include "tools/registry.h"
#include "providers/provider.h"
#include <stdlib.h>
#include <string.h>
struct mp_agent { mp_agent_cfg cfg; mp_bus *bus; mp_ctx ctx; mp_toolreg *tools; };
mp_agent *mp_agent_new(const mp_agent_cfg *cfg, mp_bus *bus){
    mp_agent *a=calloc(1,sizeof*a); if(!a)return NULL;
    a->cfg=*cfg; a->bus=bus; mp_ctx_init(&a->ctx);
    a->tools=mp_toolreg_default();
    mp_ctx_push(&a->ctx,"system","You are MiniPi, a careful coding agent. Use tools to act.",NULL);
    return a; }
void mp_agent_free(mp_agent *a){ if(!a)return; mp_ctx_free(&a->ctx); mp_toolreg_free(a->tools); free(a); }
mp_ctx *mp_agent_ctx(mp_agent *a){ return &a->ctx; }
mp_bus *mp_agent_bus(mp_agent *a){ return a?a->bus:NULL; }
int mp_agent_run_turn(mp_agent *a, const char *user_text){
    if(!a||!user_text)return -1;
    mp_event ev={MP_EV_USER_MSG,user_text,NULL}; mp_bus_emit(a->bus,&ev);
    mp_ctx_push(&a->ctx,"user",user_text,NULL);
    for(int turn=0;turn<a->cfg.max_turns;turn++){
        mp_reply *r=mp_provider_complete(a->cfg.model,&a->ctx); /* M1 stub => deterministic echo */
        if(!r)return -2;
        mp_event d={MP_EV_ASSISTANT_DELTA,r->text,NULL}; mp_bus_emit(a->bus,&d);
        if(r->tool_calls){
            const cJSON *tc=NULL;
            cJSON_ArrayForEach(tc,r->tool_calls){
                const cJSON*n=cJSON_GetObjectItemCaseSensitive(tc,"name");
                const cJSON*args=cJSON_GetObjectItemCaseSensitive(tc,"arguments");
                mp_event tcev={MP_EV_TOOL_CALL,n&&cJSON_IsString(n)?n->valuestring:"?",args};
                mp_bus_emit(a->bus,&tcev);
                char *res=mp_tool_execute(a->tools,n&&cJSON_IsString(n)?n->valuestring:"",args);
                mp_event rev={MP_EV_TOOL_RESULT,res,NULL}; mp_bus_emit(a->bus,&rev);
                mp_ctx_push(&a->ctx,"tool",res?res:"(error)",cJSON_Duplicate(tc,1));
                free(res);
            }
        }
        mp_ctx_push(&a->ctx,"assistant",r->text?r->text:"",NULL);
        mp_event dn={MP_EV_ASSISTANT_DONE,r->text,NULL}; mp_bus_emit(a->bus,&dn);
        int done=!r->tool_calls; mp_reply_free(r);
        if(done)break;
    }
    mp_event te={MP_EV_TURN_END,NULL,NULL}; mp_bus_emit(a->bus,&te);
    return 0;
}
