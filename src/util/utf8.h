#ifndef MINIPI_UTIL_UTF8_H
#define MINIPI_UTIL_UTF8_H
#include <stddef.h>
/* Strict UTF-8 validation (rejects overlongs, surrogates, >U+10FFFF, NUL). */
int  mp_utf8_valid(const char *s, size_t n);
size_t mp_utf8_len(const char *s, size_t n);      /* codepoints; 0 if invalid */
size_t mp_utf8_cp_bytes(unsigned char lead);       /* 0 => invalid lead/continuation */
#endif
