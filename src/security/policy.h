#ifndef MINIPI_SECURITY_POLICY_H
#define MINIPI_SECURITY_POLICY_H
/* Permission gate + harmful-action flagging (see docs/RESEARCH.md sections 3-4).
 * Design rules (mirroring Claude Code / Codex CLI / Gemini CLI consensus):
 *  - Every tool call is checked BEFORE execution; nothing runs silently.
 *  - HIGH risk is flagged regardless of mode; in full-auto it still blocks.
 *  - In interactive mode the user is asked (y once / a always / n deny /
 *    d deny+note). Denials are returned to the model as tool results so it
 *    can adapt instead of retrying blindly.
 *  - Non-interactive (--prompt) fails closed: side-effecting tools do not run
 *    unless --auto-approve was passed explicitly. */
#include "cJSON.h"

typedef enum { MP_RISK_NONE = 0, MP_RISK_LOW, MP_RISK_MED, MP_RISK_HIGH } mp_risk;
typedef enum { MP_MODE_ASK_ALWAYS = 0, MP_MODE_AUTO_READ, MP_MODE_FULL_AUTO } mp_pmode;
typedef enum { MP_ALLOW, MP_DENY, MP_NEED_PROMPT } mp_verdict;

/* Categories (static strings): destructive-shell, privilege-escalation,
 * remote-code-exec, secrets-access, exfiltration, prompt-injection, path-escape, none. */

/* Scan free text (user input or serialized tool call) for harmful patterns.
 * Returns risk level; sets *cat to a static category name. */
mp_risk mp_policy_scan(const char *text, const char **cat);

/* 1 if the tool has no side effects ("read"). */
int mp_policy_tool_is_readonly(const char *tool_name);

const char *mp_risk_name(mp_risk r);
const char *mp_mode_name(mp_pmode m);
int mp_mode_parse(const char *s, mp_pmode *out);   /* "ask"|"auto-read"|"full-auto" */

typedef struct mp_policy mp_policy;
mp_policy *mp_policy_new(mp_pmode mode, int interactive);
void       mp_policy_free(mp_policy *p);
mp_pmode   mp_policy_mode(mp_policy *p);
void       mp_policy_set_mode(mp_policy *p, mp_pmode m);
/* Decide a pending tool call. name + args produce a canonical preview string
 * (also used for the prompt). Sets *risk and *cat. Grants ("a") persist per
 * tool name for the session. */
mp_verdict mp_policy_check(mp_policy *p, const char *name, const cJSON *args,
                           char *preview, size_t preview_sz,
                           mp_risk *risk, const char **cat);
/* Record the human's answer from the REPL prompt ("y"/"a"/"n"/"d"). */
void mp_policy_record_answer(mp_policy *p, const char *name, char answer);

#endif
