#include "builtins.h"
#include <string.h>

/* Create a minimal NODE_FN_DECL used as a method signature stub.
 * Only fn_name is meaningful — the completeness checker matches on name. */
static ASTNode *make_method_sig(Arena *arena, InternTable *intern_tab, const char *name) {
    ASTNode *sig = ast_new(arena, NODE_FN_DECL, (SrcLoc){NULL, 0, 0, 0});
    sig->fn_decl.fn_name = intern_cstr(intern_tab, name);
    da_init(&sig->fn_decl.pattern);
    sig->fn_decl.return_type = NULL;
    sig->fn_decl.body = NULL;
    sig->fn_decl.fixity = FIXITY_NONE;
    da_init(&sig->fn_decl.sigils);
    sig->fn_decl.is_primitive = false;
    return sig;
}

/* Register a marker trait (no methods, no prerequisites). */
static void register_marker_trait(TraitRegistry *tr, Arena *arena,
                                  InternTable *intern_tab,
                                  const char *name, const char *builtin_src) {
    TraitDef *def = (TraitDef *)arena_alloc(arena, sizeof(TraitDef));
    def->name = intern_cstr(intern_tab, name);
    def->type_var = NULL;
    def->source_algebra = builtin_src;
    da_init(&def->method_sigs);
    da_init(&def->requires);
    da_init(&def->required_identities);
    da_push(&tr->defs, def);
}

void register_builtin_traits(TraitRegistry *tr, Arena *arena, InternTable *intern_tab) {
    const char *builtin_src = intern_cstr(intern_tab, "__builtin__");

    /* ── Type-level traits (Functor → Applicative → Monad) ────────── */

    /* Functor: one required method "map", no prerequisites. */
    {
        TraitDef *def = (TraitDef *)arena_alloc(arena, sizeof(TraitDef));
        def->name = intern_cstr(intern_tab, "Functor");
        def->type_var = intern_cstr(intern_tab, "F");
        def->source_algebra = builtin_src;
        da_init(&def->method_sigs);
        da_push(&def->method_sigs, make_method_sig(arena, intern_tab, "map"));
        da_init(&def->requires);
        da_init(&def->required_identities);
        da_push(&tr->defs, def);
    }

    /* Applicative: one required method "ap", prerequisite Functor. */
    {
        TraitDef *def = (TraitDef *)arena_alloc(arena, sizeof(TraitDef));
        def->name = intern_cstr(intern_tab, "Applicative");
        def->type_var = intern_cstr(intern_tab, "F");
        def->source_algebra = builtin_src;
        da_init(&def->method_sigs);
        da_push(&def->method_sigs, make_method_sig(arena, intern_tab, "ap"));
        da_init(&def->requires);
        da_push(&def->requires, intern_cstr(intern_tab, "Functor"));
        da_init(&def->required_identities);
        da_push(&tr->defs, def);
    }

    /* Monad: one required method "bind", prerequisite Applicative. */
    {
        TraitDef *def = (TraitDef *)arena_alloc(arena, sizeof(TraitDef));
        def->name = intern_cstr(intern_tab, "Monad");
        def->type_var = intern_cstr(intern_tab, "M");
        def->source_algebra = builtin_src;
        da_init(&def->method_sigs);
        da_push(&def->method_sigs, make_method_sig(arena, intern_tab, "bind"));
        da_init(&def->requires);
        da_push(&def->requires, intern_cstr(intern_tab, "Applicative"));
        da_init(&def->required_identities);
        da_push(&tr->defs, def);
    }

    /* ── Function-level marker traits ─────────────────────────────── */

    /* Bind: marks a function as implementing monadic bind semantics. */
    register_marker_trait(tr, arena, intern_tab, "Bind", builtin_src);

    /* Unit: marks a function as implementing monadic unit semantics. */
    register_marker_trait(tr, arena, intern_tab, "Unit", builtin_src);

    /* Map: marks a function as implementing functor map semantics. */
    register_marker_trait(tr, arena, intern_tab, "Map", builtin_src);
}
