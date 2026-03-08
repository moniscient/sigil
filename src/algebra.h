#ifndef SIGIL_ALGEBRA_H
#define SIGIL_ALGEBRA_H

#include "ast.h"
#include "errors.h"

/* ── Sigil Binding ───────────────────────────────────────────────── */

typedef struct {
    const char *fn_name;
    const char *sigil;       /* primary sigil (e.g., "+") */
    Fixity fixity;
    int param_count;
    TypeRef **param_types;   /* array of param_count types */
    TypeRef *return_type;
    StrList all_sigils;      /* all sigils in the pattern (for compound matching) */
    PatList pattern;         /* full pattern for compound matching */
    ASTNode *fn_node;        /* back-reference to the fn decl node */
} SigilBinding;

DA_TYPEDEF(SigilBinding, SigilBindingList)

/* ── Precedence Table ────────────────────────────────────────────── */

typedef struct {
    const char **sigils;   /* low to high */
    int count;
} PrecedenceTable;

/* ── Algebra Registry ────────────────────────────────────────────── */

typedef struct AlgebraEntry {
    const char *name;
    SigilBindingList bindings;
    PrecedenceTable precedence;
    NodeList trait_decls;
    NodeList implement_blocks;
} AlgebraEntry;

DA_TYPEDEF(AlgebraEntry*, AlgebraList)

typedef struct {
    AlgebraList algebras;
    Arena *arena;
    InternTable *intern_tab;
} AlgebraRegistry;

void algebra_registry_init(AlgebraRegistry *r, Arena *arena, InternTable *intern_tab);
AlgebraEntry *algebra_registry_add(AlgebraRegistry *r, const char *name);
AlgebraEntry *algebra_registry_find(AlgebraRegistry *r, const char *name);

/* Register fn declarations from a parsed algebra node into the registry. */
void algebra_register_declarations(AlgebraRegistry *r, AlgebraEntry *alg, ASTNode *algebra_node);

/* Look up a sigil binding in an algebra. */
SigilBinding *algebra_find_sigil(AlgebraEntry *alg, const char *sigil, Fixity fixity);

/* Get precedence level for a sigil (0 = lowest). Returns -1 if not in table. */
int algebra_get_precedence(AlgebraEntry *alg, const char *sigil);

/* Try to match a compound sigil pattern against a sequence of tokens. */
SigilBinding *algebra_match_compound(AlgebraEntry *alg, const char **sigils, int count);

/* Check for collision rule violations: same sigil + fixity + param types = error.
 * Returns true if no collisions found. */
bool algebra_check_collisions(AlgebraEntry *alg, ErrorList *errors);

#endif /* SIGIL_ALGEBRA_H */
