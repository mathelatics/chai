#include "loop.h"
#include "tools/registry.h"
#include "providers/provider.h"
#include "security/policy.h"
#include "config/config.h"
#include "net/broker_client.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern mp_config g_cfg;   /* process-wide provider config (defined in tui/repl.c) */

/* Route one completion through the live provider when a cloud/local provider is
 * active; fall back to the deterministic offline stub otherwise. Keeps the
 * --prompt one-shot path honest: same wire, same schemas, no interactive
 * streamer (deltas are swallowed here; final text is emitted via the bus). */
static mp_reply *complete_routed(const char *model, mp_ctx *ctx, mp_toolreg *tools) {
    mp_provider *prov = mp_cfg_active(&g_cfg);
    if (!prov)
        return mp_provider_complete(model ? model : "offline-stub", ctx);
    const char *m = g_cfg.session_model[0] ? g_cfg.session_model : prov->model;
    cJSON *schemas = tools ? mp_toolreg_schemas(tools) : NULL;
    char host[256]; const char *local_base = NULL, *broker_host = NULL;
    if (prov->kind == MP_KIND_OPENAI_LOCAL) {
        local_base = prov->base_url;
    } else {
        const char *p = strstr(prov->base_url, "://");
        if (p) {
            p += 3; const char *e = strchr(p, '/');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len && len < sizeof host) { memcpy(host, p, len); host[len] = 0; broker_host = host; }
        }
        if (!broker_host) { snprintf(host, sizeof host, "%s", "integrate.api.nvidia.com"); broker_host = host; }
    }
    mp_broker_set_key_env(prov->key_env[0] ? prov->key_env : "NVIDIA_API_KEY");
    mp_reply *r = mp_nemotron_stream_ex(m, ctx, 0, 4096, 0.2, 0.95, NULL, NULL,
                                        schemas, local_base, broker_host);
    if (schemas) cJSON_Delete(schemas);
    return r;
}

struct mp_agent { mp_agent_cfg cfg; mp_bus *bus; mp_ctx ctx; mp_toolreg *tools; };

/* live-route accessors used by the REPL (streaming provider path) */
const char *mp_agent_model(mp_agent *a){ return a?a->cfg.model:NULL; }
int  mp_agent_max_turns(mp_agent *a){ return a?(a->cfg.max_turns>0?a->cfg.max_turns:8):8; }
long mp_agent_token_budget(mp_agent *a){ return a?a->cfg.token_budget:0; }

mp_agent *mp_agent_new(const mp_agent_cfg *cfg, mp_bus *bus){
    mp_agent *a=calloc(1,sizeof*a); if(!a)return NULL;
    a->cfg=*cfg; a->bus=bus; mp_ctx_init(&a->ctx);
    a->tools=mp_toolreg_default();
    mp_ctx_push(&a->ctx,"system","You are MiniPi, a careful coding agent. Use tools to act.",NULL);
    return a; }
void mp_agent_free(mp_agent *a){ if(!a)return; mp_ctx_free(&a->ctx); /* tools owned by caller in _pol path? default reg is ours */ if(a->tools)mp_toolreg_free(a->tools); free(a); }
mp_ctx *mp_agent_ctx(mp_agent *a){ return &a->ctx; }
mp_toolreg *mp_agent_tools(mp_agent *a){ return a?a->tools:NULL; }
mp_bus *mp_agent_bus(mp_agent *a){ return a?a->bus:NULL; }

static const cJSON *call_name(const cJSON *tc){ return cJSON_GetObjectItemCaseSensitive(tc,"name"); }
static const cJSON *call_args(const cJSON *tc){ return cJSON_GetObjectItemCaseSensitive(tc,"arguments"); }

int mp_agent_run_turn(mp_agent *a, const char *user_text){
    if(!a||!user_text)return -1;
    mp_event ev={MP_EV_USER_MSG,user_text,NULL}; mp_bus_emit(a->bus,&ev);
    mp_ctx_push(&a->ctx,"user",user_text,NULL);
    for(int turn=0;turn<a->cfg.max_turns;turn++){
        mp_reply *r=complete_routed(a->cfg.model,&a->ctx,a->tools);
        if(!r)return -2;
        mp_event d={MP_EV_ASSISTANT_DELTA,r->text,NULL}; mp_bus_emit(a->bus,&d);
        if(r->tool_calls){
            const cJSON *tc=NULL;
            cJSON_ArrayForEach(tc,r->tool_calls){
                const cJSON*n=call_name(tc); const cJSON*args=call_args(tc);
                mp_event tcev={MP_EV_TOOL_CALL,n&&cJSON_IsString(n)?n->valuestring:"?",args};
                mp_bus_emit(a->bus,&tcev);
                /* No policy supplied => fail closed: refuse to run tools. */
                const char *msg="[blocked] no permission policy attached; tool was not executed";
                mp_event rev={MP_EV_TOOL_RESULT,msg,NULL}; mp_bus_emit(a->bus,&rev);
                mp_ctx_push(&a->ctx,"tool",msg,cJSON_Duplicate(tc,1));
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

int mp_agent_run_turn_pol(mp_agent *a, const char *user_text, mp_policy *pol,
                          mp_toolreg *tools){
    if(!a||!user_text)return -1;
    if(!tools)tools=a->tools;
    mp_event ev={MP_EV_USER_MSG,user_text,NULL}; mp_bus_emit(a->bus,&ev);
    mp_ctx_push(&a->ctx,"user",user_text,NULL);
    for(int turn=0;turn<(a->cfg.max_turns>0?a->cfg.max_turns:8);turn++){
        mp_reply *r=complete_routed(a->cfg.model,&a->ctx,tools);
        if(!r)return -2;
        mp_event d={MP_EV_ASSISTANT_DELTA,r->text,NULL}; mp_bus_emit(a->bus,&d);
        if(r->tool_calls){
            const cJSON *tc=NULL;
            cJSON_ArrayForEach(tc,r->tool_calls){
                const cJSON*n=call_name(tc); const cJSON*args=call_args(tc);
                const char *name=(n&&cJSON_IsString(n))?n->valuestring:"";
                mp_event tcev={MP_EV_TOOL_CALL,name,args};
                mp_bus_emit(a->bus,&tcev);
                char preview[1024]; mp_risk risk; const char *cat=NULL;
                mp_verdict v=pol?mp_policy_check(pol,name,args,preview,sizeof preview,&risk,&cat)
                                :MP_DENY;
                char *res=NULL;
                if(v==MP_ALLOW){
                    res=mp_tool_execute(tools,name,args);
                } else {
                    fprintf(stderr,"[policy] BLOCKED %s (risk=%s cat=%s): %s\n",
                            preview,mp_risk_name(risk),cat?cat:"?",
                            v==MP_DENY?"denied (fail-closed; use --auto-approve or the REPL to grant)":"needs interactive approval but none available");
                    char buf[1200];
                    snprintf(buf,sizeof buf,
                        "[permission-denied] tool '%s' flagged risk=%s category=%s was NOT executed. "
                        "Explain what you wanted to do and ask the user to approve it in the REPL, or rephrase without the harmful action.",
                        name,mp_risk_name(risk),cat?cat:"unknown");
                    res=strdup(buf);
                }
                mp_event rev={MP_EV_TOOL_RESULT,res?res:"(error)",NULL}; mp_bus_emit(a->bus,&rev);
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
