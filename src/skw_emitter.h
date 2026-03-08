#ifndef SIGIL_SKW_EMITTER_H
#define SIGIL_SKW_EMITTER_H

#include "ast.h"
#include <stdio.h>

typedef struct {
    FILE *out;
    int indent;
} SkwEmitter;

void skw_emitter_init(SkwEmitter *e, FILE *out);
void skw_emit(SkwEmitter *e, ASTNode *node);

/* Emit to a string buffer instead of FILE. Returns malloc'd string. */
char *skw_emit_to_string(ASTNode *node);

#endif /* SIGIL_SKW_EMITTER_H */
