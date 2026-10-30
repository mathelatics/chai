#ifndef MINIPI_TUI_RENDER_H
#define MINIPI_TUI_RENDER_H
/* termbox2 rendering helpers. All functions no-op when TB is not initialized
 * (headless mode), so the same code paths are safe in CI. */
void mp_render_status(const char *model, long tokens, const char *session);
void mp_render_diff_preview(const char *diff_text);
#endif
