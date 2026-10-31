#ifndef MINIPI_TUI_DISP_H
#define MINIPI_TUI_DISP_H
#include <stddef.h>
#include <stdint.h>

/* Stream-safe display layer (C-1). All model/tool text that reaches the
 * terminal goes through here so a hostile or buggy upstream can never emit
 * raw escape sequences or control bytes. Each stream keeps its own sanitizer
 * state so multibyte chars split across deltas render correctly. */
typedef enum {
    MP_DISP_ANSWER = 0,   /* normal colors: assistant answer text   */
    MP_DISP_REASONING,    /* dimmed, optional prefix; may be muted   */
    MP_DISP_TOOL_OUT      /* tool stdout/stderr echoed to user       */
} mp_disp_stream;

void mp_disp_set_reasoning_enabled(int on);   /* /thinking toggles this */
int  mp_disp_reasoning_enabled(void);

/* Write (or write-all, len==SIZE_MAX uses strlen) sanitized text to stdout. */
void mp_disp_write(mp_disp_stream s, const char *text, size_t len);

/* End of a streamed message: flush pending partials and terminate the line
 * if the last thing written wasn't a newline. Call once per assistant reply
 * (and after each tool output block). */
void mp_disp_end(mp_disp_stream s);

/* Reset all streams (new turn). */
void mp_disp_reset(void);
#endif
