#include "render.h"
#include <stdio.h>
#include <string.h>
#ifdef MINIPI_WITH_TERMBOX2
#include "termbox2.h"
static int tb_ok(void) { return 1; }
static void putstr(int x, int y, const char *s) {
    for (int i = 0; s[i]; i++) tb_change_cell(x + i, y, (unsigned char)s[i], TB_WHITE, TB_BLACK);
}
#endif
void mp_render_status(const char *model, long tokens, const char *session) {
#ifdef MINIPI_WITH_TERMBOX2
    if (tb_ok()) { char bar[256]; snprintf(bar, sizeof bar, " %s | %ld tok | %s ", model, tokens, session);
                   putstr(0, 0, bar); tb_present(); return; }
#endif
    fprintf(stderr, "[%s %ld tok %s]\n", model, tokens, session);
}
void mp_render_diff_preview(const char *d) {
#ifdef MINIPI_WITH_TERMBOX2
    if (tb_ok()) { int y = 2; for (const char *s = d; *s && y < 20; ) {
        const char *e = strchr(s, '\n'); size_t l = e ? (size_t)(e - s) : strlen(s);
        char ln[128]; size_t c = l < 127 ? l : 127; memcpy(ln, s, c); ln[c] = 0;
        putstr(0, y++, ln); if (!e) break; s = e + 1; }
        tb_present(); return; }
#endif
    fputs(d, stdout);
}
