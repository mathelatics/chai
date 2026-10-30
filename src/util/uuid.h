#ifndef MINIPI_UTIL_UUID_H
#define MINIPI_UTIL_UUID_H
#include <stddef.h>
/* UUID v4 from getrandom()/\/dev\/urandom (CSPRNG). Writes 36 chars + NUL.
 * Returns 0 on success, -1 if no entropy source available. */
int mp_uuid_v4(char out[37]);
#endif
