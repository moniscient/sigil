#include "traits.h"
#include <string.h>

void trait_registry_init(TraitRegistry *tr, Arena *arena, InternTable *intern_tab, ErrorList *errors) {
    da_init(&tr->defs);
    da_init(&tr->impls);
    tr->type_owners = NULL;
    tr->arena = arena;
    tr->intern_tab = intern_tab;
    tr->errors = errors;
}

void trait_register_type_owner(TraitRegistry *tr, const char *type_name, const char *algebra_name) {
    const char *tn = intern_cstr(tr->intern_tab, type_name);
    const char *an = intern_cstr(tr->intern_tab, algebra_name);
    /* Check if already registered */
    for (TypeOwner *o = tr->type_owners; o; o = o->next) {
        if (o->type_name == tn) return;
    }
    TypeOwner *o = (TypeOwner *)arena_alloc(tr->arena, sizeof(TypeOwner));
    o->type_name = tn;
    o->algebra_name = an;
    o->next = tr->type_owners;
    tr->type_owners = o;
}

const char *trait_find_type_owner(TraitRegistry *tr, const char *type_name) {
    const char *tn = intern_cstr(tr->intern_tab, type_name);
    for (TypeOwner *o = tr->type_owners; o; o = o->next) {
        if (o->type_name == tn) return o->algebra_name;
    }
    return NULL;
}

bool trait_type_has_trait(TraitRegistry *tr, const char *type_name, const char *trait_name) {
    return trait_find_impl(tr, trait_name, type_name) != NULL;
}

void trait_register_def(TraitRegistry *tr, ASTNode *trait_node) {
    assert(trait_node->kind == NODE_TRAIT_DECL);
    TraitDef *def = (TraitDef *)arena_alloc(tr->arena, sizeof(TraitDef));
    def->name = intern_cstr(tr->intern_tab, trait_node->trait_decl.trait_name);
    def->type_var = trait_node->trait_decl.type_var;
    def->source_algebra = NULL;
    def->method_sigs = trait_node->trait_decl.methods;
    def->requires = trait_node->trait_decl.requires;
    /* Extract required identity declarations from method_sigs */
    da_init(&def->required_identities);
    for (int i = 0; i < def->method_sigs.count; i++) {
        ASTNode *sig = def->method_sigs.items[i];
        if (sig->kind == NODE_REQUIRES_IDENTITY)
            da_push(&def->required_identities, sig->requires_identity.req_identity_fn);
    }
    da_push(&tr->defs, def);
}

void trait_register_impl(TraitRegistry *tr, ASTNode *impl_node) {
    assert(impl_node->kind == NODE_IMPLEMENT);
    TraitImpl *impl = (TraitImpl *)arena_alloc(tr->arena, sizeof(TraitImpl));
    impl->trait_name = intern_cstr(tr->intern_tab, impl_node->implement.trait_name);
    impl->concrete_type = intern_cstr(tr->intern_tab, impl_node->implement.concrete_type);
    impl->source_algebra = NULL;

    /* Separate NODE_IDENTITY entries from method nodes */
    NodeList methods_only;
    da_init(&methods_only);
    impl->identities = NULL;
    impl->identity_count = 0;

    for (int i = 0; i < impl_node->implement.methods.count; i++) {
        ASTNode *m = impl_node->implement.methods.items[i];
        if (m->kind == NODE_IDENTITY) {
            int idx = impl->identity_count++;
            impl->identities = realloc(impl->identities,
                impl->identity_count * sizeof(IdentityEntry));
            impl->identities[idx].fn_name =
                intern_cstr(tr->intern_tab, m->identity.identity_fn_name);
            impl->identities[idx].value = m->identity.identity_value;
        } else {
            da_push(&methods_only, m);
        }
    }
    impl->methods = methods_only;
    da_push(&tr->impls, impl);
}

void trait_register_from_algebra(TraitRegistry *tr, AlgebraEntry *alg) {
    for (int i = 0; i < alg->trait_decls.count; i++) {
        trait_register_def(tr, alg->trait_decls.items[i]);
        /* Tag with source algebra */
        tr->defs.items[tr->defs.count - 1]->source_algebra = alg->name;
    }
    for (int i = 0; i < alg->implement_blocks.count; i++) {
        trait_register_impl(tr, alg->implement_blocks.items[i]);
        /* Tag with source algebra */
        tr->impls.items[tr->impls.count - 1]->source_algebra = alg->name;
    }
}

TraitDef *trait_find_def(TraitRegistry *tr, const char *name) {
    const char *interned = intern_cstr(tr->intern_tab, name);
    for (int i = 0; i < tr->defs.count; i++) {
        if (tr->defs.items[i]->name == interned)
            return tr->defs.items[i];
    }
    return NULL;
}

TraitImpl *trait_find_impl(TraitRegistry *tr, const char *trait_name, const char *type_name) {
    const char *t = intern_cstr(tr->intern_tab, trait_name);
    const char *ty = intern_cstr(tr->intern_tab, type_name);
    for (int i = 0; i < tr->impls.count; i++) {
        if (tr->impls.items[i]->trait_name == t &&
            tr->impls.items[i]->concrete_type == ty)
            return tr->impls.items[i];
    }
    return NULL;
}

bool trait_check_prerequisites(TraitRegistry *tr, const char *trait_name, const char *type_name) {
    TraitDef *def = trait_find_def(tr, trait_name);
    if (!def) return false;

    for (int i = 0; i < def->requires.count; i++) {
        const char *req = def->requires.items[i];
        TraitImpl *impl = trait_find_impl(tr, req, type_name);
        if (!impl) {
            error_add(tr->errors, ERR_TRAIT, (SrcLoc){NULL, 0, 0, 0},
                     "type '%s' must implement '%s' before implementing '%s'",
                     type_name, req, trait_name);
            return false;
        }
    }
    return true;
}

bool trait_check_all(TraitRegistry *tr) {
    bool ok = true;

    for (int i = 0; i < tr->impls.count; i++) {
        TraitImpl *impl = tr->impls.items[i];

        /* Check that the trait exists */
        TraitDef *def = trait_find_def(tr, impl->trait_name);
        if (!def) {
            error_add(tr->errors, ERR_TRAIT, (SrcLoc){NULL, 0, 0, 0},
                     "implementation for unknown trait '%s'", impl->trait_name);
            ok = false;
            continue;
        }

        /* Check prerequisites */
        if (!trait_check_prerequisites(tr, impl->trait_name, impl->concrete_type))
            ok = false;

        /* Built-in traits (__builtin__) skip the orphan rule —
         * any algebra may implement them for any type it defines. */
        const char *builtin_str = intern_cstr(tr->intern_tab, "__builtin__");
        bool is_builtin_trait = (def->source_algebra == builtin_str);

        /* Orphan rule: impl must be in the algebra that defines the trait
         * or the algebra that defines the type. */
        if (!is_builtin_trait &&
            impl->source_algebra && def->source_algebra &&
            impl->source_algebra != def->source_algebra) {
            /* Impl is in a different algebra than the trait.
             * Check if the impl's algebra defines the concrete type. */
            const char *ct = impl->concrete_type;
            bool is_builtin = (strcmp(ct, "int") == 0 || strcmp(ct, "float") == 0 ||
                               strcmp(ct, "bool") == 0 || strcmp(ct, "char") == 0 ||
                               strcmp(ct, "map") == 0 || strcmp(ct, "void") == 0);
            const char *type_owner = trait_find_type_owner(tr, ct);
            bool type_in_impl_algebra = (type_owner && type_owner == impl->source_algebra);
            if (!is_builtin && !type_in_impl_algebra) {
                error_add(tr->errors, ERR_TRAIT, (SrcLoc){NULL, 0, 0, 0},
                         "orphan rule violation: implement '%s' for '%s' must be in the algebra "
                         "that defines '%s' or '%s'",
                         impl->trait_name, impl->concrete_type,
                         impl->trait_name, impl->concrete_type);
                ok = false;
            }
        }

        /* Check identity completeness: all required identities must be provided */
        for (int j = 0; j < def->required_identities.count; j++) {
            const char *req_fn = intern_cstr(tr->intern_tab, def->required_identities.items[j]);
            bool found = false;
            for (int k = 0; k < impl->identity_count; k++) {
                if (impl->identities[k].fn_name == req_fn) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                error_add(tr->errors, ERR_TRAIT, (SrcLoc){NULL, 0, 0, 0},
                         "implementation of '%s' for '%s' is missing identity for '%s'",
                         impl->trait_name, impl->concrete_type, req_fn);
                ok = false;
            }
        }

        /* Check completeness: all methods in the trait must be implemented */
        for (int j = 0; j < def->method_sigs.count; j++) {
            ASTNode *sig = def->method_sigs.items[j];
            if (sig->kind != NODE_FN_DECL) continue;
            const char *method_name = sig->fn_decl.fn_name;
            bool found = false;
            for (int k = 0; k < impl->methods.count; k++) {
                ASTNode *m = impl->methods.items[k];
                if (m->kind == NODE_FN_DECL &&
                    strcmp(m->fn_decl.fn_name, method_name) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                error_add(tr->errors, ERR_TRAIT, (SrcLoc){NULL, 0, 0, 0},
                         "implementation of '%s' for '%s' is missing method '%s'",
                         impl->trait_name, impl->concrete_type, method_name);
                ok = false;
            }
        }
    }

    return ok;
}
