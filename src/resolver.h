#ifndef SIGIL_RESOLVER_H
#define SIGIL_RESOLVER_H

#include "ast.h"
#include "algebra.h"
#include "traits.h"
#include "types.h"
#include "errors.h"

/* ── Overload Resolution ─────────────────────────────────────────── */

typedef struct {
    Arena *arena;
    InternTable *intern_tab;
    AlgebraRegistry *algebra_reg;
    TraitRegistry *trait_reg;
    TypeChecker *type_checker;
    ErrorList *errors;
} Resolver;

void resolver_init(Resolver *r, Arena *arena, InternTable *intern_tab,
                   AlgebraRegistry *algebra_reg, TraitRegistry *trait_reg,
                   TypeChecker *type_checker, ErrorList *errors);

/* Resolve overloaded calls in the AST. */
bool resolve_ast(Resolver *r, ASTNode *node);

/* Resolve a single sigil to its concrete binding given argument types. */
SigilBinding *resolve_sigil(Resolver *r, AlgebraEntry *alg,
                            const char *sigil, Fixity fixity,
                            TypeRef **arg_types, int arg_count);

/* Resolve generic type variables to concrete types. */
TypeRef *resolve_generic(Resolver *r, TypeRef *generic_type,
                         const char *type_var, TypeRef *concrete_type);

#endif /* SIGIL_RESOLVER_H */
