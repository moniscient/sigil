#ifndef SIGIL_C_EMITTER_H
#define SIGIL_C_EMITTER_H

#include "ast.h"
#include "types.h"
#include <stdio.h>

/* Monomorphization instance: one specialization of a generic fn */
typedef struct MonoInstance {
    ASTNode *fn_node;            /* original generic fn AST */
    const char *fn_name;         /* e.g., "identity" */
    int type_var_count;
    const char **type_var_names; /* ["T"] or ["T", "U"] */
    TypeRef **concrete_types;    /* [TYPE_INT] or [TYPE_INT, TYPE_FLOAT] */
    struct MonoInstance *next;
} MonoInstance;

typedef struct {
    FILE *out;
    int indent;
    TypeChecker *tc;
    Arena *arena;
    const char **var_params;   /* var param names for current fn */
    int var_param_count;
    bool in_implement;         /* true when emitting implement block methods */
    const char *implement_type; /* concrete type name for implement block */
    MonoInstance *mono_instances; /* linked list of all generic specializations */
    MonoInstance *current_mono;   /* non-NULL when emitting a mono specialization */
} CEmitter;

void c_emitter_init(CEmitter *e, FILE *out, TypeChecker *tc, Arena *arena);
void c_emit(CEmitter *e, ASTNode *node);

#endif /* SIGIL_C_EMITTER_H */
