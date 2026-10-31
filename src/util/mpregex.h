/* util/mpregex.h — tiny regex engine shared by the grep tool (C-13).
 *
 * Motivation: POSIX <regex.h> is absent from MSVC/MinGW, which blocked the
 * grep tool on Windows. This engine supports exactly the subset our tool
 * documents and tests rely on:
 *   literals, . (any byte), character classes [abc] [a-z0-9] [^abc],
 *   alternation a|b, groups (...), quantifiers * + ? {n} {n,m} on atoms,
 *   anchors ^ $ at any nesting level.
 * Matching semantics: leftmost match anywhere in the subject (unless ^),
 * case-sensitive or case-folded via flag. Backtracking matcher; patterns are
 * small (<=256 chars) and lines bounded, so worst-case cost is acceptable
 * for a tool that already caps matches/files.
 *
 * API returns 0 = match, 1 = no match, -1 = bad pattern. */
#ifndef MINIPI_MPREGEX_H
#define MINIPI_MPREGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mp_regex mp_regex;

mp_regex *mp_regcomp(const char *pattern, int icase);
int       mp_regexec(const mp_regex *re, const char *text);
void      mp_regfree(mp_regex *re);

#ifdef __cplusplus
}
#endif
#endif /* MINIPI_MPREGEX_H */
