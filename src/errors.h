#ifndef SIGIL_ERRORS_H
#define SIGIL_ERRORS_H

#include "common.h"

typedef enum {
    ERR_LEXER,
    ERR_PARSER,
    ERR_DESUGAR,
    ERR_TYPE,
    ERR_TRAIT,
    ERR_RESOLVE
} ErrorKind;

typedef struct SigilError {
    struct SigilError *next;
    ErrorKind kind;
    SrcLoc loc;
    char message[512];
} SigilError;

typedef struct {
    SigilError *first;
    SigilError *last;
    int count;
    Arena *arena;
} ErrorList;

void error_list_init(ErrorList *el, Arena *arena);
void error_add(ErrorList *el, ErrorKind kind, SrcLoc loc, const char *fmt, ...);
void error_print_all(const ErrorList *el);
bool error_has_errors(const ErrorList *el);

#endif /* SIGIL_ERRORS_H */
