#include "types.h"
#include <string.h>

TypeRef *make_type(Arena *a, TypeKind kind) {
    TypeRef *t = (TypeRef *)arena_alloc(a, sizeof(TypeRef));
    memset(t, 0, sizeof(TypeRef));
    t->kind = kind;
    return t;
}

TypeRef *make_named_type(Arena *a, const char *name) {
    TypeRef *t = make_type(a, TYPE_NAMED);
    t->name = name;
    return t;
}

TypeRef *make_generic_type(Arena *a, const char *name) {
    TypeRef *t = make_type(a, TYPE_GENERIC);
    t->name = name;
    return t;
}

bool types_equal(TypeRef *a, TypeRef *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TYPE_NAMED:
        case TYPE_GENERIC:
            return a->name == b->name; /* interned pointer comparison */
        case TYPE_TRAIT_BOUND:
            return a->name == b->name && a->trait_name == b->trait_name;
        case TYPE_MAP:
            /* A bare map (no key/val subtypes) is equal to any map */
            if (!a->key_type && !a->val_type) return true;
            if (!b->key_type && !b->val_type) return true;
            return types_equal(a->key_type, b->key_type) &&
                   types_equal(a->val_type, b->val_type);
        case TYPE_FN:
            if (a->fn_param_count != b->fn_param_count) return false;
            for (int i = 0; i < a->fn_param_count; i++)
                if (!types_equal(a->fn_param_types[i], b->fn_param_types[i])) return false;
            return types_equal(a->fn_return_type, b->fn_return_type);
        default:
            return true; /* primitive types match by kind */
    }
}

TypeRef *make_fn_type(Arena *a, int param_count, TypeRef **param_types, TypeRef *return_type) {
    TypeRef *t = make_type(a, TYPE_FN);
    t->fn_param_count = param_count;
    t->fn_param_types = (TypeRef **)arena_alloc(a, param_count * sizeof(TypeRef *));
    for (int i = 0; i < param_count; i++)
        t->fn_param_types[i] = param_types[i];
    t->fn_return_type = return_type;
    return t;
}

bool type_compatible(TypeRef *a, TypeRef *b) {
    if (!a || !b) return true;
    if (a->kind == TYPE_UNKNOWN || b->kind == TYPE_UNKNOWN) return true;
    if (a->kind == TYPE_GENERIC || a->kind == TYPE_TRAIT_BOUND) return true;
    if (b->kind == TYPE_GENERIC || b->kind == TYPE_TRAIT_BOUND) return true;
    return types_equal(a, b);
}

/* ── Generic Type Variable Unification ───────────────────────────── */

/* Substitution table for type variables (small, stack-allocated) */
#define MAX_TYPE_VARS 16

typedef struct {
    const char *var_name;
    TypeRef *concrete;
} TypeVarBinding;

TypeRef *unify_generic_return(Arena *arena, int param_count, TypeRef **param_types,
                              TypeRef **arg_types, TypeRef *return_type) {
    if (!return_type) return make_type(arena, TYPE_VOID);

    /* If return type is not generic/trait-bound, no unification needed */
    if (return_type->kind != TYPE_GENERIC && return_type->kind != TYPE_TRAIT_BOUND)
        return return_type;

    /* Build substitution table from params */
    TypeVarBinding bindings[MAX_TYPE_VARS];
    int bind_count = 0;

    for (int i = 0; i < param_count && i < MAX_TYPE_VARS; i++) {
        if (!param_types[i] || !arg_types[i]) continue;
        if (arg_types[i]->kind == TYPE_UNKNOWN) continue;

        const char *var_name = NULL;
        if (param_types[i]->kind == TYPE_GENERIC)
            var_name = param_types[i]->name;
        else if (param_types[i]->kind == TYPE_TRAIT_BOUND)
            var_name = param_types[i]->name;

        if (!var_name) continue;

        /* Check if already bound */
        bool found = false;
        for (int j = 0; j < bind_count; j++) {
            if (bindings[j].var_name == var_name) { found = true; break; }
        }
        if (!found && bind_count < MAX_TYPE_VARS) {
            bindings[bind_count].var_name = var_name;
            bindings[bind_count].concrete = arg_types[i];
            bind_count++;
        }
    }

    /* Substitute in return type */
    for (int i = 0; i < bind_count; i++) {
        if (return_type->name == bindings[i].var_name)
            return bindings[i].concrete;
    }

    return return_type;
}

/* ── Type Environment ────────────────────────────────────────────── */

void type_checker_init(TypeChecker *tc, Arena *arena, InternTable *intern_tab, ErrorList *errors) {
    tc->arena = arena;
    tc->intern_tab = intern_tab;
    tc->errors = errors;
    tc->global_env = type_env_push(NULL);
    tc->global_env->arena = arena;
    tc->fn_registry = NULL;
    tc->current_fn_return_type = NULL;
    tc->type_registry = NULL;
    tc->trait_registry = NULL;
}

void type_checker_set_trait_registry(TypeChecker *tc, TraitRegistry *tr) {
    tc->trait_registry = tr;
}

TypeEnv *type_env_push(TypeEnv *parent) {
    Arena *a = parent ? parent->arena : NULL;
    TypeEnv *env;
    if (a) {
        env = (TypeEnv *)arena_alloc(a, sizeof(TypeEnv));
    } else {
        env = (TypeEnv *)calloc(1, sizeof(TypeEnv));
    }
    env->bindings = NULL;
    env->parent = parent;
    env->arena = a;
    return env;
}

TypeRef *type_env_lookup(TypeEnv *env, const char *name) {
    for (TypeEnv *e = env; e; e = e->parent) {
        for (TypeBinding *b = e->bindings; b; b = b->next) {
            if (b->name == name) return b->type;
        }
    }
    return NULL;
}

bool type_env_is_mutable(TypeEnv *env, const char *name) {
    for (TypeEnv *e = env; e; e = e->parent) {
        for (TypeBinding *b = e->bindings; b; b = b->next) {
            if (b->name == name) return b->is_mutable;
        }
    }
    return false;
}

void type_env_bind(TypeEnv *env, const char *name, TypeRef *type, bool is_mutable) {
    TypeBinding *b = (TypeBinding *)arena_alloc(env->arena, sizeof(TypeBinding));
    b->name = name;
    b->type = type;
    b->is_mutable = is_mutable;
    b->next = env->bindings;
    env->bindings = b;
}

/* ── Function Registry ───────────────────────────────────────────── */

void type_checker_register_fn(TypeChecker *tc, ASTNode *fn_node) {
    if (fn_node->kind != NODE_FN_DECL) return;

    FnEntry *e = (FnEntry *)arena_alloc(tc->arena, sizeof(FnEntry));
    e->name = fn_node->fn_decl.fn_name;
    e->return_type = fn_node->fn_decl.return_type;

    /* Count params */
    int pc = 0;
    for (int i = 0; i < fn_node->fn_decl.pattern.count; i++)
        if (fn_node->fn_decl.pattern.items[i].kind == PAT_PARAM) pc++;

    e->param_count = pc;
    e->param_types = (TypeRef **)arena_alloc(tc->arena, pc * sizeof(TypeRef *));
    e->param_mutable = (bool *)arena_alloc(tc->arena, pc * sizeof(bool));

    int idx = 0;
    for (int i = 0; i < fn_node->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn_node->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM) {
            e->param_types[idx] = pe->type;
            e->param_mutable[idx] = pe->is_mutable;
            idx++;
        }
    }

    /* Check for PAT_REPEATS in pattern */
    e->has_repeats = false;
    e->repeats_start_idx = pc; /* default: after all params */
    for (int i = 0; i < fn_node->fn_decl.pattern.count; i++) {
        if (fn_node->fn_decl.pattern.items[i].kind == PAT_REPEATS) {
            e->has_repeats = true;
            /* Count PAT_PARAM elements before this PAT_REPEATS */
            int ri = 0;
            for (int j = 0; j < i; j++)
                if (fn_node->fn_decl.pattern.items[j].kind == PAT_PARAM) ri++;
            e->repeats_start_idx = ri;
            break;
        }
    }

    e->next = tc->fn_registry;
    tc->fn_registry = e;
}

static FnEntry *find_fn(TypeChecker *tc, const char *name) {
    for (FnEntry *e = tc->fn_registry; e; e = e->next) {
        if (e->name == name || strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

/* ── User-Defined Type Registry ───────────────────────────────────── */

TypeDef *type_def_lookup(TypeChecker *tc, const char *name) {
    for (TypeDef *td = tc->type_registry; td; td = td->next) {
        if (td->name == name || strcmp(td->name, name) == 0) return td;
    }
    return NULL;
}

static void type_def_register(TypeChecker *tc, ASTNode *node) {
    const char *name = node->type_decl.type_name;
    if (type_def_lookup(tc, name)) {
        error_add(tc->errors, ERR_TYPE, node->loc,
                 "duplicate type definition '%s'", name);
        return;
    }
    TypeDef *td = (TypeDef *)arena_alloc(tc->arena, sizeof(TypeDef));
    td->name = name;
    td->field_count = node->type_decl.field_count;
    td->field_types = node->type_decl.field_types;
    td->field_names = node->type_decl.field_names;
    td->next = tc->type_registry;
    tc->type_registry = td;
}

/* ── Type Checking ───────────────────────────────────────────────── */

/* Check if an AST body contains at least one return statement */
static bool body_has_return(ASTNode *node) {
    if (!node) return false;
    if (node->kind == NODE_RETURN) return true;
    if (node->kind == NODE_BLOCK || node->kind == NODE_BEGIN_END) {
        for (int i = 0; i < node->block.stmts.count; i++) {
            if (body_has_return(node->block.stmts.items[i])) return true;
        }
    }
    if (node->kind == NODE_IF) {
        if (body_has_return(node->if_stmt.then_body)) return true;
        for (int i = 1; i < node->if_stmt.elifs.count; i += 2) {
            if (body_has_return(node->if_stmt.elifs.items[i])) return true;
        }
        if (node->if_stmt.else_body && body_has_return(node->if_stmt.else_body)) return true;
    }
    if (node->kind == NODE_WHILE) return body_has_return(node->while_stmt.while_body);
    if (node->kind == NODE_FOR) return body_has_return(node->for_stmt.for_body);
    return false;
}

static TypeRef *check_node(TypeChecker *tc, ASTNode *node, TypeEnv *env);

/* Look up the map type of an expression (from env or resolved_type) */
static TypeRef *get_map_type(TypeChecker *tc __attribute__((unused)), ASTNode *node, TypeEnv *env) {
    if (node->kind == NODE_IDENT) {
        TypeRef *t = type_env_lookup(env, node->ident.ident);
        if (t && t->kind == TYPE_MAP) return t;
    }
    if (node->resolved_type && node->resolved_type->kind == TYPE_MAP)
        return node->resolved_type;
    return NULL;
}

/*
 * Trait-bound verification trace:
 *
 * Given: fn foo TraitBound T x returns T
 *   - FnEntry has param_types[0]->kind == TYPE_TRAIT_BOUND,
 *     param_types[0]->trait_name == "TraitBound", param_types[0]->name == "T"
 *
 * Call: foo someVal  (where someVal has concrete type "MyType")
 *   - arg_types[0] = check_node(someVal) → resolved_type with kind TYPE_NAMED, name "MyType"
 *   - find_fn(tc, "foo") returns the FnEntry
 *   - Loop over params: i=0, param_types[0]->kind == TYPE_TRAIT_BOUND
 *   - trait_name = "TraitBound", arg type = TYPE_NAMED with name "MyType"
 *   - tc->trait_registry non-NULL → call trait_type_has_trait(tr, "MyType", "TraitBound")
 *   - If returns false → error_add(ERR_TRAIT, "argument 1 to 'foo': type 'MyType' does not implement trait 'TraitBound'")
 *   - Then proceeds to unify_generic_return as before
 */
static TypeRef *check_call(TypeChecker *tc, ASTNode *node, TypeEnv *env) {
    const char *name = node->call.call_name;
    int argc = node->call.args.count;

    /* Check if this is a UDT constructor */
    TypeDef *td = type_def_lookup(tc, name);
    if (td) {
        if (argc != td->field_count) {
            error_add(tc->errors, ERR_TYPE, node->loc,
                     "type '%s' expects %d fields, got %d", name, td->field_count, argc);
        }
        /* Type check each arg against field type */
        for (int i = 0; i < argc && i < td->field_count; i++) {
            TypeRef *arg_t = check_node(tc, node->call.args.items[i], env);
            if (arg_t->kind != TYPE_UNKNOWN && td->field_types[i] &&
                !type_compatible(td->field_types[i], arg_t)) {
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "field '%s' of type '%s': type mismatch",
                         td->field_names[i], name);
            }
        }
        TypeRef *result = make_named_type(tc->arena, name);
        node->resolved_type = result;
        return result;
    }

    /* Special-case map operations for generic type tracking */
    if (strcmp(name, "mapnew") == 0) {
        return make_type(tc->arena, TYPE_MAP);
    }

    /* Type check arguments and collect their types */
    TypeRef **arg_types = (TypeRef **)arena_alloc(tc->arena, argc * sizeof(TypeRef *));
    for (int i = 0; i < argc; i++) {
        arg_types[i] = check_node(tc, node->call.args.items[i], env);
    }

    if (strcmp(name, "set") == 0 && argc == 4) {
        /* Double-indexed set: set m i j v → set(get(m, i), j, v) */
        return make_type(tc->arena, TYPE_VOID);
    }

    if (strcmp(name, "set") == 0 && argc == 3) {
        /* Check if first arg is UDT for field set */
        if (arg_types[0] && arg_types[0]->kind == TYPE_NAMED) {
            TypeDef *utd = type_def_lookup(tc, arg_types[0]->name);
            if (utd && node->call.args.items[1]->kind == NODE_IDENT) {
                const char *field = node->call.args.items[1]->ident.ident;
                bool found = false;
                for (int i = 0; i < utd->field_count; i++) {
                    if (strcmp(utd->field_names[i], field) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    error_add(tc->errors, ERR_TYPE, node->loc,
                             "type '%s' has no field '%s'", utd->name, field);
                }
                return make_type(tc->arena, TYPE_VOID);
            }
        }
        /* set map key val — refine map's key/val types */
        TypeRef *map_type = get_map_type(tc, node->call.args.items[0], env);
        if (map_type) {
            if (!map_type->key_type && arg_types[1]->kind != TYPE_UNKNOWN)
                map_type->key_type = arg_types[1];
            if (!map_type->val_type && arg_types[2]->kind != TYPE_UNKNOWN)
                map_type->val_type = arg_types[2];
        }
        return make_type(tc->arena, TYPE_VOID);
    }

    if (strcmp(name, "get") == 0 && argc == 3) {
        /* Double-indexed get: get m i j → get(get(m, i), j)
         * For a map-of-maps, the inner get returns a map, outer returns val type.
         * With bare map (no subtypes), return TYPE_UNKNOWN to avoid false errors. */
        TypeRef *map_type = get_map_type(tc, node->call.args.items[0], env);
        if (map_type && map_type->val_type && map_type->val_type->kind == TYPE_MAP
            && map_type->val_type->val_type)
            return map_type->val_type->val_type;
        /* For bare map<int, map> or untyped map, assume int (common for matrices) */
        return make_type(tc->arena, TYPE_INT);
    }

    if (strcmp(name, "get") == 0 && argc == 2) {
        /* Check if first arg is UDT for field get */
        if (arg_types[0] && arg_types[0]->kind == TYPE_NAMED) {
            TypeDef *utd = type_def_lookup(tc, arg_types[0]->name);
            if (utd && node->call.args.items[1]->kind == NODE_IDENT) {
                const char *field = node->call.args.items[1]->ident.ident;
                for (int i = 0; i < utd->field_count; i++) {
                    if (strcmp(utd->field_names[i], field) == 0)
                        return utd->field_types[i];
                }
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "type '%s' has no field '%s'", utd->name, field);
                return make_type(tc->arena, TYPE_UNKNOWN);
            }
        }
        /* get map key — return the map's value type */
        TypeRef *map_type = get_map_type(tc, node->call.args.items[0], env);
        if (map_type && map_type->val_type)
            return map_type->val_type;
        return make_type(tc->arena, TYPE_UNKNOWN);
    }

    if (strcmp(name, "has") == 0 && argc == 2) {
        return make_type(tc->arena, TYPE_BOOL);
    }

    if (strcmp(name, "equal") == 0 || strcmp(name, "compare") == 0 ||
        strcmp(name, "less") == 0 || strcmp(name, "greater") == 0 ||
        strcmp(name, "less_equal") == 0 || strcmp(name, "greater_equal") == 0 ||
        strcmp(name, "not") == 0 || strcmp(name, "and") == 0 || strcmp(name, "or") == 0) {
        return make_type(tc->arena, TYPE_BOOL);
    }

    if (strcmp(name, "remove") == 0 && argc == 2) {
        return make_type(tc->arena, TYPE_VOID);
    }

    if (strcmp(name, "mapcount") == 0 && argc == 1) {
        return make_type(tc->arena, TYPE_INT);
    }

    if (strcmp(name, "length") == 0 && argc == 1) {
        return make_type(tc->arena, TYPE_INT);
    }

    if (strcmp(name, "to_int") == 0 && argc == 1) {
        return make_type(tc->arena, TYPE_INT);
    }

    if (strcmp(name, "to_float") == 0 && argc == 1) {
        return make_type(tc->arena, TYPE_FLOAT);
    }

    if (strcmp(name, "to_string") == 0 && argc == 1) {
        TypeRef *t = make_type(tc->arena, TYPE_MAP);
        t->key_type = make_type(tc->arena, TYPE_INT);
        t->val_type = make_type(tc->arena, TYPE_CHAR);
        return t;
    }

    if (strcmp(name, "concat") == 0 && argc == 2) {
        TypeRef *t = make_type(tc->arena, TYPE_MAP);
        t->key_type = make_type(tc->arena, TYPE_INT);
        t->val_type = make_type(tc->arena, TYPE_CHAR);
        return t;
    }

    if (strcmp(name, "clone") == 0 && argc == 1) {
        return arg_types[0] ? arg_types[0] : make_type(tc->arena, TYPE_MAP);
    }

    if (strcmp(name, "keys") == 0 && argc == 1) {
        return make_type(tc->arena, TYPE_MAP);
    }

    if (strcmp(name, "values") == 0 && argc == 1) {
        return make_type(tc->arena, TYPE_MAP);
    }

    if (strcmp(name, "append") == 0 && argc == 2) {
        return make_type(tc->arena, TYPE_VOID);
    }

    if (strcmp(name, "invoke") == 0 && argc >= 1) {
        if (arg_types[0] && arg_types[0]->kind == TYPE_FN && arg_types[0]->fn_return_type)
            return arg_types[0]->fn_return_type;
        return make_type(tc->arena, TYPE_UNKNOWN);
    }

    if (strcmp(name, "print") == 0 && argc == 1) {
        return make_type(tc->arena, TYPE_VOID);
    }

    /* Look up fn in registry */
    FnEntry *fn = find_fn(tc, name);
    if (fn) {
        /* Verify trait bounds on parameters */
        if (tc->trait_registry) {
            int check_count = fn->param_count < argc ? fn->param_count : argc;
            for (int i = 0; i < check_count; i++) {
                if (!fn->param_types[i] || fn->param_types[i]->kind != TYPE_TRAIT_BOUND)
                    continue;
                if (!arg_types[i]) continue;
                /* Only check when the concrete type is fully known */
                if (arg_types[i]->kind == TYPE_UNKNOWN ||
                    arg_types[i]->kind == TYPE_GENERIC ||
                    arg_types[i]->kind == TYPE_TRAIT_BOUND)
                    continue;
                const char *concrete_name = NULL;
                if (arg_types[i]->kind == TYPE_NAMED)
                    concrete_name = arg_types[i]->name;
                else if (arg_types[i]->kind == TYPE_INT)
                    concrete_name = "int";
                else if (arg_types[i]->kind == TYPE_FLOAT)
                    concrete_name = "float";
                else if (arg_types[i]->kind == TYPE_BOOL)
                    concrete_name = "bool";
                else if (arg_types[i]->kind == TYPE_CHAR)
                    concrete_name = "char";
                else if (arg_types[i]->kind == TYPE_MAP)
                    concrete_name = "map";
                if (concrete_name &&
                    !trait_type_has_trait(tc->trait_registry, concrete_name,
                                         fn->param_types[i]->trait_name)) {
                    error_add(tc->errors, ERR_TRAIT, node->loc,
                             "argument %d to '%s': type '%s' does not implement trait '%s'",
                             i + 1, name, concrete_name, fn->param_types[i]->trait_name);
                }
            }
        }
        /* Unify generic type variables and return resolved return type */
        return unify_generic_return(tc->arena, fn->param_count, fn->param_types,
                                    arg_types, fn->return_type);
    }

    /* Fallback for built-in primitives not yet registered */
    return make_type(tc->arena, TYPE_UNKNOWN);
}

static TypeRef *check_node(TypeChecker *tc, ASTNode *node, TypeEnv *env) {
    if (!node) return make_type(tc->arena, TYPE_VOID);

    TypeRef *result_type = NULL;

    switch (node->kind) {
        case NODE_INT_LIT:
            result_type = make_type(tc->arena, TYPE_INT);
            node->resolved_type = result_type;
            return result_type;
        case NODE_FLOAT_LIT:
            result_type = make_type(tc->arena, TYPE_FLOAT);
            node->resolved_type = result_type;
            return result_type;
        case NODE_BOOL_LIT:
            result_type = make_type(tc->arena, TYPE_BOOL);
            node->resolved_type = result_type;
            return result_type;
        case NODE_STRING_LIT: {
            TypeRef *map_type = make_type(tc->arena, TYPE_MAP);
            map_type->key_type = make_type(tc->arena, TYPE_INT);
            map_type->val_type = make_type(tc->arena, TYPE_CHAR);
            node->resolved_type = map_type;
            return map_type;
        }

        case NODE_IDENT: {
            TypeRef *t = type_env_lookup(env, node->ident.ident);
            if (!t) {
                node->resolved_type = make_type(tc->arena, TYPE_UNKNOWN);
                return node->resolved_type;
            }
            node->resolved_type = t;
            return t;
        }

        case NODE_CALL:
            result_type = check_call(tc, node, env);
            node->resolved_type = result_type;
            return result_type;

        case NODE_CHAIN: {
            /* Type check all operands; result type = type of first binary call */
            for (int i = 0; i < node->chain.chain_operands.count; i++)
                check_node(tc, node->chain.chain_operands.items[i], env);
            /* Build a synthetic NODE_CALL with first two operands to get return type */
            ASTNode tmp;
            tmp.kind = NODE_CALL;
            tmp.loc = node->loc;
            tmp.resolved_type = NULL;
            tmp.call.call_name = node->chain.chain_fn_name;
            da_init(&tmp.call.args);
            if (node->chain.chain_operands.count >= 2) {
                da_push(&tmp.call.args, node->chain.chain_operands.items[0]);
                da_push(&tmp.call.args, node->chain.chain_operands.items[1]);
            }
            /* check_call now handles trait-bound verification internally,
             * so no additional trait checking is needed here for NODE_CHAIN. */
            result_type = check_call(tc, &tmp, env);
            node->resolved_type = result_type;
            return result_type;
        }

        case NODE_LET:
        case NODE_VAR: {
            TypeRef *val_type = check_node(tc, node->binding.value, env);
            type_env_bind(env, node->binding.bind_name, val_type,
                         node->kind == NODE_VAR);
            return make_type(tc->arena, TYPE_VOID);
        }

        case NODE_ASSIGN: {
            const char *name = node->assign.assign_name;
            TypeRef *existing = type_env_lookup(env, name);
            if (!existing) {
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "assignment to undeclared variable '%s'", name);
            } else if (!type_env_is_mutable(env, name)) {
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "assignment to immutable binding '%s' — use 'var' to declare mutable bindings",
                         name);
            }
            TypeRef *val_type = check_node(tc, node->assign.value, env);
            /* Check type compatibility */
            if (existing && val_type && existing->kind != TYPE_UNKNOWN &&
                val_type->kind != TYPE_UNKNOWN && !types_equal(existing, val_type)) {
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "type mismatch in assignment to '%s'", name);
            }
            return make_type(tc->arena, TYPE_VOID);
        }

        case NODE_RETURN: {
            TypeRef *ret_val = check_node(tc, node->ret.value, env);
            if (tc->current_fn_return_type &&
                ret_val->kind != TYPE_UNKNOWN &&
                tc->current_fn_return_type->kind != TYPE_UNKNOWN &&
                tc->current_fn_return_type->kind != TYPE_GENERIC &&
                tc->current_fn_return_type->kind != TYPE_TRAIT_BOUND &&
                !types_equal(ret_val, tc->current_fn_return_type)) {
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "return type mismatch: expected %d, got %d",
                         tc->current_fn_return_type->kind, ret_val->kind);
            }
            return ret_val;
        }

        case NODE_IF: {
            TypeRef *cond_type = check_node(tc, node->if_stmt.condition, env);
            if (cond_type->kind != TYPE_BOOL && cond_type->kind != TYPE_UNKNOWN) {
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "if condition must be bool");
            }
            TypeRef *then_type = check_node(tc, node->if_stmt.then_body, env);
            for (int i = 0; i < node->if_stmt.elifs.count; i += 2) {
                check_node(tc, node->if_stmt.elifs.items[i], env);
                check_node(tc, node->if_stmt.elifs.items[i + 1], env);
            }
            if (node->if_stmt.else_body)
                check_node(tc, node->if_stmt.else_body, env);
            return then_type;
        }

        case NODE_WHILE:
            check_node(tc, node->while_stmt.condition, env);
            check_node(tc, node->while_stmt.while_body, env);
            return make_type(tc->arena, TYPE_VOID);

        case NODE_FOR: {
            TypeEnv *loop_env = type_env_push(env);
            check_node(tc, node->for_stmt.iterable, env);
            type_env_bind(loop_env, node->for_stmt.var_name,
                         make_type(tc->arena, TYPE_UNKNOWN), false);
            check_node(tc, node->for_stmt.for_body, loop_env);
            return make_type(tc->arena, TYPE_VOID);
        }

        case NODE_MATCH:
            check_node(tc, node->match_stmt.match_value, env);
            for (int i = 0; i < node->match_stmt.cases.count; i++)
                check_node(tc, node->match_stmt.cases.items[i], env);
            return make_type(tc->arena, TYPE_UNKNOWN);

        case NODE_CASE:
            check_node(tc, node->case_branch.case_pattern, env);
            return check_node(tc, node->case_branch.case_body, env);

        case NODE_DEFAULT:
            return check_node(tc, node->default_branch.default_body, env);

        case NODE_BLOCK:
        case NODE_BEGIN_END: {
            TypeRef *last = make_type(tc->arena, TYPE_VOID);
            TypeEnv *block_env = type_env_push(env);
            for (int i = 0; i < node->block.stmts.count; i++)
                last = check_node(tc, node->block.stmts.items[i], block_env);
            return last;
        }

        case NODE_FN_DECL: {
            /* Register this fn for call resolution */
            type_checker_register_fn(tc, node);

            TypeEnv *fn_env = type_env_push(env);
            /* Bind parameters, preserving var (mutable ref) annotation */
            for (int i = 0; i < node->fn_decl.pattern.count; i++) {
                PatElem *pe = &node->fn_decl.pattern.items[i];
                if (pe->kind == PAT_PARAM && pe->param_name) {
                    type_env_bind(fn_env, pe->param_name,
                                 pe->type ? pe->type : make_type(tc->arena, TYPE_UNKNOWN),
                                 pe->is_mutable);
                }
            }
            /* Set current fn return type for return statement validation */
            TypeRef *prev_return_type = tc->current_fn_return_type;
            tc->current_fn_return_type = node->fn_decl.return_type;
            if (node->fn_decl.body)
                check_node(tc, node->fn_decl.body, fn_env);
            tc->current_fn_return_type = prev_return_type;
            /* Verify: non-void return type requires explicit return */
            if (node->fn_decl.return_type &&
                node->fn_decl.return_type->kind != TYPE_VOID &&
                !node->fn_decl.is_primitive &&
                node->fn_decl.body &&
                !body_has_return(node->fn_decl.body)) {
                error_add(tc->errors, ERR_TYPE, node->loc,
                         "function '%s' declared as returning non-void but has no return statement",
                         node->fn_decl.fn_name);
            }
            return make_type(tc->arena, TYPE_VOID);
        }

        case NODE_TRAIT_DECL:
            /* Register trait method signatures for call resolution */
            for (int i = 0; i < node->trait_decl.methods.count; i++) {
                type_checker_register_fn(tc, node->trait_decl.methods.items[i]);
            }
            return make_type(tc->arena, TYPE_VOID);

        case NODE_IMPLEMENT:
            /* Register impl method bodies for call resolution */
            for (int i = 0; i < node->implement.methods.count; i++) {
                type_checker_register_fn(tc, node->implement.methods.items[i]);
            }
            /* Type-check the method bodies */
            for (int i = 0; i < node->implement.methods.count; i++) {
                check_node(tc, node->implement.methods.items[i], env);
            }
            return make_type(tc->arena, TYPE_VOID);

        case NODE_ALGEBRA:
        case NODE_LIBRARY: {
            TypeEnv *alg_env = type_env_push(env);
            for (int i = 0; i < node->algebra.declarations.count; i++)
                check_node(tc, node->algebra.declarations.items[i], alg_env);
            return make_type(tc->arena, TYPE_VOID);
        }

        case NODE_USE:
            return check_node(tc, node->use_block.body, env);

        case NODE_TYPE_DECL:
            type_def_register(tc, node);
            return make_type(tc->arena, TYPE_VOID);

        case NODE_PRECEDENCE:
        case NODE_ALIAS:
        case NODE_PARAM:
        case NODE_TYPE_REF:
        case NODE_BREAK:
        case NODE_CONTINUE:
            return make_type(tc->arena, TYPE_VOID);

        case NODE_PROGRAM:
            for (int i = 0; i < node->program.top_level.count; i++)
                check_node(tc, node->program.top_level.items[i], env);
            return make_type(tc->arena, TYPE_VOID);

        case NODE_IMPORT:
            for (int i = 0; i < node->import_decl.declarations.count; i++)
                check_node(tc, node->import_decl.declarations.items[i], env);
            return make_type(tc->arena, TYPE_VOID);

        case NODE_LAMBDA: {
            TypeEnv *lambda_env = type_env_push(env);
            for (int i = 0; i < node->lambda.lambda_param_count; i++) {
                type_env_bind(lambda_env, node->lambda.lambda_param_names[i],
                             node->lambda.lambda_param_types[i], false);
            }
            TypeRef *prev_ret = tc->current_fn_return_type;
            tc->current_fn_return_type = node->lambda.lambda_return_type;
            check_node(tc, node->lambda.lambda_body, lambda_env);
            tc->current_fn_return_type = prev_ret;
            /* Build TYPE_FN */
            result_type = make_fn_type(tc->arena, node->lambda.lambda_param_count,
                                       node->lambda.lambda_param_types,
                                       node->lambda.lambda_return_type);
            node->resolved_type = result_type;
            return result_type;
        }

        case NODE_COMPREHENSION: {
            TypeEnv *comp_env = type_env_push(env);
            TypeRef *src_type = check_node(tc, node->comprehension.comp_source, env);
            /* Bind iteration variable */
            TypeRef *elem_type = make_type(tc->arena, TYPE_UNKNOWN);
            if (src_type && src_type->kind == TYPE_MAP && src_type->key_type)
                elem_type = src_type->key_type;
            type_env_bind(comp_env, node->comprehension.comp_var, elem_type, false);
            /* Check filter */
            if (node->comprehension.comp_filter) {
                TypeRef *filter_t = check_node(tc, node->comprehension.comp_filter, comp_env);
                if (filter_t->kind != TYPE_BOOL && filter_t->kind != TYPE_UNKNOWN)
                    error_add(tc->errors, ERR_TYPE, node->loc,
                             "comprehension 'where' clause must be bool");
            }
            /* Check transform */
            TypeRef *transform_t = check_node(tc, node->comprehension.comp_transform, comp_env);
            /* Result: map<int, transform_type> */
            result_type = make_type(tc->arena, TYPE_MAP);
            result_type->key_type = make_type(tc->arena, TYPE_INT);
            result_type->val_type = transform_t;
            node->resolved_type = result_type;
            return result_type;
        }

        case NODE_SIGIL_EXPR:
            /* Should have been desugared */
            error_add(tc->errors, ERR_TYPE, node->loc,
                     "unresolved sigil expression '%s'", node->sigil_expr.sigil);
            return make_type(tc->arena, TYPE_UNKNOWN);
    }

    return make_type(tc->arena, TYPE_UNKNOWN);
}

bool type_check(TypeChecker *tc, ASTNode *node) {
    check_node(tc, node, tc->global_env);
    return !error_has_errors(tc->errors);
}
