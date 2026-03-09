#ifndef SIGIL_TYPES_H
#define SIGIL_TYPES_H

#include "ast.h"
#include "errors.h"

/* ── Type Environment ────────────────────────────────────────────── */

typedef struct TypeBinding {
    const char *name;
    TypeRef *type;
    bool is_mutable;
    struct TypeBinding *next;
} TypeBinding;

typedef struct TypeEnv {
    TypeBinding *bindings;
    struct TypeEnv *parent;
    Arena *arena;
} TypeEnv;

/* ── Function Registry (for type inference through calls) ────────── */

typedef struct FnEntry {
    const char *name;
    int param_count;
    TypeRef **param_types;
    bool *param_mutable;     /* which params are var (mutable ref) */
    TypeRef *return_type;
    bool has_repeats;        /* true if fn pattern contains PAT_REPEATS */
    int repeats_start_idx;   /* param index where repeats args begin */
    struct FnEntry *next;
} FnEntry;

/* ── User-Defined Type Registry ──────────────────────────────────── */

typedef struct TypeDef {
    const char *name;
    int field_count;
    TypeRef **field_types;
    const char **field_names;
    struct TypeDef *next;
} TypeDef;

typedef struct {
    Arena *arena;
    InternTable *intern_tab;
    ErrorList *errors;
    TypeEnv *global_env;
    FnEntry *fn_registry;    /* linked list of known fn declarations */
    TypeRef *current_fn_return_type; /* return type of the fn currently being checked */
    TypeDef *type_registry;  /* linked list of user-defined types */
} TypeChecker;

void type_checker_init(TypeChecker *tc, Arena *arena, InternTable *intern_tab, ErrorList *errors);

/* Register a fn declaration so its return type is available for call inference. */
void type_checker_register_fn(TypeChecker *tc, ASTNode *fn_node);

/* Type check an AST. Returns false if errors were found. */
bool type_check(TypeChecker *tc, ASTNode *node);

/* Type equality check. */
bool types_equal(TypeRef *a, TypeRef *b);

/* Check if type b is compatible with type a (b can be used where a is expected).
 * Handles generics: a generic type is compatible with any concrete type. */
bool type_compatible(TypeRef *a, TypeRef *b);

/* Create primitive type references. */
TypeRef *make_type(Arena *a, TypeKind kind);
TypeRef *make_named_type(Arena *a, const char *name);
TypeRef *make_generic_type(Arena *a, const char *name);
TypeRef *make_fn_type(Arena *a, int param_count, TypeRef **param_types, TypeRef *return_type);

/* Look up a variable's type in the environment. */
TypeRef *type_env_lookup(TypeEnv *env, const char *name);

/* Look up a variable's mutability in the environment. */
bool type_env_is_mutable(TypeEnv *env, const char *name);

/* Bind a name in the environment. */
void type_env_bind(TypeEnv *env, const char *name, TypeRef *type, bool is_mutable);

/* Create a child environment. */
TypeEnv *type_env_push(TypeEnv *parent);

/* Generic type variable unification: given a fn's param types and concrete arg types,
 * determine the concrete type for each type variable. Returns the resolved return type. */
TypeRef *unify_generic_return(Arena *arena, int param_count, TypeRef **param_types,
                              TypeRef **arg_types, TypeRef *return_type);

/* Look up a user-defined type by name. */
TypeDef *type_def_lookup(TypeChecker *tc, const char *name);

#endif /* SIGIL_TYPES_H */
