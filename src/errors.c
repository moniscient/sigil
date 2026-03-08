#include "errors.h"
#include <stdarg.h>

void error_list_init(ErrorList *el, Arena *arena) {
    el->first = el->last = NULL;
    el->count = 0;
    el->arena = arena;
}

void error_add(ErrorList *el, ErrorKind kind, SrcLoc loc, const char *fmt, ...) {
    SigilError *e = (SigilError *)arena_alloc(el->arena, sizeof(SigilError));
    e->next = NULL;
    e->kind = kind;
    e->loc = loc;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
    if (el->last) el->last->next = e;
    else el->first = e;
    el->last = e;
    el->count++;
}

static const char *error_kind_str(ErrorKind k) {
    switch (k) {
        case ERR_LEXER:   return "lexer";
        case ERR_PARSER:  return "parser";
        case ERR_DESUGAR: return "desugar";
        case ERR_TYPE:    return "type";
        case ERR_TRAIT:   return "trait";
        case ERR_RESOLVE: return "resolve";
    }
    return "unknown";
}

void error_print_all(const ErrorList *el) {
    for (const SigilError *e = el->first; e; e = e->next) {
        fprintf(stderr, "%s:%d:%d: %s error: %s\n",
                e->loc.file ? e->loc.file : "<input>",
                e->loc.line, e->loc.col,
                error_kind_str(e->kind), e->message);
    }
}

bool error_has_errors(const ErrorList *el) {
    return el->count > 0;
}
