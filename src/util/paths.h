#ifndef MINIPI_UTIL_PATHS_H
#define MINIPI_UTIL_PATHS_H
/* Home/config resolution with an explicit, loud fallback order:
 *   1. $MINIPI_HOME (if set and absolute)
 *   2. $HOME        (if set and absolute)
 *   3. NULL         — callers MUST log and fail visibly; we never silently
 *                     fall back to /tmp or "." (that bug hid session writes). */
const char *mp_home_dir(void);            /* NULL if unusable; reason in mp_home_error() */
const char *mp_home_error(void);          /* human-readable reason for NULL above */
/* mkdir -p semantics; returns 0 ok, -1 with errno preserved (logged). */
int mp_mkdir_p(const char *path);
/* Build "<home>/.minipi" (creates nothing). Returns static buffer or NULL. */
const char *mp_minipi_dir(void);
#endif
