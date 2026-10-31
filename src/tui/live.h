#ifndef MINIPI_TUI_LIVE_H
#define MINIPI_TUI_LIVE_H
/* C-3 (GAP §4 / pi-tui): transient "live region" rendered at the bottom of the
 * screen — spinner + elapsed timer + status line — that is drawn, then fully
 * erased before anything permanent (scrollback) is printed, so history never
 * gets corrupted. All frames are wrapped in CSI 2026 synchronized-update
 * brackets (ESC[?2026h ... ESC[?2026l) when the stream looks like a terminal,
 * and every glyph passes through the C-1 sanitizer first. The live region is
 * capped at MP_LIVE_MAX_ROWS rows; overflow text is elided with '…'. */
#include <stddef.h>

#define MP_LIVE_MAX_ROWS 8

void mp_live_start(const char *status);   /* begin a live block ("thinking…") */
void mp_live_update(const char *status);  /* replace status text (sanitized) */
/* Advance animation by one tick (~100ms cadence expected from caller). */
void mp_live_tick(void);
/* Print a permanent line ABOVE the live region (scrollback-safe). */
void mp_live_print_above(const char *text, size_t len); /* SIZE_MAX => strlen */
const char *mp_live_elapsed_str(void);   /* "12.3s" since start */
void mp_live_stop(void);                 /* erase live region entirely */
int  mp_live_active(void);
#endif
