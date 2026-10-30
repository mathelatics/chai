#include "events.h"
#include <stdlib.h>
#define MP_BUS_MAX 32
struct mp_bus { mp_ev_cb cbs[MP_BUS_MAX]; void *uds[MP_BUS_MAX]; int n; };
mp_bus *mp_bus_new(void){ mp_bus *b=calloc(1,sizeof *b); return b; }
void mp_bus_free(mp_bus *b){ free(b); }
void mp_bus_subscribe(mp_bus *b, mp_ev_cb cb, void *ud){
    if(!b||b->n>=MP_BUS_MAX)return; b->cbs[b->n]=cb; b->uds[b->n]=ud; b->n++; }
void mp_bus_emit(mp_bus *b, const mp_event *ev){
    if(!b)return; for(int i=0;i<b->n;i++) b->cbs[i](ev,b->uds[i]); }
