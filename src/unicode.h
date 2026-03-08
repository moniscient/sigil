#ifndef SIGIL_UNICODE_H
#define SIGIL_UNICODE_H

#include "common.h"

typedef enum {
    CHARCLASS_ASCII_ALNUM,    /* a-z A-Z 0-9 _ */
    CHARCLASS_ASCII_SYMBOL,   /* printable ASCII non-alnum */
    CHARCLASS_UNICODE,        /* codepoints > U+007F */
    CHARCLASS_WHITESPACE,     /* space, tab, newline, etc. */
    CHARCLASS_EOF
} CharClass;

/* Decode one UTF-8 codepoint. Returns bytes consumed, writes codepoint to *cp. */
int utf8_decode(const char *s, uint32_t *cp);

/* Encode one codepoint to UTF-8. Returns bytes written. buf must have 4+ bytes. */
int utf8_encode(uint32_t cp, char *buf);

/* Classify a codepoint. */
CharClass charclass_of(uint32_t cp);

/* Is the codepoint a digit? */
bool is_digit(uint32_t cp);

/* Is the codepoint the start of a number (digit or leading sign followed by digit)? */
bool is_number_start(uint32_t cp);

#endif /* SIGIL_UNICODE_H */
