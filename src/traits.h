#ifndef SIGIL_TRAITS_H
#define SIGIL_TRAITS_H

#include "ast.h"
#include "algebra.h"
#include "errors.h"

/* ── Trait Registry ──────────────────────────────────────────────── */

typedef struct TraitDef {
    const char *name;
    const char *type_var;
    const char *source_algebra; /* algebra that declared this trait */
    NodeList method_sigs;       /* NODE_FN_DECL signatures */
    StrList requires;           /* prerequisite trait names */
    StrList required_identities; /* fn names that require identity declarations */
} TraitDef;

/* Identity binding: constant expression as identity element for a function */
typedef struct {
    const char *fn_name;
    ASTNode *value;
} IdentityEntry;

typedef struct TraitImpl {
    const char *trait_name;
    const char *concrete_type;
    const char *source_algebra; /* algebra that declared this implementation */
    NodeList methods;           /* concrete method implementations */
    IdentityEntry *identities;  /* identity declarations */
    int identity_count;
} TraitImpl;

DA_TYPEDEF(TraitDef*, TraitDefList)
DA_TYPEDEF(TraitImpl*, TraitImplList)

/* Type ownership: tracks which algebra defines each user type */
typedef struct TypeOwner {
    const char *type_name;
    const char *algebra_name;
    struct TypeOwner *next;
} TypeOwner;

typedef struct TraitRegistry {
    TraitDefList defs;
    TraitImplList impls;
    TypeOwner *type_owners;   /* linked list of type→algebra mappings */
    Arena *arena;
    InternTable *intern_tab;
    ErrorList *errors;
} TraitRegistry;

void trait_registry_init(TraitRegistry *tr, Arena *arena, InternTable *intern_tab, ErrorList *errors);

/* Register trait declarations and implementations from an algebra. */
void trait_register_from_algebra(TraitRegistry *tr, AlgebraEntry *alg);

/* Register a trait definition. */
void trait_register_def(TraitRegistry *tr, ASTNode *trait_node);

/* Register a trait implementation. */
void trait_register_impl(TraitRegistry *tr, ASTNode *impl_node);

/* Find a trait definition by name. */
TraitDef *trait_find_def(TraitRegistry *tr, const char *name);

/* Find an implementation of a trait for a concrete type. */
TraitImpl *trait_find_impl(TraitRegistry *tr, const char *trait_name, const char *type_name);

/* Check that all prerequisites are satisfied. Returns true if ok. */
bool trait_check_prerequisites(TraitRegistry *tr, const char *trait_name, const char *type_name);

/* Check all implementations for completeness and orphan rule violations. */
bool trait_check_all(TraitRegistry *tr);

/* Register that a type is defined by an algebra. */
void trait_register_type_owner(TraitRegistry *tr, const char *type_name, const char *algebra_name);

/* Find which algebra defines a type. Returns NULL if unknown/builtin. */
const char *trait_find_type_owner(TraitRegistry *tr, const char *type_name);

/* Check if a type implements a given trait. */
bool trait_type_has_trait(TraitRegistry *tr, const char *type_name, const char *trait_name);

#endif /* SIGIL_TRAITS_H */
