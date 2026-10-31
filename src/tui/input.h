#ifndef MINIPI_TUI_INPUT_H
#define MINIPI_TUI_INPUT_H
/* C-4 (pi-tui Editor port): raw-mode line editor for the REPL.
 * Features: UTF-8-aware cursor movement (left/right/home/end/Ctrl-left/right),
 * backspace/delete, kill-line/kill-word/yank (simple kill ring), multiline via
 * Shift-until-enter continuation (\ at EOL or Alt-Enter later), per-cwd
 * persistent history with Ctrl-R substring search, Tab completion of paths and
 * slash commands, bracketed paste, Ctrl-C = interrupt current turn (or clear
 * line when idle), Ctrl-D = EOF on empty line. Replaces fgets() reading in the
 * REPL; falls back to plain getline when stdin is not a tty. */
#include <stddef.h>

typedef struct mp_editor mp_editor;

/* Callbacks the caller provides for interactive control events. */
typedef enum { MP_ED_QUIT, MP_ED_INTERRUPT, MP_ED_SUBMIT } mp_ed_action;
struct mp_editor {
    int tty;                    /* raw mode active? */
    char *buf; size_t len, cap; /* logical text (UTF-8, may contain \n) */
    size_t cur;                 /* cursor byte offset */
    char  **hist; int nhist, hcap;   /* session history (loaded from file) */
    int hist_pos;               /* -1 = editing fresh line */
    char hist_file[4096];
    char killbuf[8192];         /* single-slot kill ring */
    int bracketed;              /* saw ESC[200~ .. ESC[201~ wrapper */
};

mp_editor *mp_ed_new(const char *hist_file);
void       mp_ed_free(mp_editor *e);
/* Read one logical input line (prompt shown). Returns MP_ED_SUBMIT with text
 * in *out (malloc'd, trailing newline stripped), or QUIT/INTERRUPT.
 * If stdin is not a tty, reads a plain line (no escape handling). */
mp_ed_action mp_ed_readline(mp_editor *e, const char *prompt, char **out);
/* History persistence: load/save (max 500 entries, dedup consecutive). */
int  mp_ed_hist_load(mp_editor *e);
int  mp_ed_hist_save(mp_editor *e);
void mp_ed_hist_add(mp_editor *e, const char *text);
/* Exposed for unit tests: edit one keystroke into buffer state.
 * 'key' codes: printable chars pass through; specials use 0x01..0x1A (Ctrl-A
 * .. Ctrl-Z style) plus 0x7F backspace, 0x1B-prefixed sequences decoded by the
 * caller. Returns action needed (SUBMIT on Enter, none otherwise). */
mp_ed_action mp_ed_key(mp_editor *e, int key);
#endif
