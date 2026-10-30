#ifndef MINIPI_AGENT_EVENTS_H
#define MINIPI_AGENT_EVENTS_H
/* Agent event bus: providers/tools emit events; TUI and session store consume.
 * Single-threaded core loop (libuv owns I/O threads; callbacks marshal here). */
typedef enum {
    MP_EV_USER_MSG, MP_EV_ASSISTANT_DELTA, MP_EV_ASSISTANT_DONE,
    MP_EV_TOOL_CALL, MP_EV_TOOL_RESULT, MP_EV_ERROR, MP_EV_TURN_END
} mp_ev_kind;
typedef struct { mp_ev_kind kind; const char *text; const void *json; } mp_event;
typedef void (*mp_ev_cb)(const mp_event *ev, void *ud);
typedef struct mp_bus mp_bus;
mp_bus *mp_bus_new(void);
void    mp_bus_free(mp_bus *b);
void    mp_bus_subscribe(mp_bus *b, mp_ev_cb cb, void *ud);
void    mp_bus_emit(mp_bus *b, const mp_event *ev);
#endif
