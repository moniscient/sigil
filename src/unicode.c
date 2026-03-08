#include "unicode.h"

int utf8_decode(const char *s, uint32_t *cp) {
    const uint8_t *b = (const uint8_t *)s;
    if (b[0] == 0) { *cp = 0; return 0; }
    if (b[0] < 0x80) { *cp = b[0]; return 1; }
    if ((b[0] & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(b[0] & 0x1F) << 6) | (b[1] & 0x3F);
        return 2;
    }
    if ((b[0] & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(b[0] & 0x0F) << 12) | ((uint32_t)(b[1] & 0x3F) << 6) | (b[2] & 0x3F);
        return 3;
    }
    if ((b[0] & 0xF8) == 0xF0) {
        *cp = ((uint32_t)(b[0] & 0x07) << 18) | ((uint32_t)(b[1] & 0x3F) << 12) |
              ((uint32_t)(b[2] & 0x3F) << 6) | (b[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

int utf8_encode(uint32_t cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

CharClass charclass_of(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') return CHARCLASS_WHITESPACE;
    if (cp == 0) return CHARCLASS_EOF;
    if (cp <= 0x7F) {
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
            (cp >= '0' && cp <= '9') || cp == '_')
            return CHARCLASS_ASCII_ALNUM;
        return CHARCLASS_ASCII_SYMBOL;
    }
    return CHARCLASS_UNICODE;
}

bool is_digit(uint32_t cp) {
    return cp >= '0' && cp <= '9';
}

bool is_number_start(uint32_t cp) {
    return is_digit(cp);
}
