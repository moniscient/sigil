#include "resolver.h"
#include <string.h>

void resolver_init(Resolver *r, Arena *arena, InternTable *intern_tab,
                   AlgebraRegistry *algebra_reg, TraitRegistry *trait_reg,
                   TypeChecker *type_checker, ErrorList *errors) {
    r->arena = arena;
    r->intern_tab = intern_tab;
    r->algebra_reg = algebra_reg;
    r->trait_reg = trait_reg;
    r->type_checker = type_checker;
    r->errors = errors;
}

SigilBinding *resolve_sigil(Resolver *r, AlgebraEntry *alg,
                            const char *sigil, Fixity fixity,
                            TypeRef **arg_types, int arg_count) {
    /* Step 1: Find all candidates with matching sigil */
    SigilBinding *candidates[64];
    int candidate_count = 0;

    for (int i = 0; i < alg->bindings.count && candidate_count < 64; i++) {
        SigilBinding *b = &alg->bindings.items[i];
        if (b->sigil != sigil) continue;
        candidates[candidate_count++] = b;
    }

    if (candidate_count == 0) return NULL;
    if (candidate_count == 1) return candidates[0];

    /* Step 2: Narrow by fixity */
    if (fixity != FIXITY_NONE) {
        int new_count = 0;
        for (int i = 0; i < candidate_count; i++) {
            if (candidates[i]->fixity == fixity)
                candidates[new_count++] = candidates[i];
        }
        if (new_count > 0) candidate_count = new_count;
        if (candidate_count == 1) return candidates[0];
    }

    /* Step 3: Narrow by parameter types */
    if (arg_types && arg_count > 0) {
        int new_count = 0;
        for (int i = 0; i < candidate_count; i++) {
            SigilBinding *b = candidates[i];
            if (b->param_count != arg_count) continue;
            bool match = true;
            for (int j = 0; j < arg_count && match; j++) {
                if (!arg_types[j] || arg_types[j]->kind == TYPE_UNKNOWN) continue;
                if (!b->param_types[j]) continue;
                /* Generic/trait-bounded types always match for now */
                if (b->param_types[j]->kind == TYPE_GENERIC ||
                    b->param_types[j]->kind == TYPE_TRAIT_BOUND) continue;
                if (!types_equal(arg_types[j], b->param_types[j]))
                    match = false;
            }
            if (match) candidates[new_count++] = b;
        }
        if (new_count > 0) candidate_count = new_count;
    }

    if (candidate_count == 1) return candidates[0];
    if (candidate_count > 1) {
        error_add(r->errors, ERR_RESOLVE, (SrcLoc){NULL, 0, 0, 0},
                 "ambiguous sigil '%s': %d candidates remain after resolution", sigil, candidate_count);
    }
    return candidate_count > 0 ? candidates[0] : NULL;
}

TypeRef *resolve_generic(Resolver *r, TypeRef *generic_type,
                         const char *type_var, TypeRef *concrete_type) {
    if (!generic_type) return NULL;
    if (generic_type->kind == TYPE_GENERIC && generic_type->name == type_var)
        return concrete_type;
    if (generic_type->kind == TYPE_TRAIT_BOUND && generic_type->name == type_var)
        return concrete_type;
    if (generic_type->kind == TYPE_MAP) {
        TypeRef *t = (TypeRef *)arena_alloc(r->arena, sizeof(TypeRef));
        *t = *generic_type;
        t->key_type = resolve_generic(r, generic_type->key_type, type_var, concrete_type);
        t->val_type = resolve_generic(r, generic_type->val_type, type_var, concrete_type);
        return t;
    }
    return generic_type;
}

/* ── AST Resolution Pass ─────────────────────────────────────────── */

static void resolve_node(Resolver *r, ASTNode *node, AlgebraEntry *alg) {
    if (!node) return;

    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.top_level.count; i++)
                resolve_node(r, node->program.top_level.items[i], alg);
            break;

        case NODE_IMPORT:
            for (int i = 0; i < node->import_decl.declarations.count; i++)
                resolve_node(r, node->import_decl.declarations.items[i], alg);
            break;

        case NODE_ALGEBRA:
        case NODE_LIBRARY: {
            AlgebraEntry *a = algebra_registry_find(r->algebra_reg, node->algebra.algebra_name);
            for (int i = 0; i < node->algebra.declarations.count; i++)
                resolve_node(r, node->algebra.declarations.items[i], a);
            break;
        }

        case NODE_USE: {
            AlgebraEntry *a = algebra_registry_find(r->algebra_reg, node->use_block.algebra_name);
            resolve_node(r, node->use_block.body, a);
            break;
        }

        case NODE_FN_DECL:
            resolve_node(r, node->fn_decl.body, alg);
            break;

        case NODE_CALL:
            for (int i = 0; i < node->call.args.count; i++)
                resolve_node(r, node->call.args.items[i], alg);
            /* Check trait constraints on generic fn calls */
            {
                const char *cn = node->call.call_name;
                FnEntry *fe = NULL;
                for (FnEntry *f = r->type_checker->fn_registry; f; f = f->next) {
                    if (strcmp(f->name, cn) == 0) { fe = f; break; }
                }
                if (fe) {
                    for (int i = 0; i < fe->param_count && i < node->call.args.count; i++) {
                        TypeRef *pt = fe->param_types[i];
                        if (pt && pt->kind == TYPE_TRAIT_BOUND && pt->trait_name) {
                            /* Infer concrete type of the argument */
                            TypeRef *arg_t = node->call.args.items[i]->resolved_type;
                            if (arg_t && arg_t->kind != TYPE_UNKNOWN &&
                                arg_t->kind != TYPE_GENERIC && arg_t->kind != TYPE_TRAIT_BOUND) {
                                const char *type_name = NULL;
                                switch (arg_t->kind) {
                                    case TYPE_INT: type_name = "int"; break;
                                    case TYPE_FLOAT: type_name = "float"; break;
                                    case TYPE_BOOL: type_name = "bool"; break;
                                    case TYPE_CHAR: type_name = "char"; break;
                                    case TYPE_MAP: type_name = "map"; break;
                                    case TYPE_NAMED: type_name = arg_t->name; break;
                                    default: break;
                                }
                                if (type_name && r->trait_reg->defs.count > 0) {
                                    TraitDef *td = trait_find_def(r->trait_reg, pt->trait_name);
                                    if (td) {
                                        TraitImpl *ti = trait_find_impl(r->trait_reg, pt->trait_name, type_name);
                                        if (!ti) {
                                            error_add(r->errors, ERR_RESOLVE, node->loc,
                                                     "type '%s' does not implement trait '%s'",
                                                     type_name, pt->trait_name);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;

        case NODE_BLOCK:
        case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                resolve_node(r, node->block.stmts.items[i], alg);
            break;

        case NODE_IF:
            resolve_node(r, node->if_stmt.condition, alg);
            resolve_node(r, node->if_stmt.then_body, alg);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                resolve_node(r, node->if_stmt.elifs.items[i], alg);
            resolve_node(r, node->if_stmt.else_body, alg);
            break;

        case NODE_WHILE:
            resolve_node(r, node->while_stmt.condition, alg);
            resolve_node(r, node->while_stmt.while_body, alg);
            break;

        case NODE_FOR:
            resolve_node(r, node->for_stmt.iterable, alg);
            resolve_node(r, node->for_stmt.for_body, alg);
            break;

        case NODE_MATCH:
            resolve_node(r, node->match_stmt.match_value, alg);
            for (int i = 0; i < node->match_stmt.cases.count; i++)
                resolve_node(r, node->match_stmt.cases.items[i], alg);
            break;

        case NODE_CASE:
            resolve_node(r, node->case_branch.case_pattern, alg);
            resolve_node(r, node->case_branch.case_body, alg);
            break;

        case NODE_DEFAULT:
            resolve_node(r, node->default_branch.default_body, alg);
            break;

        case NODE_LET:
        case NODE_VAR:
            resolve_node(r, node->binding.value, alg);
            break;

        case NODE_ASSIGN:
            resolve_node(r, node->assign.value, alg);
            break;

        case NODE_RETURN:
            resolve_node(r, node->ret.value, alg);
            break;

        default:
            break;
    }
}

bool resolve_ast(Resolver *r, ASTNode *node) {
    resolve_node(r, node, NULL);
    return !error_has_errors(r->errors);
}
