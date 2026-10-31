#ifndef MINIPI_AGENT_EVENTS_H
#define MINIPI_AGENT_EVENTS_H
/* Agent event bus: providers/tools emit events; TUI and session store consume.
 * Single-threaded core loop (libuv owns I/O threads; callbacks marshal here). */
typedef enum {
    MP_EV_USER_MSG, MP_EV_ASSISTANT_DELTA, MP_EV_ASSISTANT_DONE,
    MP_EV_TOOL_CALL, MP_EV_TOOL_RESULT, MP_EV_ERROR, MP_EV_TURN_END,
    /* C-5/C-6 additions (pi event spine vocabulary): */
    MP_EV_AGENT_START,      /* one-shot/headless run begins        */
    MP_EV_AGENT_END,        /* run ends; text = stop reason        */
    MP_EV_USAGE             /* per-turn usage; json = usage object */
} mp_ev_kind;
typedef struct { mp_ev_kind kind; const char *text; const void *json; } mp_event;
/* Strongly-typed payloads for the C-5/C-6 spine. When json points at one of
 * these structs, kind identifies the layout:
 *   MP_EV_USAGE      -> const mp_usage*       (per-turn token accounting)
 *   MP_EV_AGENT_END  -> const mp_run_stats*   (totals + stop reason)
 * Other kinds keep `json` as a cJSON* (tool args) or NULL. */
typedef struct { long input_tokens; long output_tokens; double cost_usd; } mp_usage;
typedef struct { long input_tokens, output_tokens; int turns; char stop[32]; } mp_run_stats;
typedef void (*mp_ev_cb)(const mp_event *ev, void *ud);
typedef struct mp_bus mp_bus;
mp_bus *mp_bus_new(void);
void    mp_bus_free(mp_bus *b);
void    mp_bus_subscribe(mp_bus *b, mp_ev_cb cb, void *ud);
void    mp_bus_emit(mp_bus *b, const mp_event *ev);
/* C-6: canonical event name ("tool_call", ...) and a subscriber that writes
 * every bus event as one JSONL record to `f` (flushed per line). */
const char *mp_ev_name(mp_ev_kind k);
struct _IO_FILE;
void    mp_bus_emit_jsonl(mp_bus *b, struct _IO_FILE *f);
#endif
