#include "events.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#define MP_BUS_MAX 32
struct mp_bus { mp_ev_cb cbs[MP_BUS_MAX]; void *uds[MP_BUS_MAX]; int n; };
static void jsonl_cb(const mp_event *ev, void *ud); /* fwd decl (C-6) */
mp_bus *mp_bus_new(void){ mp_bus *b=calloc(1,sizeof *b); return b; }
void mp_bus_free(mp_bus *b){ free(b); }
void mp_bus_subscribe(mp_bus *b, mp_ev_cb cb, void *ud){
    if(!b || b->n >= MP_BUS_MAX) return;
    b->cbs[b->n] = cb; b->uds[b->n] = ud; b->n++;
}
void mp_bus_emit(mp_bus *b, const mp_event *ev){
    if(!b) return;
    for(int i = 0; i < b->n; i++) b->cbs[i](ev, b->uds[i]);
}

/* ---- C-6: JSONL event stream (pi coding-agent --print --mode json analogue) */
const char *mp_ev_name(mp_ev_kind k){
    switch(k){
    case MP_EV_USER_MSG:        return "user_msg";
    case MP_EV_ASSISTANT_DELTA: return "assistant_delta";
    case MP_EV_ASSISTANT_DONE:  return "assistant_done";
    case MP_EV_TOOL_CALL:       return "tool_call";
    case MP_EV_TOOL_RESULT:     return "tool_result";
    case MP_EV_ERROR:           return "error";
    case MP_EV_TURN_END:        return "turn_end";
    case MP_EV_AGENT_START:     return "agent_start";
    case MP_EV_AGENT_END:       return "agent_end";
    case MP_EV_USAGE:           return "usage";
    }
    return "event";
}
void mp_bus_emit_jsonl(mp_bus *b, FILE *f){
    if(!b||!f)return;
    mp_bus_subscribe(b, jsonl_cb, f);
}
static void jsonl_cb(const mp_event *ev, void *ud){
    FILE *f = ud;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", mp_ev_name(ev->kind));
    if(ev->text) cJSON_AddStringToObject(o, "text", ev->text);
    if(ev->kind == MP_EV_USAGE && ev->json){
        const mp_usage *u = ev->json;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "input_tokens", (double)u->input_tokens);
        cJSON_AddNumberToObject(d, "output_tokens", (double)u->output_tokens);
        cJSON_AddNumberToObject(d, "cost_usd", u->cost_usd);
        cJSON_AddItemToObject(o, "data", d);
    } else if(ev->kind == MP_EV_AGENT_END && ev->json){
        const mp_run_stats *st = ev->json;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "input_tokens", (double)st->input_tokens);
        cJSON_AddNumberToObject(d, "output_tokens", (double)st->output_tokens);
        cJSON_AddNumberToObject(d, "turns", st->turns);
        cJSON_AddStringToObject(d, "stop", st->stop);
        cJSON_AddItemToObject(o, "data", d);
    } else if(ev->json){
        /* json may be a cJSON* (already owned elsewhere): embed a duplicate */
        cJSON *d = cJSON_Duplicate((cJSON*)ev->json, 1);
        if(d) cJSON_AddItemToObject(o, "data", d);
    }
    char *s = cJSON_PrintUnformatted(o);
    if(s){ fputs(s, f); fputc('\n', f); fflush(f); free(s); }
    cJSON_Delete(o);
}
