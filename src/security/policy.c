/* Permission gate + harmful-action scanner. Pure heuristics, offline-first:
 * no safety API required (an external guard can be layered later via the same
 * openai-local provider path — see docs/RESEARCH.md §4). */
#include "security/policy.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <khash.h>

KHASH_SET_INIT_STR(grantset)

struct mp_policy {
    mp_pmode mode;
    int interactive;               /* 0 => fail closed on anything non-readonly */
    khash_t(grantset) *always;     /* session-scoped "a" grants by tool name */
};

const char *mp_risk_name(mp_risk r) {
    switch (r) { case MP_RISK_HIGH: return "HIGH"; case MP_RISK_MED: return "MED";
                 case MP_RISK_LOW: return "LOW"; default: return "none"; }
}
const char *mp_mode_name(mp_pmode m) {
    switch (m) { case MP_MODE_AUTO_READ: return "auto-read";
                 case MP_MODE_FULL_AUTO: return "full-auto";
                 default: return "ask-always"; }
}
int mp_mode_parse(const char *s, mp_pmode *out) {
    if (!s) return -1;
    if (!strcmp(s, "ask") || !strcmp(s, "ask-always")) { *out = MP_MODE_ASK_ALWAYS; return 0; }
    if (!strcmp(s, "auto-read")) { *out = MP_MODE_AUTO_READ; return 0; }
    if (!strcmp(s, "full-auto")) { *out = MP_MODE_FULL_AUTO; return 0; }
    return -1;
}

/* ---- pattern table ------------------------------------------------------ */
typedef struct { const char *pat; mp_risk risk; const char *cat; } rule;
/* matched case-insensitively against the raw text of the action/input */
static const char *RX(const char *s, char *buf, size_t n) { /* lower-copy helper */
    size_t i = 0; for (; s[i] && i + 1 < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[i] = '\0'; return buf; }

/* collapse whitespace around shell pipes so "curl|sh", "curl  |  sh" etc. all
 * normalize to "curl | sh" before pattern matching */
static void norm_pipes(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '|') {
            /* back up over spaces already written */
            while (w > s && w[-1] == ' ') w--;
            *w++ = ' '; *w++ = '|'; *w++ = ' ';
            r++;
            while (*r == ' ') r++;
        } else { *w++ = *r++; }
    }
    *w = '\0';
}

static const struct { const char *pat; mp_risk r; const char *cat; } RULES[] = {
    /* destructive shell */
    {"rm -rf /",        MP_RISK_HIGH, "destructive-shell"},
    {"rm -fr /",        MP_RISK_HIGH, "destructive-shell"},
    {"rm -rf ~",        MP_RISK_HIGH, "destructive-shell"},
    {"mkfs",            MP_RISK_HIGH, "destructive-shell"},
    {"dd if=",          MP_RISK_HIGH, "destructive-shell"},
    {"dd of=/dev",      MP_RISK_HIGH, "destructive-shell"},
    {":(){ :|:& };:",   MP_RISK_HIGH, "destructive-shell"},
    {"> /dev/sd",       MP_RISK_HIGH, "destructive-shell"},
    {"chmod -R 777 /",  MP_RISK_HIGH, "destructive-shell"},
    {"shutdown",        MP_RISK_HIGH, "destructive-shell"},
    {"reboot",          MP_RISK_HIGH, "destructive-shell"},
    {"git push --force",MP_RISK_MED,  "destructive-shell"},
    {"git reset --hard",MP_RISK_MED,  "destructive-shell"},
    {"drop table",      MP_RISK_MED,  "destructive-shell"},
    {"truncate -s 0",   MP_RISK_MED,  "destructive-shell"},
    /* privilege escalation */
    {"sudo ",           MP_RISK_MED,  "privilege-escalation"},
    {"su -",            MP_RISK_MED,  "privilege-escalation"},
    {"chown root",      MP_RISK_MED,  "privilege-escalation"},
    {"setuid",          MP_RISK_HIGH, "privilege-escalation"},
    /* remote code execution */
    {"curl | sh",       MP_RISK_HIGH, "remote-code-exec"},
    {"curl | bash",     MP_RISK_HIGH, "remote-code-exec"},
    {"wget | sh",       MP_RISK_HIGH, "remote-code-exec"},
    {"wget | bash",     MP_RISK_HIGH, "remote-code-exec"},
    {"| sh",            MP_RISK_MED,  "remote-code-exec"},
    {"| bash",          MP_RISK_MED,  "remote-code-exec"},
    {"eval(",           MP_RISK_LOW,  "remote-code-exec"},
    /* secrets access */
    {"~/.ssh",          MP_RISK_HIGH, "secrets-access"},
    {"/.ssh/",          MP_RISK_HIGH, "secrets-access"},
    {"id_rsa",          MP_RISK_HIGH, "secrets-access"},
    {".env",            MP_RISK_MED,  "secrets-access"},
    {"api_key",         MP_RISK_MED,  "secrets-access"},
    {"apikey",          MP_RISK_MED,  "secrets-access"},
    {"authorization: bearer", MP_RISK_MED, "secrets-access"},
    {"/etc/shadow",     MP_RISK_HIGH, "secrets-access"},
    {"credentials.json",MP_RISK_HIGH, "secrets-access"},
    /* exfiltration */
    {"webhook.site",    MP_RISK_HIGH, "exfiltration"},
    {"requestbin",      MP_RISK_HIGH, "exfiltration"},
    {"ngrok.io",        MP_RISK_MED,  "exfiltration"},
    {"curl -x ",        MP_RISK_HIGH, "exfiltration"},
    {"nc -e",           MP_RISK_HIGH, "exfiltration"},
    /* prompt injection markers (in model output or fetched content) */
    {"ignore previous instructions", MP_RISK_HIGH, "prompt-injection"},
    {"ignore all previous",          MP_RISK_HIGH, "prompt-injection"},
    {"disregard your system prompt", MP_RISK_HIGH, "prompt-injection"},
    {"you are now dan",              MP_RISK_HIGH, "prompt-injection"},
    {"jailbreak",                    MP_RISK_MED,  "prompt-injection"},
    {"do anything now",              MP_RISK_MED,  "prompt-injection"},
    {NULL, MP_RISK_NONE, NULL}
};

mp_risk mp_policy_scan(const char *text, const char **cat) {
    if (cat) *cat = "none";
    if (!text || !*text) return MP_RISK_NONE;
    char low[8192];
    /* scan in windows so long texts still match */
    mp_risk worst = MP_RISK_NONE;
    const char *worst_cat = "none";
    size_t tl = strlen(text);
    for (size_t off = 0; off < tl; off += 4096) {
        size_t chunk = tl - off > 4000 ? 4000 : tl - off;
        char w[8192];
        memcpy(w, text + off, chunk); w[chunk] = '\0';
        RX(w, low, sizeof low);
        norm_pipes(low);
        for (int i = 0; RULES[i].pat; i++) {
            if (strstr(low, RULES[i].pat)) {
                if (RULES[i].r > worst) { worst = RULES[i].r; worst_cat = RULES[i].cat; }
            }
        }
        if (off + chunk >= tl) break;
    }
    if (cat) *cat = worst_cat;
    return worst;
}

int mp_policy_tool_is_readonly(const char *tool_name) {
    /* read-only file-inspection tools: no side effects, safe for auto-read */
    return tool_name && (!strcmp(tool_name, "read") || !strcmp(tool_name, "ls") ||
                         !strcmp(tool_name, "find") || !strcmp(tool_name, "grep"));
}

/* ---- policy object ------------------------------------------------------- */
mp_policy *mp_policy_new(mp_pmode mode, int interactive) {
    mp_policy *p = calloc(1, sizeof *p);
    p->mode = mode; p->interactive = interactive;
    p->always = kh_init(grantset);
    return p;
}
void mp_policy_free(mp_policy *p) {
    if (!p) return;
    khint_t k;
    for (k = kh_begin(p->always); k != kh_end(p->always); ++k)
        if (kh_exist(p->always, k)) free((void *)kh_key(p->always, k));
    kh_destroy(grantset, p->always);
    free(p);
}
mp_pmode mp_policy_mode(mp_policy *p) { return p ? p->mode : MP_MODE_ASK_ALWAYS; }
void mp_policy_set_mode(mp_policy *p, mp_pmode m) { if (p) p->mode = m; }

static int has_grant(mp_policy *p, const char *name) {
    khint_t k = kh_get(grantset, p->always, name);
    return k != kh_end(p->always);
}
void mp_policy_record_answer(mp_policy *p, const char *name, char answer) {
    if (!p || !name) return;
    if (answer == 'a') {
        int ret; char *dup = strdup(name);
        kh_put(grantset, p->always, dup, &ret);
        if (ret <= 0) free(dup); /* already present or error */
    }
}

mp_verdict mp_policy_check(mp_policy *p, const char *name, const cJSON *args,
                           char *preview, size_t preview_sz,
                           mp_risk *risk, const char **cat) {
    if (!p || !name) { if (risk) *risk = MP_RISK_HIGH; if (cat) *cat = "policy-error"; return MP_DENY; }
    char *aj = args ? cJSON_PrintUnformatted(args) : NULL;
    if (preview && preview_sz)
        snprintf(preview, preview_sz, "%s %s", name, aj ? aj : "{}");
    mp_risk r = aj ? mp_policy_scan(aj, cat) : MP_RISK_NONE;
    if (name && strstr(name, "..")) { r = MP_RISK_HIGH; if (cat) *cat = "path-escape"; }
    free(aj);
    if (risk) *risk = r;

    int readonly = mp_policy_tool_is_readonly(name);

    /* HIGH risk is never auto-run, even in full-auto (fail closed hard). */
    if (r == MP_RISK_HIGH) {
        if (p->interactive) return MP_NEED_PROMPT;   /* user may still allow once */
        return MP_DENY;                              /* non-interactive: blocked */
    }
    switch (p->mode) {
    case MP_MODE_FULL_AUTO:
        return MP_ALLOW;                              /* MED/LOW run; audit-logged */
    case MP_MODE_AUTO_READ:
        if (readonly && r == MP_RISK_NONE) return MP_ALLOW;
        return p->interactive ? MP_NEED_PROMPT : MP_DENY;
    case MP_MODE_ASK_ALWAYS:
    default:
        if (has_grant(p, name) && r <= MP_RISK_LOW) return MP_ALLOW;
        return p->interactive ? MP_NEED_PROMPT : MP_DENY;
    }
}
