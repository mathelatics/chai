#ifndef MINIPI_TOOLS_OUTPUT_CAP_H
#define MINIPI_TOOLS_OUTPUT_CAP_H
/* C-2 (GAP §5 / pi-coding-agent output capping): cap tool results BEFORE they
 * re-enter model context. Unbounded outputs blow the token budget and can be
 * used for prompt-injection stuffing. Policy: keep head AND tail (the most
 * informative parts of logs), elide the middle with an explicit marker so the
 * model knows content was dropped and how much. Caps:
 *   - bytes (default 64 KiB)
 *   - lines (default 800, per half)
 *   - line width (default 400 chars; long lines get '…' mid-cut)
 * UTF-8-safe: never splits a multibyte codepoint, never emits control bytes
 * beyond \n/\t (reuses the C-1 sanitizer state machine). */
#include <stddef.h>

#define MP_CAP_DEFAULT_BYTES 65536
#define MP_CAP_DEFAULT_LINES 800      /* kept per half (head & tail) */
#define MP_CAP_DEFAULT_WIDTH 400

/* Returns a malloc'd capped copy of `text` (len==SIZE_MAX => strlen). If no
 * capping was needed returns an exact duplicate. The marker line looks like:
 *   [... 1234 bytes / 56 lines elided by minipi output cap ...]
 * placed between the kept head and tail. */
char *mp_output_cap(const char *text, size_t len,
                    size_t max_bytes, int max_lines_per_half, int max_width);
#endif
