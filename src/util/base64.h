#ifndef MINIPI_UTIL_BASE64_H
#define MINIPI_UTIL_BASE64_H
#include <stddef.h>
/* Returns bytes written (excl. NUL) or -1 on overflow/short input. */
int mp_b64_encode(const unsigned char *in, size_t n, char *out, size_t out_cap);
/* Returns decoded length or -1 on invalid input. */
int mp_b64_decode(const char *in, size_t n, unsigned char *out, size_t out_cap);
#endif
