#include "algebra.h"
#include "types.h"
#include <string.h>

void algebra_registry_init(AlgebraRegistry *r, Arena *arena, InternTable *intern_tab) {
    da_init(&r->algebras);
    r->arena = arena;
    r->intern_tab = intern_tab;
}

AlgebraEntry *algebra_registry_add(AlgebraRegistry *r, const char *name) {
    AlgebraEntry *e = (AlgebraEntry *)arena_alloc(r->arena, sizeof(AlgebraEntry));
    e->name = intern_cstr(r->intern_tab, name);
    da_init(&e->bindings);
    e->precedence.sigils = NULL;
    e->precedence.count = 0;
    da_init(&e->trait_decls);
    da_init(&e->implement_blocks);
    da_push(&r->algebras, e);
    return e;
}

AlgebraEntry *algebra_registry_find(AlgebraRegistry *r, const char *name) {
    const char *interned = intern_cstr(r->intern_tab, name);
    for (int i = 0; i < r->algebras.count; i++) {
        if (r->algebras.items[i]->name == interned)
            return r->algebras.items[i];
    }
    return NULL;
}

static void register_fn_binding(AlgebraRegistry *r, AlgebraEntry *alg, ASTNode *fn) {
    if (fn->kind != NODE_FN_DECL) return;
    if (fn->fn_decl.sigils.count == 0) return; /* no sigil = keyword-only function */

    SigilBinding b;
    memset(&b, 0, sizeof(b));
    b.fn_name = fn->fn_decl.fn_name;
    b.sigil = fn->fn_decl.sigils.items[0];
    b.fixity = fn->fn_decl.fixity;
    b.all_sigils = fn->fn_decl.sigils;
    b.pattern = fn->fn_decl.pattern;
    b.return_type = fn->fn_decl.return_type;
    b.fn_node = fn;

    /* Count and collect param types */
    int pc = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++)
        if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM) pc++;
    b.param_count = pc;
    b.param_types = (TypeRef **)arena_alloc(r->arena, pc * sizeof(TypeRef *));
    int idx = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM)
            b.param_types[idx++] = fn->fn_decl.pattern.items[i].type;
    }

    da_push(&alg->bindings, b);
}

void algebra_register_declarations(AlgebraRegistry *r, AlgebraEntry *alg, ASTNode *algebra_node) {
    NodeList *decls = &algebra_node->algebra.declarations;
    for (int i = 0; i < decls->count; i++) {
        ASTNode *decl = decls->items[i];
        switch (decl->kind) {
            case NODE_FN_DECL:
                register_fn_binding(r, alg, decl);
                break;
            case NODE_PRECEDENCE: {
                alg->precedence.count = decl->precedence.sigils.count;
                alg->precedence.sigils = (const char **)arena_alloc(
                    r->arena, alg->precedence.count * sizeof(const char *));
                for (int j = 0; j < alg->precedence.count; j++)
                    alg->precedence.sigils[j] = decl->precedence.sigils.items[j];
                break;
            }
            case NODE_TRAIT_DECL:
                da_push(&alg->trait_decls, decl);
                break;
            case NODE_IMPLEMENT:
                da_push(&alg->implement_blocks, decl);
                break;
            default:
                break;
        }
    }
}

SigilBinding *algebra_find_sigil(AlgebraEntry *alg, const char *sigil, Fixity fixity) {
    for (int i = 0; i < alg->bindings.count; i++) {
        SigilBinding *b = &alg->bindings.items[i];
        if (b->sigil == sigil && (fixity == FIXITY_NONE || b->fixity == fixity))
            return b;
    }
    return NULL;
}

int algebra_get_precedence(AlgebraEntry *alg, const char *sigil) {
    for (int i = 0; i < alg->precedence.count; i++) {
        if (alg->precedence.sigils[i] == sigil)
            return i;
    }
    return -1;
}

bool algebra_check_collisions(AlgebraEntry *alg, ErrorList *errors) {
    bool ok = true;
    for (int i = 0; i < alg->bindings.count; i++) {
        SigilBinding *a = &alg->bindings.items[i];
        for (int j = i + 1; j < alg->bindings.count; j++) {
            SigilBinding *b = &alg->bindings.items[j];
            /* Same sigil and same fixity? */
            if (a->sigil != b->sigil) continue;
            if (a->fixity != b->fixity) continue;
            /* Same parameter count and types? */
            if (a->param_count != b->param_count) continue;
            bool same_types = true;
            for (int k = 0; k < a->param_count; k++) {
                if (!types_equal(a->param_types[k], b->param_types[k])) {
                    same_types = false;
                    break;
                }
            }
            if (same_types) {
                error_add(errors, ERR_RESOLVE, (SrcLoc){NULL, 0, 0, 0},
                         "collision in algebra '%s': sigil '%s' with same fixity and parameter types declared in both '%s' and '%s'",
                         alg->name, a->sigil, a->fn_name, b->fn_name);
                ok = false;
            }
        }
    }
    return ok;
}

SigilBinding *algebra_match_compound(AlgebraEntry *alg, const char **sigils, int count) {
    /* Find bindings whose sigil set matches the given sequence */
    for (int i = 0; i < alg->bindings.count; i++) {
        SigilBinding *b = &alg->bindings.items[i];
        if (b->all_sigils.count < 2) continue; /* not a compound */

        /* Check if all given sigils appear in this binding's pattern */
        bool all_match = true;
        int matched = 0;
        for (int j = 0; j < count && all_match; j++) {
            bool found = false;
            for (int k = 0; k < b->all_sigils.count; k++) {
                if (b->all_sigils.items[k] == sigils[j]) {
                    found = true;
                    matched++;
                    break;
                }
            }
            if (!found) all_match = false;
        }
        if (all_match && matched >= 2) return b;
    }
    return NULL;
}
