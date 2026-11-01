/* Full-screen terminal UI (termbox2). Single-threaded by design: provider
 * streaming and tool calls run on the same thread, and the UI is redrawn from
 * stream callbacks. Everything that enters the transcript is sanitized, so a
 * hostile model/tool cannot emit escape sequences into the terminal.
 *
 * Layout:
 *   row 0        status bar (provider/model/policy/thinking/tokens)
 *   rows 1..h-3  scrollable transcript (user, assistant, reasoning, tools)
 *   row h-2      input line
 *   row h-1      hint / transient status
 */
#define _GNU_SOURCE
#include "tui/fullscreen.h"
#include "tui/app.h"
#include "agent/context.h"
#include "providers/provider.h"
#include "providers/router.h"
#include "tools/registry.h"
#include "security/policy.h"
#include "config/config.h"
#include "config/keys.h"
#include "session/store.h"
#include "util/paths.h"
#include "util/utf8.h"
#include "cJSON.h"
#include "log.h"
#include "termbox2.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern mp_config g_cfg;   /* defined in tui/repl.c (shared with main/loop) */

#define INPUT_MAX 8192
#define MAX_LINES 4000
#define MAX_ENTRIES 2000

enum { E_USER = 0, E_ASSIST, E_REASON, E_TOOL, E_TOOL_OUT, E_ERROR, E_INFO };

typedef struct { int kind; char *text; } tentry;
typedef struct { char *s; uint32_t fg; } dline;
typedef struct { dline *v; int n, cap; } dlines;

typedef struct {
    mp_agent *agent;
    mp_policy *policy;
    mp_store *store;
    int thinking;
    long max_tokens;
    int quit, busy;
    int scroll;
    long in_tok, out_tok;

    tentry *ent; int nent, cap;
    int ans_entry, rea_entry;
    mp_utf8_san san_ans, san_rea;

    char input[INPUT_MAX]; size_t inlen, incur;
    int secret;
    char prompt[128];
    char hint[512];
    char model_label[256];

    char **hist; int nhist, hcap, hist_pos;
    char hist_file[4096];

    double last_draw;
} tui;

/* ------------------------------------------------------------- utf8 utils */
static size_t u8_next(const char *s, size_t n, size_t *i, uint32_t *cp) {
    if (*i >= n) return 0;
    unsigned char c = (unsigned char)s[*i];
    size_t len = 1; uint32_t v = c;
    if (c < 0x80) { v = c; len = 1; }
    else if ((c & 0xE0) == 0xC0 && *i + 1 < n) { v = ((c & 0x1Fu) << 6) | (s[*i+1] & 0x3Fu); len = 2; }
    else if ((c & 0xF0) == 0xE0 && *i + 2 < n) { v = ((c & 0x0Fu) << 12) | ((s[*i+1] & 0x3Fu) << 6) | (s[*i+2] & 0x3Fu); len = 3; }
    else if ((c & 0xF8) == 0xF0 && *i + 3 < n) { v = ((c & 0x07u) << 18) | ((s[*i+1] & 0x3Fu) << 12) | ((s[*i+2] & 0x3Fu) << 6) | (s[*i+3] & 0x3Fu); len = 4; }
    else v = 0xFFFD;
    *cp = v; *i += len;
    return len;
}
static int cp_width(uint32_t cp) {
    if (cp >= 0x0300 && cp <= 0x036F) return 0;           /* combining */
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}
static int cp_count(const char *s, size_t n) {
    int c = 0; size_t i = 0; uint32_t cp;
    while (i < n && u8_next(s, n, &i, &cp)) c++;
    return c;
}
static size_t byte_of_cp(const char *s, size_t n, int target) {
    size_t i = 0; int c = 0; uint32_t cp;
    while (i < n && c < target && u8_next(s, n, &i, &cp)) c++;
    return i;
}
static int cp_index_of_byte(const char *s, size_t n, size_t byte) {
    size_t i = 0; int c = 0; uint32_t cp;
    while (i < n && i < byte && u8_next(s, n, &i, &cp)) c++;
    return c;
}

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

/* ------------------------------------------------------------ transcript */
static void ui_free_entry(tentry *e) { free(e->text); e->text = NULL; }
static void ui_clear(tui *u) {
    for (int i = 0; i < u->nent; i++) ui_free_entry(&u->ent[i]);
    u->nent = 0; u->ans_entry = u->rea_entry = -1;
}
static int ui_add(tui *u, int kind, const char *text) {
    if (u->nent >= MAX_ENTRIES) {
        int drop = MAX_ENTRIES / 4;
        for (int i = 0; i < drop; i++) ui_free_entry(&u->ent[i]);
        memmove(u->ent, u->ent + drop, sizeof(tentry) * (size_t)(u->nent - drop));
        u->nent -= drop;
        if (u->ans_entry >= 0) u->ans_entry -= drop;
        if (u->rea_entry >= 0) u->rea_entry -= drop;
    }
    if (u->nent == u->cap) {
        int nc = u->cap ? u->cap * 2 : 64;
        tentry *nv = realloc(u->ent, sizeof(tentry) * (size_t)nc);
        if (!nv) return -1;
        u->ent = nv; u->cap = nc;
    }
    u->ent[u->nent].kind = kind;
    u->ent[u->nent].text = strdup(text ? text : "");
    if (!u->ent[u->nent].text) return -1;
    return u->nent++;
}
static void ui_append(tui *u, int idx, const char *s) {
    if (idx < 0 || idx >= u->nent || !s || !*s) return;
    size_t old = strlen(u->ent[idx].text), add = strlen(s);
    char *nv = realloc(u->ent[idx].text, old + add + 1);
    if (!nv) return;
    memcpy(nv + old, s, add + 1);
    u->ent[idx].text = nv;
}
/* Tool output and model-supplied strings are untrusted: strip escape
 * sequences/control bytes before they can reach a terminal cell. */
static void ui_add_clean(tui *u, int kind, const char *text) {
    if (!text) return;
    size_t n = strlen(text), cap = n * 4 + 16;
    char *clean = malloc(cap);
    if (!clean) return;
    mp_utf8_san s; mp_utf8_san_init(&s);
    size_t used = 0, off = 0;
    while (off < n) {
        size_t chunk = n - off > 2048 ? 2048 : n - off;
        if (mp_utf8_sanitize(&s, text + off, chunk, clean + used, cap - used, &used) != 0) break;
        off += chunk;
    }
    mp_utf8_flush(&s, clean + used, cap - used, &used);
    clean[used] = 0;
    ui_add(u, kind, clean);
    free(clean);
}
static void ui_info(tui *u, int kind, const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_add(u, kind, buf);
}
static void ui_error(tui *u, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_add(u, E_ERROR, buf);
}

/* ---------------------------------------------------------------- drawing */
static void dl_free(dlines *d) {
    for (int i = 0; i < d->n; i++) free(d->v[i].s);
    free(d->v); memset(d, 0, sizeof *d);
}
static void dl_add(dlines *d, const char *s, uint32_t fg) {
    if (d->n >= MAX_LINES) {
        free(d->v[0].s);
        memmove(d->v, d->v + 1, sizeof(dline) * (size_t)(d->n - 1));
        d->n--;
    }
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 128;
        dline *nv = realloc(d->v, sizeof(dline) * (size_t)nc);
        if (!nv) return;
        d->v = nv; d->cap = nc;
    }
    d->v[d->n].s = strdup(s ? s : "");
    d->v[d->n].fg = fg;
    if (d->v[d->n].s) d->n++;
}
static void wrap_into(dlines *d, const char *prefix, const char *text,
                      uint32_t fg, int width) {
    if (width < 4) width = 4;
    size_t cap = (size_t)width * 4 + (prefix ? strlen(prefix) : 0) + 16;
    char *line = malloc(cap);
    if (!line) return;
    size_t len = 0; int col = 0;
    if (prefix) {
        size_t pl = strlen(prefix);
        if (pl + 1 > cap) pl = cap - 1;
        memcpy(line, prefix, pl); len = pl; line[len] = 0;
        col = (int)pl;
    }
    size_t n = strlen(text), i = 0; uint32_t cp;
    while (i < n) {
        size_t at = i;
        u8_next(text, n, &i, &cp);
        if (cp == '\n') { line[len] = 0; dl_add(d, line, fg); len = 0; col = 0; line[0] = 0; continue; }
        if (cp == '\t') { cp = ' '; }
        int cw = cp_width(cp);
        if (col + cw > width && col > 0) { line[len] = 0; dl_add(d, line, fg); len = 0; col = 0; line[0] = 0; }
        if (cw == 0 && col == 0) { i = at + 1; continue; }
        char tmp[8]; int tl = cp < 0x80 ? (tmp[0] = (char)cp, 1) : tb_utf8_unicode_to_char(tmp, cp);
        if (!tl) tl = 1;
        if (len + (size_t)tl + 1 < cap) { memcpy(line + len, tmp, (size_t)tl); len += (size_t)tl; line[len] = 0; col += cw; }
    }
    line[len] = 0;
    dl_add(d, line, fg);
    free(line);
}
static uint32_t entry_fg(int kind) {
    switch (kind) {
        case E_USER:     return TB_CYAN | TB_BOLD;
        case E_ASSIST:   return TB_WHITE;
        case E_REASON:   return TB_DEFAULT | TB_BOLD;
        case E_TOOL:     return TB_YELLOW;
        case E_TOOL_OUT: return TB_DEFAULT;
        case E_ERROR:    return TB_RED | TB_BOLD;
        default:         return TB_BLUE | TB_BOLD;
    }
}
static const char *entry_prefix(int kind) {
    switch (kind) {
        case E_USER:     return "you > ";
        case E_REASON:   return "think > ";
        case E_TOOL:     return "";
        case E_TOOL_OUT: return "";
        default:         return "";
    }
}
static void build_lines(tui *u, dlines *d, int width) {
    memset(d, 0, sizeof *d);
    for (int i = 0; i < u->nent; i++) {
        int kind = u->ent[i].kind;
        const char *pre = entry_prefix(kind);
        const char *text = u->ent[i].text ? u->ent[i].text : "";
        if (kind == E_ASSIST && i == u->nent - 1 && u->busy && !*text) {
            wrap_into(d, pre, "…", entry_fg(kind), width);
            continue;
        }
        wrap_into(d, pre, text, entry_fg(kind), width);
    }
    if (u->nent == 0) {
        wrap_into(d, NULL, "MiniPi — a pi-style coding agent in C.", TB_CYAN, width);
        wrap_into(d, NULL, "", TB_DEFAULT, width);
        wrap_into(d, NULL, "Type a message, or /help for commands.", TB_DEFAULT, width);
        wrap_into(d, NULL, "/connect to pick a cloud or local provider.", TB_DEFAULT, width);
    }
}
static void fill_row(int y, int w, uint32_t bg) {
    for (int x = 0; x < w; x++) tb_set_cell(x, y, ' ', TB_DEFAULT, bg);
}
static void draw_box(int x, int y, int w, int h, const char *title) {
    if (w < 4 || h < 3) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            tb_set_cell(x + i, y + j, ' ', TB_WHITE, TB_BLUE);
    char t[256];
    snprintf(t, sizeof t, " %s ", title ? title : "");
    t[w > 2 ? w - 2 : 0] = 0;
    tb_printf(x + 1, y, TB_BOLD | TB_WHITE, TB_BLUE, "%s", t);
}

static void render(tui *u) {
    int w = tb_width(), h = tb_height();
    tb_clear();
    if (w < 24 || h < 7) {
        tb_printf(0, 0, TB_RED, TB_DEFAULT, "terminal too small (%dx%d)", w, h);
        tb_present();
        return;
    }
    mp_provider *prov = mp_cfg_active(&g_cfg);
    const char *pm = prov ? prov->name : "none";
    const char *model = g_cfg.session_model[0] ? g_cfg.session_model : (prov ? prov->model : "-");
    snprintf(u->model_label, sizeof u->model_label, "%s/%s", pm, model);
    char bar[512];        snprintf(bar, sizeof bar,
             " MiniPi  %s|%s|think:%s|in:%ld/out:%ld",
             u->model_label, mp_mode_name(mp_policy_mode(u->policy)),
             u->thinking ? "on" : "off", u->in_tok, u->out_tok);
    fill_row(0, w, TB_WHITE);
    tb_printf(0, 0, TB_BLACK | TB_BOLD, TB_WHITE, "%-*.*s", w, w, bar);

    dlines d; build_lines(u, &d, w - 2);
    int area_h = h - 2;
    int max_scroll = d.n > area_h ? d.n - area_h : 0;
    if (u->scroll > max_scroll) u->scroll = max_scroll;
    if (u->scroll < 0) u->scroll = 0;
    int first = d.n > area_h ? d.n - area_h - u->scroll : 0;
    for (int i = 0; i < area_h; i++) {
        int idx = first + i;
        if (idx >= d.n) break;
        tb_printf(1, i + 1, d.v[idx].fg, TB_DEFAULT, "%s", d.v[idx].s);
    }
    dl_free(&d);

    /* input line */
    fill_row(h - 1, w, TB_DEFAULT);
    tb_printf(0, h - 1, TB_GREEN | TB_BOLD, TB_DEFAULT, "%s", u->prompt[0] ? u->prompt : "> ");
    int prompt_w = (int)strlen(u->prompt[0] ? u->prompt : "> ");
    int avail = w - prompt_w - 1;
    int cur_cp = cp_index_of_byte(u->input, u->inlen, u->incur);
    int total_cp = cp_count(u->input, u->inlen);
    int start_cp = 0;
    if (cur_cp >= avail) start_cp = cur_cp - avail + 1;
    if (start_cp > total_cp) start_cp = 0;
    size_t bs = byte_of_cp(u->input, u->inlen, start_cp);
    size_t be = byte_of_cp(u->input, u->inlen, start_cp + avail);
    if (u->secret) {
        int dots = cp_count(u->input + bs, be - bs);
        for (int i = 0; i < dots && prompt_w + i < w - 1; i++)
            tb_set_cell(prompt_w + i, h - 2, '*', TB_WHITE, TB_DEFAULT);
    } else {
        char *slice = strndup(u->input + bs, be - bs);
        if (slice) { tb_printf(prompt_w, h - 2, TB_WHITE, TB_DEFAULT, "%s", slice); free(slice); }
    }
    int cx = prompt_w + (cur_cp - start_cp);
    if (cx >= w) cx = w - 1;
    tb_set_cursor(cx, h - 1);

    /* hint row */
    fill_row(h, w, TB_DEFAULT);
    char hint[512];
    if (u->hint[0]) snprintf(hint, sizeof hint, "%s", u->hint);
    else snprintf(hint, sizeof hint,
                  "Enter send  ·  /help  ·  PgUp/PgDn scroll  ·  Ctrl-C %s",
                  u->input[0] ? "clear line" : "quit");
    tb_printf(0, h - 1, TB_DEFAULT | TB_BOLD, TB_DEFAULT, "%-*.*s", w, w, hint);
    tb_present();
    u->last_draw = now_s();
}

/* ------------------------------------------------------------- modal pick */
static int tui_pick(tui *u, const char *title, char **items, int n, int selected) {
    if (n <= 0) return -1;
    if (selected < 0 || selected >= n) selected = 0;
    for (;;) {
        render(u);
        int w = tb_width(), h = tb_height();
        int bw = w - 6 > 78 ? 78 : w - 6;
        int bh = h - 4 > 12 ? 12 : h - 4;
        if (bw < 24) bw = w - 2;
        if (bh < 5) bh = h - 1;
        int bx = (w - bw) / 2, by = (h - bh) / 2;
        draw_box(bx, by, bw, bh, title);
        int list_h = bh - 3;
        int first = selected - list_h / 2;
        if (first < 0) first = 0;
        if (first > n - list_h) first = n - list_h > 0 ? n - list_h : 0;
        for (int i = 0; i < list_h; i++) {
            int idx = first + i;
            if (idx >= n) break;
            char line[512];
            snprintf(line, sizeof line, " %s", items[idx]);
            line[bw - 2 > 0 ? bw - 2 : 0] = 0;
            if (idx == selected)
                tb_printf(bx + 1, by + 1 + i, TB_BLACK | TB_BOLD, TB_WHITE, "%-*.*s", bw - 2, bw - 2, line);
            else
                tb_printf(bx + 1, by + 1 + i, TB_WHITE, TB_BLUE, "%-*.*s", bw - 2, bw - 2, line);
        }
        tb_printf(bx + 1, by + bh - 1, TB_DEFAULT | TB_BOLD, TB_BLUE,
                  "  up/down select   Enter confirm   Esc cancel  ");
        tb_present();
        struct tb_event ev;
        if (tb_poll_event(&ev) != TB_OK) continue;
        if (ev.type == TB_EVENT_RESIZE) continue;
        if (ev.type != TB_EVENT_KEY) continue;
        if (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C) return -1;
        if (ev.key == TB_KEY_ENTER) return selected;
        if (ev.key == TB_KEY_ARROW_UP) selected = (selected + n - 1) % n;
        else if (ev.key == TB_KEY_ARROW_DOWN) selected = (selected + 1) % n;
        else if (ev.key == TB_KEY_PGUP) selected = selected - list_h < 0 ? 0 : selected - list_h;
        else if (ev.key == TB_KEY_PGDN) selected = selected + list_h >= n ? n - 1 : selected + list_h;
        else if (ev.key == TB_KEY_HOME) selected = 0;
        else if (ev.key == TB_KEY_END) selected = n - 1;
    }
}

/* --------------------------------------------------------------- input line */
static void input_insert(tui *u, const char *s, size_t n) {
    if (u->inlen + n + 1 >= sizeof u->input) return;
    memmove(u->input + u->incur + n, u->input + u->incur, u->inlen - u->incur);
    memcpy(u->input + u->incur, s, n);
    u->inlen += n; u->incur += n; u->input[u->inlen] = 0;
}
static void input_backspace(tui *u) {
    if (!u->incur) return;
    size_t prev = u->incur - 1;
    while (prev > 0 && ((unsigned char)u->input[prev] & 0xC0) == 0x80) prev--;
    memmove(u->input + prev, u->input + u->incur, u->inlen - u->incur + 1);
    u->inlen -= u->incur - prev;
    u->incur = prev;
}
static void input_delete(tui *u) {
    if (u->incur >= u->inlen) return;
    size_t next = u->incur + 1;
    while (next < u->inlen && ((unsigned char)u->input[next] & 0xC0) == 0x80) next++;
    memmove(u->input + u->incur, u->input + next, u->inlen - next + 1);
    u->inlen -= next - u->incur;
}
static int prompt_line(tui *u, const char *label, const char *initial,
                       int secret, char *out, size_t out_n) {
    snprintf(u->prompt, sizeof u->prompt, "%s", label ? label : "> ");
    u->secret = secret;
    if (initial) snprintf(u->input, sizeof u->input, "%s", initial);
    else u->input[0] = 0;
    u->inlen = strlen(u->input); u->incur = u->inlen;
    snprintf(u->hint, sizeof u->hint, "Enter confirm · Esc cancel");
    for (;;) {
        render(u);
        struct tb_event ev;
        if (tb_poll_event(&ev) != TB_OK) continue;
        if (ev.type == TB_EVENT_RESIZE) continue;
        if (ev.type != TB_EVENT_KEY) continue;
        if (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C) {
            u->input[0] = 0; u->inlen = u->incur = 0;
            u->secret = 0; snprintf(u->prompt, sizeof u->prompt, "> ");
            u->hint[0] = 0;
            return -1;
        }
        if (ev.key == TB_KEY_ENTER) {
            if (out && out_n) snprintf(out, out_n, "%s", u->input);
            u->input[0] = 0; u->inlen = u->incur = 0;
            u->secret = 0; snprintf(u->prompt, sizeof u->prompt, "> ");
            u->hint[0] = 0;
            return 0;
        }
        if (ev.key == TB_KEY_ARROW_LEFT) {
            if (u->incur) { size_t p = u->incur - 1; while (p > 0 && ((unsigned char)u->input[p] & 0xC0) == 0x80) p--; u->incur = p; }
        } else if (ev.key == TB_KEY_ARROW_RIGHT) {
            if (u->incur < u->inlen) { size_t nx = u->incur + 1; while (nx < u->inlen && ((unsigned char)u->input[nx] & 0xC0) == 0x80) nx++; u->incur = nx; }
        } else if (ev.key == TB_KEY_HOME) u->incur = 0;
        else if (ev.key == TB_KEY_END) u->incur = u->inlen;
        else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) input_backspace(u);
        else if (ev.key == TB_KEY_DELETE || ev.key == TB_KEY_CTRL_D) input_delete(u);
        else if (ev.key == TB_KEY_CTRL_U) { u->input[0] = 0; u->inlen = u->incur = 0; }
        else if (ev.ch) {
            char tmp[8]; int n = tb_utf8_unicode_to_char(tmp, ev.ch);
            if (n > 0) input_insert(u, tmp, (size_t)n);
        }
    }
}

/* ---------------------------------------------------------- provider flows */
static void tui_connect(tui *u, const char *preset_id) {
    const mp_preset *p = preset_id && *preset_id ? mp_preset_find(preset_id) : NULL;
    char **labels = NULL; int nlabels = 0;
    if (!p) {
        int n = 0; const mp_preset *ps = mp_presets(&n);
        labels = calloc((size_t)n, sizeof *labels);
        if (!labels) return;
        for (int i = 0; i < n; i++) {
            char b[256];
            snprintf(b, sizeof b, "%-12s %-20s %s%s", ps[i].id, ps[i].label,
                     ps[i].base_url, ps[i].local ? "  (local)" : "");
            labels[nlabels] = strdup(b);
            if (labels[nlabels]) nlabels++;
        }
        int sel = tui_pick(u, "Connect a provider", labels, nlabels, 0);
        for (int i = 0; i < nlabels; i++) free(labels[i]);
        free(labels);
        if (sel < 0) return;
        p = mp_preset_by_index(sel + 1);
        if (!p) return;
    }
    char base[512]; snprintf(base, sizeof base, "%s", p->base_url);
    if (p->local) {
        char ans[512];
        if (prompt_line(u, "url> ", p->base_url, 0, ans, sizeof ans) < 0) return;
        if (*ans) snprintf(base, sizeof base, "%s", ans);
    } else {
        char existing[512];
        int have = mp_keys_lookup(p->key_env, existing, sizeof existing);
        char key[1024] = "";
        char label[256];
        snprintf(label, sizeof label, "key> %s", p->key_env);
        u->hint[0] = 0;
        if (prompt_line(u, label, "", 1, key, sizeof key) == 0 && *key) {
            if (mp_keys_save(p->key_env, key) == 0) {
                ui_info(u, E_INFO, "[key] %s stored in %s (mode 0600)", p->key_env, mp_keys_env_path());
                have = 1;
            } else ui_error(u, "[key] could not store key; export %s for the broker", p->key_env);
        }
        if (!have) {
            ui_error(u, "no key configured for %s — run /connect again or /connect key %s", p->key_env, p->id);
            return;
        }
    }
    if (mp_cfg_find(&g_cfg, p->id)) mp_cfg_rm(&g_cfg, p->id);
    int rc = mp_cfg_add_ex(&g_cfg, p->id, p->kind, base, p->model, p->key_env, p->auth);
    if (rc) { ui_error(u, "connect failed (code %d: provider list full or bad url)", rc); return; }
    mp_cfg_use(&g_cfg, p->id);
    mp_provider *prov = mp_cfg_active(&g_cfg);

    u->busy = 1; snprintf(u->hint, sizeof u->hint, "fetching models from %.400s …", base);
    render(u);
    mp_model_list ml;
    if (mp_provider_list_models(prov, &ml) == 0 && ml.n > 0) {
        char **items = calloc((size_t)ml.n, sizeof *items);
        if (items) {
            for (int i = 0; i < ml.n; i++) items[i] = strdup(ml.v[i]);
            char title[256]; snprintf(title, sizeof title, "Models — %s (%d)", p->label, ml.n);
            int sel = tui_pick(u, title, items, ml.n, 0);
            if (sel >= 0) {
                snprintf(prov->model, sizeof prov->model, "%s", ml.v[sel]);
                snprintf(g_cfg.session_model, sizeof g_cfg.session_model, "%s", ml.v[sel]);
            }
            for (int i = 0; i < ml.n; i++) free(items[i]);
            free(items);
        }
    } else {
        ui_info(u, E_INFO, "live model list unavailable; keeping default '%s'", prov->model);
    }
    mp_model_list_free(&ml);
    mp_cfg_save(&g_cfg);
    u->busy = 0; u->hint[0] = 0;
    ui_info(u, E_INFO, "[connected] %s (%s) model: %s",
            prov->name, mp_kind_name(prov->kind),
            g_cfg.session_model[0] ? g_cfg.session_model : prov->model);
}
static void tui_models(tui *u, const char *filter) {
    mp_provider *prov = mp_cfg_active(&g_cfg);
    if (!prov) { ui_error(u, "no provider — run /connect"); return; }
    u->busy = 1; snprintf(u->hint, sizeof u->hint, "querying %s …", prov->name);
    render(u);
    mp_model_list ml;
    int rc = mp_provider_list_models(prov, &ml);
    u->busy = 0; u->hint[0] = 0;
    if (rc || ml.n == 0) {
        ui_error(u, "model list unavailable (status/key/broker?) — configured: %s", prov->model);
        mp_model_list_free(&ml);
        return;
    }
    char **items = calloc((size_t)ml.n, sizeof *items);
    int n = 0;
    if (items) {
        for (int i = 0; i < ml.n; i++) {
            if (filter && *filter && !strcasestr(ml.v[i], filter)) continue;
            items[n] = strdup(ml.v[i]);
            if (items[n]) n++;
        }
    }
    if (n == 0) { ui_info(u, E_INFO, "no models match '%s'", filter ? filter : ""); }
    else {
        const char *cur = g_cfg.session_model[0] ? g_cfg.session_model : prov->model;
        int sel = 0;
        for (int i = 0; i < n; i++) if (!strcmp(items[i], cur)) { sel = i; break; }
        char title[256]; snprintf(title, sizeof title, "Models — %s", prov->name);
        int pick = tui_pick(u, title, items, n, sel);
        if (pick >= 0) {
            mp_cfg_set_model(&g_cfg, items[pick]);
            ui_info(u, E_INFO, "model set: %s", items[pick]);
        }
    }
    for (int i = 0; i < n; i++) free(items[i]);
    free(items);
    mp_model_list_free(&ml);
}

/* ------------------------------------------------------------- approvals */
static void truncate_mid(char *s, size_t cap) {
    size_t n = strlen(s);
    if (n <= cap) return;
    size_t head = cap / 2, tail = cap - head - 20;
    char *out = malloc(cap + 1);
    if (!out) return;
    memcpy(out, s, head);
    memcpy(out + head, " … ", 5);
    memcpy(out + head + 5, s + n - tail, tail);
    out[head + 5 + tail] = 0;
    snprintf(s, cap + 1, "%s", out);
    free(out);
}
static mp_verdict tui_approve(tui *u, const char *name, const cJSON *args,
                              char *deny_note, size_t dn_sz) {
    char preview[2048];
    mp_risk risk = MP_RISK_NONE;
    const char *cat = "none";
    mp_verdict v = mp_policy_check(u->policy, name, args, preview, sizeof preview, &risk, &cat);
    if (v != MP_NEED_PROMPT) return v;
    for (;;) {
        render(u);
        int w = tb_width(), h = tb_height();
        int bw = w - 4 > 80 ? 80 : w - 4;
        int bh = h - 4 > 16 ? 16 : h - 4;
        if (bw < 30) bw = w - 2;
        if (bh < 6) bh = h - 1;
        int bx = (w - bw) / 2, by = (h - bh) / 2;
        char title[256];
        snprintf(title, sizeof title, "Permission required — risk %s%s%s",
                 mp_risk_name(risk), strcmp(cat, "none") ? " · " : "",
                 strcmp(cat, "none") ? cat : "");
        draw_box(bx, by, bw, bh, title);
        /* sanitize + wrap the preview so it cannot spill outside the box or
         * inject terminal escapes */
        size_t pn = strlen(preview), pcap = pn * 4 + 16;
        char *pv = malloc(pcap);
        if (pv) {
            mp_utf8_san ps; mp_utf8_san_init(&ps);
            size_t used = 0;
            mp_utf8_sanitize(&ps, preview, pn, pv, pcap, &used);
            mp_utf8_flush(&ps, pv + used, pcap - used, &used);
            pv[used] = 0;
            dlines body; memset(&body, 0, sizeof body);
            wrap_into(&body, NULL, pv, TB_WHITE, bw - 4);
            int body_h = bh - 5;
            for (int i = 0; i < body.n && i < body_h; i++)
                tb_printf(bx + 2, by + 2 + i, TB_WHITE, TB_BLUE, "%.*s", bw - 4, body.v[i].s);
            dl_free(&body);
            free(pv);
        }
        tb_printf(bx + 1, by + bh - 2, TB_YELLOW | TB_BOLD, TB_BLUE,
                  " [y] allow once   [a] always this tool   [n] deny   [d] deny+note ");
        tb_printf(bx + 1, by + bh - 1, TB_DEFAULT | TB_BOLD, TB_BLUE,
                  " Esc denies. HIGH-risk actions always ask. ");
        tb_present();
        struct tb_event ev;
        if (tb_poll_event(&ev) != TB_OK) continue;
        if (ev.type != TB_EVENT_KEY) continue;
        char c = 0;
        if (ev.ch) c = (char)tolower((unsigned char)ev.ch);
        else if (ev.key == TB_KEY_ESC) c = 'n';
        else if (ev.key == TB_KEY_ENTER) c = 'y';
        if (c == 'd') {
            char note[512] = "";
            if (prompt_line(u, "note> ", "", 0, note, sizeof note) < 0) note[0] = 0;
            mp_policy_record_answer(u->policy, name, 'n');
            if (deny_note && dn_sz)
                snprintf(deny_note, dn_sz,
                         "user denied this action (%s: %s). Reason: %s",
                         mp_risk_name(risk), cat, note);
            return MP_DENY;
        }
        if (c == 'y' || c == 'a' || c == 'n') {
            mp_policy_record_answer(u->policy, name, c);
            if (c == 'n') {
                if (dn_sz) snprintf(deny_note, dn_sz,
                                    "user DENIED this tool call. Do not retry it unchanged.");
                return MP_DENY;
            }
            return MP_ALLOW;
        }
    }
}

/* ------------------------------------------------------------- agent turn */
static void delta_sanitized(tui *u, int *entry, int kind, mp_utf8_san *san,
                            const char *in) {
    if (!in || !*in) return;
    char out[8192]; size_t used = 0;
    if (mp_utf8_sanitize(san, in, strlen(in), out, sizeof out, &used) != 0) return;
    if (!used) return;
    out[used] = 0;
    if (*entry < 0) *entry = ui_add(u, kind, "");
    ui_append(u, *entry, out);
}
static void delta_flush(tui *u, int entry, mp_utf8_san *san) {
    char out[8]; size_t used = 0;
    mp_utf8_flush(san, out, sizeof out, &used);
    if (used && entry >= 0) { out[used] = 0; ui_append(u, entry, out); }
}
static void tui_on_delta(const char *reasoning, const char *content, void *ud) {
    tui *u = ud;
    if (reasoning && *reasoning && u->thinking)
        delta_sanitized(u, &u->rea_entry, E_REASON, &u->san_rea, reasoning);
    if (content && *content)
        delta_sanitized(u, &u->ans_entry, E_ASSIST, &u->san_ans, content);
    double now = now_s();
    if (now - u->last_draw >= 0.033) render(u);
}
static void tui_turn(tui *u, const char *text) {
    mp_ctx *ctx = mp_agent_ctx(u->agent);
    u->scroll = 0;   /* follow the stream while the agent works */
    mp_toolreg *tools = mp_agent_tools(u->agent);
    mp_provider *prov = mp_cfg_active(&g_cfg);
    if (!prov) { ui_error(u, "no provider connected — use /connect"); return; }
    const char *model = g_cfg.session_model[0] ? g_cfg.session_model : prov->model;
    ui_add(u, E_USER, text);
    mp_ctx_push(ctx, "user", text, NULL);
    if (u->store) mp_store_append(u->store, "user", text, NULL, NULL);

    cJSON *schemas = mp_toolreg_schemas(tools);
    int max_turns = g_cfg.max_turns > 0 ? g_cfg.max_turns : 8;
    int turns = 0;
    for (int turn = 0; turn < max_turns; turn++) {
        if (u->quit) break;
        u->ans_entry = u->rea_entry = -1;
        mp_utf8_san_init(&u->san_ans);
        mp_utf8_san_init(&u->san_rea);
        u->busy = 1;
        snprintf(u->hint, sizeof u->hint, "%s…", turn ? "continuing" : "thinking");
        render(u);
        mp_reply *r = mp_provider_stream(prov, model, ctx, u->thinking,
                                         u->max_tokens, tui_on_delta, u, schemas);
        delta_flush(u, u->rea_entry, &u->san_rea);
        delta_flush(u, u->ans_entry, &u->san_ans);
        if (!r) {
            u->busy = 0; u->hint[0] = 0;
            ui_error(u, "completion failed (transport/HTTP). Check provider, key and broker.");
            break;
        }
        turns++;
        u->in_tok += r->input_tokens;
        u->out_tok += r->output_tokens;
        if (r->tool_calls) {
            const cJSON *tc = NULL;
            cJSON_ArrayForEach(tc, r->tool_calls) {
                const cJSON *nj = cJSON_GetObjectItemCaseSensitive(tc, "name");
                const cJSON *aj = cJSON_GetObjectItemCaseSensitive(tc, "arguments");
                const cJSON *idj = cJSON_GetObjectItemCaseSensitive(tc, "id");
                const char *name = cJSON_IsString(nj) ? nj->valuestring : "";
                char tcid[80];
                snprintf(tcid, sizeof tcid, "%s", cJSON_IsString(idj) ? idj->valuestring : "call_0");
                char *argtxt = aj ? cJSON_PrintUnformatted(aj) : NULL;
                char line[1024];
                snprintf(line, sizeof line, "[tool] %s %s", name, argtxt ? argtxt : "");
                free(argtxt);
                truncate_mid(line, 900);
                ui_add_clean(u, E_TOOL, line);
                u->busy = 1;
                snprintf(u->hint, sizeof u->hint, "awaiting approval: %s", name);
                render(u);
                char denybuf[768] = "";
                mp_verdict v = tui_approve(u, name, aj, denybuf, sizeof denybuf);
                char *res = NULL;
                if (v == MP_DENY) {
                    if (!denybuf[0])
                        snprintf(denybuf, sizeof denybuf,
                                 "user DENIED this tool call. Do not retry it unchanged.");
                    res = strdup(denybuf);
                    ui_add_clean(u, E_ERROR, denybuf);
                    if (u->store) mp_store_append(u->store, "denial", name, NULL, NULL);
                } else {
                    snprintf(u->hint, sizeof u->hint, "running %s…", name);
                    render(u);
                    res = mp_tool_execute(tools, name, aj);
                    char *shown = res ? strdup(res) : strdup("(error)");
                    if (shown) {
                        truncate_mid(shown, 4000);
                        ui_add_clean(u, E_TOOL_OUT, shown);
                        free(shown);
                    }
                    if (u->store) {
                        char *tcs = cJSON_PrintUnformatted(tc);
                        mp_store_append(u->store, "tool", res ? res : "(error)", tcs, NULL);
                        free(tcs);
                    }
                }
                cJSON *idwrap = cJSON_CreateObject();
                cJSON_AddStringToObject(idwrap, "id", tcid);
                cJSON *tc_copy = cJSON_Duplicate(tc, 1);
                if (tc_copy) {
                    cJSON_AddStringToObject(tc_copy, "id", tcid);
                }
                mp_ctx_push(ctx, "assistant", "", tc_copy);
                mp_ctx_push(ctx, "tool", res ? res : "(error)", idwrap);
                free(res);
            }
        }
        int had_tools = r->tool_calls != NULL;
        if (r->text && *r->text) {
            mp_ctx_push(ctx, "assistant", r->text, NULL);
            if (u->store) mp_store_append(u->store, "assistant", r->text, NULL, NULL);
        } else if (!had_tools) {
            mp_ctx_push(ctx, "assistant", "", NULL);
        }
        mp_reply_free(r);
        if (!had_tools) break;
    }
    if (schemas) cJSON_Delete(schemas);
    if (u->store) mp_store_checkpoint(u->store);
    u->busy = 0; u->hint[0] = 0; u->scroll = 0;
    ui_info(u, E_INFO, "— %d turn%s, +%ld in / +%ld tok —", turns, turns == 1 ? "" : "s",
            u->in_tok, u->out_tok);
    render(u);
}

/* ----------------------------------------------------------- slash commands */
static const char *slash_cmds[] = {
    "/help", "/connect", "/models", "/model", "/provider", "/policy",
    "/thinking", "/clear", "/quit"
};
static void tab_complete(tui *u) {
    if (u->inlen == 0 || u->input[0] != '/') return;
    size_t best = 0; int found = -1;
    for (size_t i = 0; i < sizeof slash_cmds / sizeof slash_cmds[0]; i++)
        if (!strncmp(slash_cmds[i], u->input, u->inlen)) {
            if (found < 0 || strlen(slash_cmds[i]) < best) { found = (int)i; best = strlen(slash_cmds[i]); }
        }
    if (found >= 0) {
        snprintf(u->input, sizeof u->input, "%s ", slash_cmds[found]);
        u->inlen = strlen(u->input); u->incur = u->inlen;
    }
}
static void handle_slash(tui *u, char *line) {
    char *argv[8]; int argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t", &save); t && argc < 8; t = strtok_r(NULL, " \t", &save))
        argv[argc++] = t;
    if (!argc) return;
    if (!strcmp(argv[0], "/quit") || !strcmp(argv[0], "/exit")) { u->quit = 1; return; }
    if (!strcmp(argv[0], "/clear")) { ui_clear(u); u->scroll = 0; return; }
    if (!strcmp(argv[0], "/thinking")) {
        u->thinking = !u->thinking;
        ui_info(u, E_INFO, "thinking stream: %s", u->thinking ? "on" : "off");
        return;
    }
    if (!strcmp(argv[0], "/policy")) {
        if (argc >= 2) {
            mp_pmode m;
            if (mp_mode_parse(argv[1], &m)) { ui_error(u, "modes: ask | auto-read | full-auto"); return; }
            mp_policy_set_mode(u->policy, m);
            snprintf(g_cfg.policy_mode, sizeof g_cfg.policy_mode, "%s", argv[1]);
            mp_cfg_save(&g_cfg);
        }
        ui_info(u, E_INFO, "permission mode: %s", mp_mode_name(mp_policy_mode(u->policy)));
        return;
    }
    if (!strcmp(argv[0], "/provider")) {
        mp_provider *act = mp_cfg_active(&g_cfg);
        ui_info(u, E_INFO, "providers (%d), active: %s", g_cfg.n, g_cfg.active);
        for (int i = 0; i < g_cfg.n; i++)
            ui_info(u, E_INFO, "  %s%-12s %-13s %s  model=%s",
                    act == &g_cfg.v[i] ? "* " : "  ", g_cfg.v[i].name,
                    mp_kind_name(g_cfg.v[i].kind), g_cfg.v[i].base_url, g_cfg.v[i].model);
        return;
    }
    if (!strcmp(argv[0], "/model")) {
        if (argc >= 2) {
            mp_cfg_set_model(&g_cfg, argv[1]);
            ui_info(u, E_INFO, "model set: %s", argv[1]);
        } else {
            tui_models(u, NULL);
        }
        return;
    }
    if (!strcmp(argv[0], "/models")) { tui_models(u, argc >= 2 ? argv[1] : NULL); return; }
    if (!strcmp(argv[0], "/connect")) { tui_connect(u, argc >= 2 ? argv[1] : NULL); return; }
    if (!strcmp(argv[0], "/help")) {
        ui_info(u, E_INFO,
                "commands: /connect [preset]  /models [filter]  /model [id]  "
                "/provider  /policy [ask|auto-read|full-auto]  /thinking  /clear  /quit");
        ui_info(u, E_INFO,
                "keys: Enter send · arrows edit/history · Tab complete · "
                "PgUp/PgDn scroll · Ctrl-C clear/quit · Ctrl-D quit");
        return;
    }
    ui_error(u, "unknown command %s (try /help)", argv[0]);
}

/* --------------------------------------------------------------- main loop */
static void hist_add(tui *u, const char *text) {
    if (!text || !*text) return;
    if (u->nhist && !strcmp(u->hist[u->nhist - 1], text)) return;
    if (u->nhist >= 200) {
        free(u->hist[0]);
        memmove(u->hist, u->hist + 1, sizeof(char *) * (size_t)(u->nhist - 1));
        u->nhist--;
    }
    if (u->nhist == u->hcap) {
        int nc = u->hcap ? u->hcap * 2 : 32;
        char **nv = realloc(u->hist, sizeof(char *) * (size_t)nc);
        if (!nv) return;
        u->hist = nv; u->hcap = nc;
    }
    u->hist[u->nhist++] = strdup(text);
}
static void hist_load(tui *u) {
    if (!u->hist_file[0]) return;
    FILE *f = fopen(u->hist_file, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) hist_add(u, line);
    }
    fclose(f);
}
static void hist_save(tui *u) {
    if (!u->hist_file[0]) return;
    FILE *f = fopen(u->hist_file, "w");
    if (!f) return;
    for (int i = 0; i < u->nhist; i++) fprintf(f, "%s\n", u->hist[i]);
    fclose(f);
}
static void hist_recall(tui *u, int dir) {
    if (!u->nhist) return;
    if (dir < 0) {
        if (u->hist_pos < 0) u->hist_pos = u->nhist;
        if (u->hist_pos > 0) u->hist_pos--;
    } else {
        if (u->hist_pos < 0) return;
        if (u->hist_pos < u->nhist - 1) u->hist_pos++;
        else {
            u->hist_pos = -1;
            u->input[0] = 0; u->inlen = u->incur = 0;
            return;
        }
    }
    snprintf(u->input, sizeof u->input, "%s", u->hist[u->hist_pos]);
    u->inlen = strlen(u->input); u->incur = u->inlen;
}
static void submit_line(tui *u) {
    if (!u->inlen) return;
    char *line = strdup(u->input);
    if (!line) return;
    hist_add(u, line);
    u->input[0] = 0; u->inlen = u->incur = 0; u->hist_pos = -1;
    if (line[0] == '/') handle_slash(u, line);
    else tui_turn(u, line);
    free(line);
}
static void handle_key(tui *u, const struct tb_event *ev) {
    if (ev->type != TB_EVENT_KEY) return;
    if (ev->key == TB_KEY_CTRL_C) {
        if (u->inlen) { u->input[0] = 0; u->inlen = u->incur = 0; }
        else u->quit = 1;
        return;
    }
    if (ev->key == TB_KEY_CTRL_D) {
        static int saw_first_ctrl_d = 0;
        if (!saw_first_ctrl_d++) { saw_first_ctrl_d--; return; }
        if (!u->inlen) u->quit = 1;
        else input_delete(u);
        return;
    }
    switch (ev->key) {
        case TB_KEY_ENTER: submit_line(u); return;
        case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2: input_backspace(u); return;
        case TB_KEY_DELETE: input_delete(u); return;
        case TB_KEY_ARROW_LEFT:
            if (u->incur) { size_t p = u->incur - 1; while (p > 0 && ((unsigned char)u->input[p] & 0xC0) == 0x80) p--; u->incur = p; }
            return;
        case TB_KEY_ARROW_RIGHT:
            if (u->incur < u->inlen) { size_t nx = u->incur + 1; while (nx < u->inlen && ((unsigned char)u->input[nx] & 0xC0) == 0x80) nx++; u->incur = nx; }
            return;
        case TB_KEY_HOME: u->incur = 0; return;
        case TB_KEY_END: u->incur = u->inlen; return;
        case TB_KEY_ARROW_UP: hist_recall(u, -1); return;
        case TB_KEY_ARROW_DOWN: hist_recall(u, +1); return;
        case TB_KEY_PGUP:
            u->scroll += (tb_height() - 3) / 2;
            if (u->scroll > MAX_LINES) u->scroll = MAX_LINES;
            return;
        case TB_KEY_PGDN:
            u->scroll -= (tb_height() - 3) / 2;
            if (u->scroll < 0) u->scroll = 0;
            return;
        case TB_KEY_TAB: tab_complete(u); return;
        case TB_KEY_CTRL_U:
            u->input[0] = 0; u->inlen = u->incur = 0; return;
        case TB_KEY_CTRL_W: {
            size_t p = u->incur;
            while (p > 0 && (u->input[p-1] == ' ' || u->input[p-1] == '\t')) p--;
            while (p > 0 && u->input[p-1] != ' ' && u->input[p-1] != '\t') p--;
            memmove(u->input + p, u->input + u->incur, u->inlen - u->incur + 1);
            u->inlen -= u->incur - p;
            u->incur = p;
            return;
        }
        default: break;
    }
    if (ev->ch) {
        char tmp[8]; int n = tb_utf8_unicode_to_char(tmp, ev->ch);
        if (n > 0) input_insert(u, tmp, (size_t)n);
    }
}

int mp_tui_fullscreen(mp_agent *agent, const char *model, int thinking,
                      long max_tokens, const char *sessdir) {
    if (tb_init() != TB_OK) return MP_TUI_FALLBACK;
    tb_set_input_mode(TB_INPUT_ESC);
    tb_set_output_mode(TB_OUTPUT_NORMAL);

    tui u; memset(&u, 0, sizeof u);
    u.agent = agent;
    u.thinking = thinking;
    u.max_tokens = max_tokens > 0 ? max_tokens : 16384;
    u.ans_entry = u.rea_entry = -1;
    u.hist_pos = -1;
    snprintf(u.prompt, sizeof u.prompt, "> ");

    mp_cfg_load(&g_cfg, NULL);
    if (model && *model) snprintf(g_cfg.session_model, sizeof g_cfg.session_model, "%s", model);
    mp_pmode pm = MP_MODE_ASK_ALWAYS;
    mp_mode_parse(g_cfg.policy_mode, &pm);
    u.policy = mp_policy_new(pm, 1);
    if (!u.policy) { tb_shutdown(); return MP_TUI_FALLBACK; }

    const char *mdir = sessdir;
    char sessbuf[4096];
    if (!mdir || !*mdir) {
        const char *base = mp_minipi_dir();
        if (base) { snprintf(sessbuf, sizeof sessbuf, "%s/sessions", base); mdir = sessbuf; }
    }
    if (mdir && *mdir) u.store = mp_store_open(mdir, "tui-current");
    const char *base = mp_minipi_dir();
    if (base) snprintf(u.hist_file, sizeof u.hist_file, "%s/tui-history", base);
    hist_load(&u);

    if (u.store) snprintf(u.hint, sizeof u.hint, "session: %s", mp_store_path(u.store));
    int errs = 0;
    for (;;) {
        render(&u);
        struct tb_event ev;
        if (tb_poll_event(&ev) != TB_OK) {
            if (++errs > 100) break;      /* terminal went away */
            usleep(10000);
            continue;
        }
        errs = 0;
        if (ev.type == TB_EVENT_RESIZE) continue;
        handle_key(&u, &ev);
        if (u.quit) break;
    }

    hist_save(&u);
    if (u.store) { mp_store_checkpoint(u.store); mp_store_close(u.store); }
    mp_policy_free(u.policy);
    ui_clear(&u);
    free(u.ent);
    for (int i = 0; i < u.nhist; i++) free(u.hist[i]);
    free(u.hist);
    tb_shutdown();
    return 0;
}
