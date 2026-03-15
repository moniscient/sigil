#include "c_emitter.h"
#include "parallel.h"
#include <string.h>
#include <ctype.h>

/* Forward declaration */
static ASTNode *chain_to_calls(Arena *arena, ASTNode *node);

void c_emitter_init(CEmitter *e, FILE *out, TypeChecker *tc, TraitRegistry *traits, Arena *arena) {
    e->out = out;
    e->indent = 0;
    e->tc = tc;
    e->traits = traits;
    e->arena = arena;
    e->var_params = NULL;
    e->var_param_count = 0;
    e->in_implement = false;
    e->implement_type = NULL;
    e->mono_instances = NULL;
    e->current_mono = NULL;
    e->lambdas = NULL;
    e->lambda_counter = 0;
    e->thunk_fn_counter = 0;
    e->thunk_fn_map = NULL;
    e->in_thunk_arg = false;
    e->in_fn_body = false;
    e->current_fn_name = NULL;
}

/* ── Thunk fn mapping helpers ─────────────────────────────────────── */

static int register_thunk_fn(CEmitter *e, const char *name, int param_count, TypeRef **param_types,
                              bool is_mono, int mono_tvc, TypeRef **mono_concrete) {
    int id = e->thunk_fn_counter++;
    ThunkFnMap *m = (ThunkFnMap *)arena_alloc(e->arena, sizeof(ThunkFnMap));
    m->fn_name = name;
    m->param_count = param_count;
    m->param_types = param_types;
    m->func_id = id;
    m->is_mono = is_mono;
    m->mono_type_var_count = mono_tvc;
    m->mono_concrete = mono_concrete;
    m->next = e->thunk_fn_map;
    e->thunk_fn_map = m;
    return id;
}

static int lookup_thunk_fn_id(CEmitter *e, const char *name, int param_count, TypeRef **param_types) {
    /* First try: match name + param_count + param_types (for overloaded fns) */
    if (param_types) {
        for (ThunkFnMap *m = e->thunk_fn_map; m; m = m->next) {
            if (strcmp(m->fn_name, name) != 0) continue;
            if (m->is_mono) continue;
            if (m->param_count != param_count) continue;
            if (!m->param_types) continue;
            bool match = true;
            for (int i = 0; i < param_count && match; i++) {
                if (!param_types[i] || !m->param_types[i]) continue;
                if (m->param_types[i]->kind == TYPE_GENERIC ||
                    m->param_types[i]->kind == TYPE_TRAIT_BOUND) continue;
                if (!types_equal(param_types[i], m->param_types[i]))
                    match = false;
            }
            if (match) return m->func_id;
        }
    }
    /* Second try: match name + param_count */
    for (ThunkFnMap *m = e->thunk_fn_map; m; m = m->next) {
        if (strcmp(m->fn_name, name) != 0) continue;
        if (m->is_mono) continue;
        if (m->param_count == param_count) return m->func_id;
    }
    /* Last resort: any match by name */
    for (ThunkFnMap *m = e->thunk_fn_map; m; m = m->next) {
        if (strcmp(m->fn_name, name) == 0 && !m->is_mono) return m->func_id;
    }
    return -1;
}

static int lookup_thunk_mono_id(CEmitter *e, const char *name, int tvc, TypeRef **conc) {
    for (ThunkFnMap *m = e->thunk_fn_map; m; m = m->next) {
        if (!m->is_mono) continue;
        if (strcmp(m->fn_name, name) != 0) continue;
        if (m->mono_type_var_count != tvc) continue;
        bool match = true;
        for (int i = 0; i < tvc; i++) {
            if (!types_equal(m->mono_concrete[i], conc[i])) { match = false; break; }
        }
        if (match) return m->func_id;
    }
    return -1;
}

/* Check if a call name refers to a user-defined (non-builtin) fn */
static bool is_user_fn(CEmitter *e, const char *name);

static void emit_indent(CEmitter *e) {
    for (int i = 0; i < e->indent; i++)
        fprintf(e->out, "    ");
}

/* ── Type Mapping ────────────────────────────────────────────────── */

/* ── Monomorphization helpers ─────────────────────────────────────── */

static bool fn_is_generic(ASTNode *fn) {
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM && pe->type &&
            (pe->type->kind == TYPE_GENERIC || pe->type->kind == TYPE_TRAIT_BOUND))
            return true;
    }
    return false;
}

/* Look up type var name in current_mono substitution, return concrete type or NULL */
static TypeRef *mono_substitute(CEmitter *e, TypeRef *t) {
    if (!e->current_mono || !t) return NULL;
    if (t->kind != TYPE_GENERIC && t->kind != TYPE_TRAIT_BOUND) return NULL;
    const char *name = t->name;
    if (!name) return NULL;
    for (int i = 0; i < e->current_mono->type_var_count; i++) {
        if (strcmp(e->current_mono->type_var_names[i], name) == 0)
            return e->current_mono->concrete_types[i];
    }
    return NULL;
}

static void emit_c_type(CEmitter *e, TypeRef *t) {
    if (!t) { fprintf(e->out, "int64_t"); return; }
    /* Check mono substitution for generic types */
    TypeRef *sub = mono_substitute(e, t);
    if (sub) { t = sub; }
    switch (t->kind) {
        case TYPE_BOOL:    fprintf(e->out, "bool"); break;
        case TYPE_INT:     fprintf(e->out, "int64_t"); break;
        case TYPE_INT8:    fprintf(e->out, "int8_t"); break;
        case TYPE_INT16:   fprintf(e->out, "int16_t"); break;
        case TYPE_INT32:   fprintf(e->out, "int32_t"); break;
        case TYPE_INT64:   fprintf(e->out, "int64_t"); break;
        case TYPE_FLOAT:   fprintf(e->out, "double"); break;
        case TYPE_FLOAT32: fprintf(e->out, "float"); break;
        case TYPE_FLOAT64: fprintf(e->out, "double"); break;
        case TYPE_CHAR:    fprintf(e->out, "uint32_t"); break;
        case TYPE_VOID:    fprintf(e->out, "void"); break;
        case TYPE_MAP:     fprintf(e->out, "SigilMap*"); break;
        case TYPE_ITER:    fprintf(e->out, "SigilIter"); break;
        case TYPE_NAMED:   fprintf(e->out, "SigilMap*"); break;
        case TYPE_FN:      fprintf(e->out, "SigilClosure*"); break;
        case TYPE_GENERIC:
        case TYPE_TRAIT_BOUND:
        case TYPE_UNKNOWN:
            fprintf(e->out, "int64_t"); break;
    }
}

/* ── Mangle function name ────────────────────────────────────────── */

static bool fn_name_is_overloaded(CEmitter *e, const char *name) {
    int count = 0;
    for (FnEntry *fn = e->tc->fn_registry; fn; fn = fn->next) {
        if (strcmp(fn->name, name) == 0) count++;
    }
    return count > 1;
}

static const char *type_suffix(TypeRef *t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TYPE_BOOL: return "bool";
        case TYPE_INT: return "int";
        case TYPE_INT8: return "int8";
        case TYPE_INT16: return "int16";
        case TYPE_INT32: return "int32";
        case TYPE_INT64: return "int64";
        case TYPE_FLOAT: return "float";
        case TYPE_FLOAT32: return "float32";
        case TYPE_FLOAT64: return "float64";
        case TYPE_CHAR: return "char";
        case TYPE_MAP: return "map";
        case TYPE_VOID: return "void";
        case TYPE_ITER: return "iter";
        case TYPE_NAMED: return t->name ? t->name : "named";
        case TYPE_GENERIC: return t->name ? t->name : "T";
        case TYPE_TRAIT_BOUND: return t->name ? t->name : "trait";
        case TYPE_FN: return "fn";
        case TYPE_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static void emit_fn_name(CEmitter *e, const char *name) {
    fprintf(e->out, "sigil_%s", name);
}

static void emit_mangled_fn_name(CEmitter *e, const char *name, int param_count, TypeRef **param_types) {
    if (!fn_name_is_overloaded(e, name)) {
        emit_fn_name(e, name);
        return;
    }
    fprintf(e->out, "sigil_%s", name);
    for (int i = 0; i < param_count; i++) {
        fprintf(e->out, "_%s", type_suffix(param_types[i]));
    }
}

/* ── Builtin primitive operations ────────────────────────────────── */

typedef struct {
    const char *name;
    int arity;
    const char *c_op;       /* infix operator or NULL */
    const char *c_func;     /* function call or NULL */
    bool is_unary_prefix;
} BuiltinOp;

static const BuiltinOp builtins[] = {
    {"add",           2, "+",  NULL, false},
    {"subtract",      2, "-",  NULL, false},
    {"multiply",      2, "*",  NULL, false},
    {"times",         2, "*",  NULL, false},
    {"divide",        2, "/",  NULL, false},
    {"modulo",        2, "%",  NULL, false},
    {"negate",        1, NULL, NULL, true},
    {"equal",         2, "==", NULL, false},
    {"compare",       2, "==", NULL, false},
    {"less",          2, "<",  NULL, false},
    {"greater",       2, ">",  NULL, false},
    {"less_equal",    2, "<=", NULL, false},
    {"greater_equal", 2, ">=", NULL, false},
    {"and",           2, "&&", NULL, false},
    {"or",            2, "||", NULL, false},
    {"not",           1, NULL, NULL, true},
    {"print",         1, NULL, NULL, false},
    {NULL, 0, NULL, NULL, false}
};

static const BuiltinOp *find_builtin(const char *name) {
    for (int i = 0; builtins[i].name; i++) {
        if (strcmp(builtins[i].name, name) == 0)
            return &builtins[i];
    }
    return NULL;
}

/* ── Var param helpers ────────────────────────────────────────────── */

static bool is_var_param(CEmitter *e, const char *name) {
    for (int i = 0; i < e->var_param_count; i++) {
        if (strcmp(e->var_params[i], name) == 0) return true;
    }
    return false;
}

/* ── Map operation helpers ────────────────────────────────────────── */

static bool is_map_op(const char *name) {
    return strcmp(name, "mapnew") == 0 || strcmp(name, "get") == 0 ||
           strcmp(name, "set") == 0 || strcmp(name, "has") == 0 ||
           strcmp(name, "remove") == 0 || strcmp(name, "mapcount") == 0 ||
           strcmp(name, "length") == 0 || strcmp(name, "append") == 0 ||
           strcmp(name, "clone") == 0 || strcmp(name, "keys") == 0 ||
           strcmp(name, "values") == 0 || strcmp(name, "concat") == 0;
}

/* ── New builtin operations ──────────────────────────────────────── */

static bool is_type_conversion(const char *name) {
    return strcmp(name, "to_int") == 0 || strcmp(name, "to_float") == 0 ||
           strcmp(name, "to_string") == 0;
}

static bool is_invoke_call(const char *name) {
    return strcmp(name, "invoke") == 0;
}

/* ── UDT helpers ─────────────────────────────────────────────────── */

static TypeDef *find_type_def(CEmitter *e, const char *name) {
    return type_def_lookup(e->tc, name);
}

/* ── is_user_fn: check if a call refers to a user-defined fn ────── */

static bool is_user_fn(CEmitter *e, const char *name) {
    if (!name) return false;
    /* Builtins, map ops, type conversions, invoke are NOT user fns */
    if (find_builtin(name)) return false;
    if (is_map_op(name)) return false;
    if (is_type_conversion(name)) return false;
    if (is_invoke_call(name)) return false;
    /* UDT constructors are not user fns (they're inline map creation) */
    if (find_type_def(e, name)) return false;
    /* Check if it's in the thunk fn map */
    for (ThunkFnMap *m = e->thunk_fn_map; m; m = m->next) {
        if (strcmp(m->fn_name, name) == 0) return true;
    }
    /* Check fn registry */
    for (FnEntry *f = e->tc->fn_registry; f; f = f->next) {
        if (strcmp(f->name, name) == 0) return true;
    }
    return false;
}

/* ── Forward declarations ────────────────────────────────────────── */

static void emit_expr(CEmitter *e, ASTNode *node);
static void emit_boxed(CEmitter *e, ASTNode *arg);
static void emit_stmt(CEmitter *e, ASTNode *node);
static void emit_body(CEmitter *e, ASTNode *node);
static TypeRef *infer_expr_type(CEmitter *e, ASTNode *node);
static bool fn_entry_is_generic(FnEntry *fn);
static void extract_type_vars(ASTNode *fn, const char **names, int *count);
static void emit_arg_as_thunk(CEmitter *e, ASTNode *arg);
static void emit_ctor_expr_raw(CEmitter *e, ASTNode *node, int param_count, const char **pnames);
static void emit_ctor_cond(CEmitter *e, ASTNode *cond, int param_count, const char **pnames);
static void emit_ctor_arg_val(CEmitter *e, ASTNode *node, int param_count, const char **pnames);
static int count_recursive_calls(CEmitter *e, ASTNode *node, const char *fn_name);
static ASTNode *find_return_expr(ASTNode *body);
static void collect_recursive_thunks_to_result(CEmitter *e, ASTNode *node, const char *self_name,
                                                int self_id, int param_count, const char **pnames);
static void emit_recursive_child_assignments(CEmitter *e, ASTNode *node, const char *self_name,
                                              int self_id, int param_count, const char **pnames);

/* Emit a `get` result as raw SigilVal (no unboxing). Handles begin/end wrapping. */
static void emit_raw_map_get(CEmitter *e, ASTNode *node) {
    if (!node) return;
    /* Unwrap begin/end to find the inner get call */
    if (node->kind == NODE_BEGIN_END && node->block.stmts.count == 1)
        node = node->block.stmts.items[0];
    if (node->kind == NODE_CALL && strcmp(node->call.call_name, "get") == 0
        && node->call.args.count == 2) {
        fprintf(e->out, "sigil_map_get(");
        emit_expr(e, node->call.args.items[0]);
        fprintf(e->out, ", ");
        emit_boxed(e, node->call.args.items[1]);
        fprintf(e->out, ")");
    } else {
        /* Fallback: emit normally and box as SigilVal */
        fprintf(e->out, "sigil_val_map(");
        emit_expr(e, node);
        fprintf(e->out, ")");
    }
}

/* ── Quick thunk ID lookup (simplified, for arg emission) ────────── */

static int quick_thunk_lookup(CEmitter *e, const char *name, int argc) {
    /* First try exact param count match (non-mono) */
    for (ThunkFnMap *m = e->thunk_fn_map; m; m = m->next) {
        if (m->is_mono) continue;
        if (strcmp(m->fn_name, name) == 0 && m->param_count == argc)
            return m->func_id;
    }
    /* Try mono instances */
    for (ThunkFnMap *m = e->thunk_fn_map; m; m = m->next) {
        if (!m->is_mono) continue;
        if (strcmp(m->fn_name, name) == 0 && m->param_count == argc)
            return m->func_id;
    }
    return -1;
}

/* ── Emit an expression as a SigilThunk* (for lazy arg passing) ──── */

static void emit_arg_as_thunk(CEmitter *e, ASTNode *arg) {
    if (!arg) {
        fprintf(e->out, "thunk_alloc_completed(&_arena, sigil_val_int(0))");
        return;
    }

    /* Fold chains before checking */
    if (arg->kind == NODE_CHAIN) { emit_arg_as_thunk(e, chain_to_calls(e->arena, arg)); return; }
    /* If arg is a user fn call with a thunk_id, emit a PENDING thunk */
    if (arg->kind == NODE_CALL) {
        const char *cn = arg->call.call_name;
        /* Skip builtins, map ops, type conversions, invoke, UDT constructors */
        if (!find_builtin(cn) && !is_map_op(cn) && !is_type_conversion(cn) &&
            !is_invoke_call(cn) && !find_type_def(e, cn)) {
            int call_arity = arg->call.args.count;
            int tid = quick_thunk_lookup(e, cn, call_arity);
            if (tid >= 0) {
                /* Emit PENDING child thunk */
                fprintf(e->out, "({ SigilThunk *_tk = thunk_alloc(&_arena, %d, %d);", tid, call_arity);
                for (int i = 0; i < call_arity; i++) {
                    fprintf(e->out, " _tk->args[%d] = ", i);
                    emit_arg_as_thunk(e, arg->call.args.items[i]);
                    fprintf(e->out, ";");
                }
                fprintf(e->out, " _tk; })");
                return;
            }
        }
    }

    /* Default: wrap the concrete value in a completed thunk */
    fprintf(e->out, "thunk_alloc_completed(&_arena, ");
    emit_boxed(e, arg);
    fprintf(e->out, ")");
}

/* ── Chain-to-call folding helper ─────────────────────────────────── */

/* Left-fold a NODE_CHAIN into nested NODE_CALL nodes.
 * The flat representation is preserved in the AST for future optimization;
 * this helper produces a temporary folded view for emission. */
static ASTNode *chain_to_calls(Arena *arena, ASTNode *node) {
    if (!node || node->kind != NODE_CHAIN) return node;
    int n = node->chain.chain_operands.count;
    if (n == 0) return NULL;
    if (n == 1) return node->chain.chain_operands.items[0];
    ASTNode *acc = node->chain.chain_operands.items[0];
    for (int i = 1; i < n; i++) {
        ASTNode *call = ast_new(arena, NODE_CALL, node->loc);
        call->call.call_name = node->chain.chain_fn_name;
        da_init(&call->call.args);
        da_push(&call->call.args, acc);
        da_push(&call->call.args, node->chain.chain_operands.items[i]);
        call->resolved_type = node->resolved_type;
        acc = call;
    }
    return acc;
}

/* ── Type inference for expressions (uses TypeChecker env) ───────── */

static TypeRef *infer_expr_type(CEmitter *e, ASTNode *node) {
    if (!node) return NULL;
    /* Use resolved_type from type checker if available */
    if (node->resolved_type && node->resolved_type->kind != TYPE_UNKNOWN) {
        /* In a mono body, substitute generic resolved types */
        TypeRef *sub = mono_substitute(e, node->resolved_type);
        if (sub) return sub;
        return node->resolved_type;
    }
    if (node->kind == NODE_INT_LIT)   return make_type(e->arena, TYPE_INT);
    if (node->kind == NODE_FLOAT_LIT) return make_type(e->arena, TYPE_FLOAT);
    if (node->kind == NODE_BOOL_LIT)  return make_type(e->arena, TYPE_BOOL);
    if (node->kind == NODE_STRING_LIT) {
        TypeRef *t = make_type(e->arena, TYPE_MAP);
        t->key_type = make_type(e->arena, TYPE_INT);
        t->val_type = make_type(e->arena, TYPE_CHAR);
        return t;
    }
    if (node->kind == NODE_IDENT) {
        /* In a mono body, check if this ident is a parameter with a generic type */
        if (e->current_mono) {
            ASTNode *fn = e->current_mono->fn_node;
            for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
                PatElem *pe = &fn->fn_decl.pattern.items[i];
                if (pe->kind == PAT_PARAM && pe->param_name &&
                    strcmp(pe->param_name, node->ident.ident) == 0) {
                    TypeRef *sub = mono_substitute(e, pe->type);
                    if (sub) return sub;
                    return pe->type;
                }
            }
        }
        TypeRef *t = type_env_lookup(e->tc->global_env, node->ident.ident);
        if (t) {
            TypeRef *sub = mono_substitute(e, t);
            if (sub) return sub;
            return t;
        }
    }
    if (node->kind == NODE_CALL) {
        const char *cn = node->call.call_name;
        /* UDT constructor */
        if (find_type_def(e, cn))
            return make_named_type(e->arena, cn);
        if (strcmp(cn, "mapnew") == 0) return make_type(e->arena, TYPE_MAP);
        if (strcmp(cn, "has") == 0) return make_type(e->arena, TYPE_BOOL);
        if (strcmp(cn, "equal") == 0 || strcmp(cn, "compare") == 0 ||
            strcmp(cn, "less") == 0 || strcmp(cn, "greater") == 0 ||
            strcmp(cn, "less_equal") == 0 || strcmp(cn, "greater_equal") == 0 ||
            strcmp(cn, "not") == 0 || strcmp(cn, "and") == 0 || strcmp(cn, "or") == 0)
            return make_type(e->arena, TYPE_BOOL);
        if (strcmp(cn, "mapcount") == 0 || strcmp(cn, "length") == 0) return make_type(e->arena, TYPE_INT);
        if (strcmp(cn, "to_int") == 0) return make_type(e->arena, TYPE_INT);
        if (strcmp(cn, "to_float") == 0) return make_type(e->arena, TYPE_FLOAT);
        if (strcmp(cn, "to_string") == 0) {
            TypeRef *t = make_type(e->arena, TYPE_MAP);
            t->key_type = make_type(e->arena, TYPE_INT);
            t->val_type = make_type(e->arena, TYPE_CHAR);
            return t;
        }
        if (strcmp(cn, "concat") == 0) {
            TypeRef *t = make_type(e->arena, TYPE_MAP);
            t->key_type = make_type(e->arena, TYPE_INT);
            t->val_type = make_type(e->arena, TYPE_CHAR);
            return t;
        }
        if (strcmp(cn, "clone") == 0 || strcmp(cn, "keys") == 0 || strcmp(cn, "values") == 0)
            return make_type(e->arena, TYPE_MAP);
        if (strcmp(cn, "invoke") == 0 && node->call.args.count >= 1) {
            TypeRef *fn_type = infer_expr_type(e, node->call.args.items[0]);
            if (fn_type && fn_type->kind == TYPE_FN && fn_type->fn_return_type)
                return fn_type->fn_return_type;
            return make_type(e->arena, TYPE_INT);
        }
        if (strcmp(cn, "get") == 0 && node->call.args.count == 2) {
            TypeRef *map_t = infer_expr_type(e, node->call.args.items[0]);
            if (map_t && map_t->kind == TYPE_MAP && map_t->val_type)
                return map_t->val_type;
        }
        /* Look up fn return type */
        for (FnEntry *fn = e->tc->fn_registry; fn; fn = fn->next) {
            if (strcmp(fn->name, cn) == 0)
                return fn->return_type;
        }
    }
    if (node->kind == NODE_CHAIN) {
        /* Return type of the chain = return type of the binary fn */
        const char *cn = node->chain.chain_fn_name;
        for (FnEntry *fn = e->tc->fn_registry; fn; fn = fn->next) {
            if (strcmp(fn->name, cn) == 0)
                return fn->return_type;
        }
        return make_type(e->arena, TYPE_UNKNOWN);
    }
    if (node->kind == NODE_BEGIN_END && node->block.stmts.count > 0) {
        /* Type of begin/end is the type of the last statement */
        return infer_expr_type(e, node->block.stmts.items[node->block.stmts.count - 1]);
    }
    if (node->kind == NODE_LAMBDA) {
        return make_fn_type(e->arena, node->lambda.lambda_param_count,
                           node->lambda.lambda_param_types,
                           node->lambda.lambda_return_type);
    }
    if (node->kind == NODE_COMPREHENSION) {
        TypeRef *t = make_type(e->arena, TYPE_MAP);
        t->key_type = make_type(e->arena, TYPE_INT);
        TypeRef *transform_t = infer_expr_type(e, node->comprehension.comp_transform);
        t->val_type = transform_t;
        return t;
    }
    return make_type(e->arena, TYPE_UNKNOWN);
}

/* ── Boxing / Unboxing Helpers ────────────────────────────────────── */

static void emit_boxed(CEmitter *e, ASTNode *arg) {
    TypeRef *t = infer_expr_type(e, arg);
    TypeKind k = t ? t->kind : TYPE_INT;
    const char *fn;
    switch (k) {
        case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
            fn = "sigil_val_float"; break;
        case TYPE_BOOL:
            fn = "sigil_val_bool"; break;
        case TYPE_CHAR:
            fn = "sigil_val_char"; break;
        case TYPE_MAP:
        case TYPE_NAMED:
            fn = "sigil_val_map"; break;
        case TYPE_FN:
            fn = "sigil_val_closure"; break;
        default:
            fn = "sigil_val_int"; break;
    }
    fprintf(e->out, "%s(", fn);
    emit_expr(e, arg);
    fprintf(e->out, ")");
}

static void emit_unbox_prefix(CEmitter *e, TypeRef *t) {
    TypeKind k = t ? t->kind : TYPE_INT;
    switch (k) {
        case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
            fprintf(e->out, "sigil_unbox_float("); break;
        case TYPE_BOOL:
            fprintf(e->out, "sigil_unbox_bool("); break;
        case TYPE_CHAR:
            fprintf(e->out, "sigil_unbox_char("); break;
        case TYPE_MAP:
        case TYPE_NAMED:
            fprintf(e->out, "sigil_unbox_map("); break;
        case TYPE_FN:
            fprintf(e->out, "sigil_unbox_closure("); break;
        default:
            fprintf(e->out, "sigil_unbox_int("); break;
    }
}

/* ── Lambda Capture Analysis ──────────────────────────────────────── */

typedef struct { const char *names[64]; TypeRef *types[64]; int count; } CaptureList;

static void collect_free_vars(CEmitter *e, ASTNode *node, const char **bound, int bound_count,
                               CaptureList *caps) {
    if (!node) return;
    switch (node->kind) {
        case NODE_IDENT: {
            const char *name = node->ident.ident;
            /* Check if bound locally */
            for (int i = 0; i < bound_count; i++)
                if (strcmp(bound[i], name) == 0) return;
            /* Check if already captured */
            for (int i = 0; i < caps->count; i++)
                if (strcmp(caps->names[i], name) == 0) return;
            /* Check if it's a variable in scope (not a function name) */
            TypeRef *t = type_env_lookup(e->tc->global_env, name);
            if (t && caps->count < 64) {
                caps->names[caps->count] = name;
                caps->types[caps->count] = t;
                caps->count++;
            }
            break;
        }
        case NODE_CALL:
            for (int i = 0; i < node->call.args.count; i++)
                collect_free_vars(e, node->call.args.items[i], bound, bound_count, caps);
            break;
        case NODE_CHAIN:
            for (int i = 0; i < node->chain.chain_operands.count; i++)
                collect_free_vars(e, node->chain.chain_operands.items[i], bound, bound_count, caps);
            break;
        case NODE_BLOCK: case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                collect_free_vars(e, node->block.stmts.items[i], bound, bound_count, caps);
            break;
        case NODE_LET: case NODE_VAR:
            collect_free_vars(e, node->binding.value, bound, bound_count, caps);
            break;
        case NODE_ASSIGN:
            collect_free_vars(e, node->assign.value, bound, bound_count, caps);
            break;
        case NODE_RETURN:
            collect_free_vars(e, node->ret.value, bound, bound_count, caps);
            break;
        case NODE_IF:
            collect_free_vars(e, node->if_stmt.condition, bound, bound_count, caps);
            collect_free_vars(e, node->if_stmt.then_body, bound, bound_count, caps);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                collect_free_vars(e, node->if_stmt.elifs.items[i], bound, bound_count, caps);
            collect_free_vars(e, node->if_stmt.else_body, bound, bound_count, caps);
            break;
        case NODE_WHILE:
            collect_free_vars(e, node->while_stmt.condition, bound, bound_count, caps);
            collect_free_vars(e, node->while_stmt.while_body, bound, bound_count, caps);
            break;
        case NODE_FOR:
            collect_free_vars(e, node->for_stmt.iterable, bound, bound_count, caps);
            /* Loop var is bound inside body */
            {
                const char *new_bound[65];
                int nbc = bound_count < 64 ? bound_count : 64;
                for (int i = 0; i < nbc; i++) new_bound[i] = bound[i];
                new_bound[nbc] = node->for_stmt.var_name;
                collect_free_vars(e, node->for_stmt.for_body, new_bound, nbc + 1, caps);
            }
            break;
        default:
            break;
    }
}

/* Register a lambda for deferred emission; returns the assigned ID */
static int register_lambda(CEmitter *e, ASTNode *node, CaptureList *caps) {
    int id = e->lambda_counter++;
    node->lambda.lambda_id = id;
    LambdaEntry *le = (LambdaEntry *)arena_alloc(e->arena, sizeof(LambdaEntry));
    le->id = id;
    le->lambda_node = node;
    le->capture_count = caps->count;
    le->capture_names = (const char **)arena_alloc(e->arena, caps->count * sizeof(const char *));
    le->capture_types = (TypeRef **)arena_alloc(e->arena, caps->count * sizeof(TypeRef *));
    for (int i = 0; i < caps->count; i++) {
        le->capture_names[i] = caps->names[i];
        le->capture_types[i] = caps->types[i];
    }
    le->next = e->lambdas;
    e->lambdas = le;
    return id;
}

/* Emit a deferred lambda as a static C function */
static void emit_lambda_function(CEmitter *e, LambdaEntry *le) {
    ASTNode *node = le->lambda_node;

    /* Emit: static <ret> _sigil_lambda_N(SigilClosure* _cl, <params>...) { ... } */
    fprintf(e->out, "static ");
    emit_c_type(e, node->lambda.lambda_return_type);
    fprintf(e->out, " _sigil_lambda_%d(SigilClosure *_cl", le->id);
    for (int i = 0; i < node->lambda.lambda_param_count; i++) {
        fprintf(e->out, ", ");
        emit_c_type(e, node->lambda.lambda_param_types[i]);
        fprintf(e->out, " %s", node->lambda.lambda_param_names[i]);
    }
    fprintf(e->out, ") {\n");
    e->indent++;

    /* Unpack captures from _cl->captures[] */
    for (int i = 0; i < le->capture_count; i++) {
        emit_indent(e);
        emit_c_type(e, le->capture_types[i]);
        fprintf(e->out, " %s = ", le->capture_names[i]);
        emit_unbox_prefix(e, le->capture_types[i]);
        fprintf(e->out, "_cl->captures[%d]);\n", i);
    }

    /* Body */
    emit_body(e, node->lambda.lambda_body);

    e->indent--;
    fprintf(e->out, "}\n\n");
}

/* ── Trait-Conditional Helper ────────────────────────────────────── */

static bool type_has_trait(CEmitter *e, const char *type_name, const char *trait_name) {
    if (!e->traits) return false;
    return trait_find_impl(e->traits, trait_name, type_name) != NULL;
}

/* ── Expression Emission ─────────────────────────────────────────── */

static void emit_expr(CEmitter *e, ASTNode *node) {
    if (!node) return;

    switch (node->kind) {
        case NODE_INT_LIT:
            if (node->resolved_type && node->resolved_type->kind != TYPE_INT &&
                node->resolved_type->kind != TYPE_UNKNOWN) {
                fprintf(e->out, "(");
                emit_c_type(e, node->resolved_type);
                fprintf(e->out, ")%lldLL", (long long)node->int_lit.int_val);
            } else {
                fprintf(e->out, "%lldLL", (long long)node->int_lit.int_val);
            }
            break;

        case NODE_FLOAT_LIT:
            if (node->resolved_type && node->resolved_type->kind == TYPE_FLOAT32) {
                fprintf(e->out, "%gf", node->float_lit.float_val);
            } else {
                fprintf(e->out, "%g", node->float_lit.float_val);
            }
            break;

        case NODE_BOOL_LIT:
            fprintf(e->out, "%s", node->bool_lit.bool_val ? "true" : "false");
            break;

        case NODE_STRING_LIT: {
            /* Emit as sigil_string_from_utf8("...", len) */
            fprintf(e->out, "sigil_string_from_utf8(\"");
            /* Escape the string for C */
            const char *s = node->string_lit.str_val;
            for (int i = 0; i < node->string_lit.str_len; i++) {
                switch (s[i]) {
                    case '"':  fprintf(e->out, "\\\""); break;
                    case '\\': fprintf(e->out, "\\\\"); break;
                    case '\n': fprintf(e->out, "\\n"); break;
                    case '\t': fprintf(e->out, "\\t"); break;
                    case '\r': fprintf(e->out, "\\r"); break;
                    default:   fputc(s[i], e->out); break;
                }
            }
            fprintf(e->out, "\", %d)", node->string_lit.str_len);
            break;
        }

        case NODE_IDENT:
            if (strcmp(node->ident.ident, "context") == 0) {
                fprintf(e->out, "_context");
            } else if (is_var_param(e, node->ident.ident)) {
                fprintf(e->out, "(*%s)", node->ident.ident);
            } else {
                fprintf(e->out, "%s", node->ident.ident);
            }
            break;

        case NODE_CHAIN: {
            /* Bind trait guard: chains of bind operations are always
             * sequential, even if the function also has Associative. */
            bool is_bind = type_has_trait(e, node->chain.chain_fn_name, "Bind");
            bool is_assoc = type_has_trait(e, node->chain.chain_fn_name, "Associative");
            if (is_assoc && !is_bind) {
                /* Parallel chain emission (reserved for future implementation) */
                emit_expr(e, chain_to_calls(e->arena, node));
            } else {
                /* Sequential: left-fold into nested calls */
                emit_expr(e, chain_to_calls(e->arena, node));
            }
            break;
        }

        case NODE_CALL: {
            /* UDT constructor */
            {
                TypeDef *td = find_type_def(e, node->call.call_name);
                if (td) {
                    fprintf(e->out, "({ SigilMap *_tmp = sigil_map_new();");
                    int n_args = node->call.args.count;
                    for (int i = 0; i < n_args && i < td->field_count; i++) {
                        fprintf(e->out, " sigil_map_set(_tmp, sigil_val_int(%dLL), ", i);
                        emit_boxed(e, node->call.args.items[i]);
                        fprintf(e->out, ");");
                    }
                    fprintf(e->out, " _tmp; })");
                    break;
                }
            }

            /* UDT field get */
            if (strcmp(node->call.call_name, "get") == 0 && node->call.args.count == 2) {
                TypeRef *first_t = infer_expr_type(e, node->call.args.items[0]);
                if (first_t && first_t->kind == TYPE_NAMED) {
                    TypeDef *td = find_type_def(e, first_t->name);
                    if (td && node->call.args.items[1]->kind == NODE_IDENT) {
                        const char *field = node->call.args.items[1]->ident.ident;
                        for (int i = 0; i < td->field_count; i++) {
                            if (strcmp(td->field_names[i], field) == 0) {
                                emit_unbox_prefix(e, td->field_types[i]);
                                fprintf(e->out, "sigil_map_get(");
                                emit_expr(e, node->call.args.items[0]);
                                fprintf(e->out, ", sigil_val_int(%dLL)))", i);
                                goto call_done;
                            }
                        }
                    }
                }
            }

            /* UDT field set */
            if (strcmp(node->call.call_name, "set") == 0 && node->call.args.count == 3) {
                TypeRef *first_t = infer_expr_type(e, node->call.args.items[0]);
                if (first_t && first_t->kind == TYPE_NAMED) {
                    TypeDef *td = find_type_def(e, first_t->name);
                    if (td && node->call.args.items[1]->kind == NODE_IDENT) {
                        const char *field = node->call.args.items[1]->ident.ident;
                        for (int i = 0; i < td->field_count; i++) {
                            if (strcmp(td->field_names[i], field) == 0) {
                                fprintf(e->out, "sigil_map_set(");
                                emit_expr(e, node->call.args.items[0]);
                                fprintf(e->out, ", sigil_val_int(%dLL), ", i);
                                emit_boxed(e, node->call.args.items[2]);
                                fprintf(e->out, ")");
                                goto call_done;
                            }
                        }
                    }
                }
            }

            /* Invoke: call a closure */
            if (is_invoke_call(node->call.call_name) && node->call.args.count >= 1) {
                ASTNode *closure_arg = node->call.args.items[0];
                TypeRef *fn_type = infer_expr_type(e, closure_arg);
                /* Cast fn_ptr and call: ((ret_type (*)(SigilClosure*, params...))(cl->fn_ptr))(cl, args...) */
                fprintf(e->out, "({ SigilClosure *_inv_cl = ");
                emit_expr(e, closure_arg);
                fprintf(e->out, "; ((");
                /* Return type */
                TypeRef *ret_type = (fn_type && fn_type->kind == TYPE_FN && fn_type->fn_return_type)
                    ? fn_type->fn_return_type : make_type(e->arena, TYPE_INT);
                emit_c_type(e, ret_type);
                fprintf(e->out, " (*)(SigilClosure*");
                /* Param types */
                int invoke_argc = node->call.args.count - 1;
                for (int i = 0; i < invoke_argc; i++) {
                    fprintf(e->out, ", ");
                    if (fn_type && fn_type->kind == TYPE_FN && i < fn_type->fn_param_count)
                        emit_c_type(e, fn_type->fn_param_types[i]);
                    else {
                        TypeRef *at = infer_expr_type(e, node->call.args.items[i + 1]);
                        emit_c_type(e, at);
                    }
                }
                fprintf(e->out, "))(_inv_cl->fn_ptr))(_inv_cl");
                for (int i = 1; i < node->call.args.count; i++) {
                    fprintf(e->out, ", ");
                    emit_expr(e, node->call.args.items[i]);
                }
                fprintf(e->out, "); })");
                break;
            }

            /* Type conversions */
            if (is_type_conversion(node->call.call_name) && node->call.args.count == 1) {
                const char *cn = node->call.call_name;
                ASTNode *arg = node->call.args.items[0];
                TypeRef *arg_type = infer_expr_type(e, arg);
                if (strcmp(cn, "to_int") == 0) {
                    if (arg_type && (arg_type->kind == TYPE_FLOAT || arg_type->kind == TYPE_FLOAT32 || arg_type->kind == TYPE_FLOAT64)) {
                        fprintf(e->out, "sigil_to_int(");
                        emit_expr(e, arg);
                        fprintf(e->out, ")");
                    } else if (arg_type && arg_type->kind == TYPE_CHAR) {
                        fprintf(e->out, "(int64_t)(");
                        emit_expr(e, arg);
                        fprintf(e->out, ")");
                    } else if (arg_type && arg_type->kind == TYPE_BOOL) {
                        fprintf(e->out, "(int64_t)(");
                        emit_expr(e, arg);
                        fprintf(e->out, ")");
                    } else {
                        /* Already int */
                        emit_expr(e, arg);
                    }
                } else if (strcmp(cn, "to_float") == 0) {
                    if (arg_type && arg_type->kind == TYPE_INT) {
                        fprintf(e->out, "sigil_to_float(");
                        emit_expr(e, arg);
                        fprintf(e->out, ")");
                    } else {
                        emit_expr(e, arg);
                    }
                } else if (strcmp(cn, "to_string") == 0) {
                    if (arg_type && arg_type->kind == TYPE_INT) {
                        fprintf(e->out, "sigil_int_to_string(");
                    } else if (arg_type && (arg_type->kind == TYPE_FLOAT || arg_type->kind == TYPE_FLOAT32 || arg_type->kind == TYPE_FLOAT64)) {
                        fprintf(e->out, "sigil_float_to_string(");
                    } else if (arg_type && arg_type->kind == TYPE_BOOL) {
                        fprintf(e->out, "sigil_bool_to_string(");
                    } else if (arg_type && arg_type->kind == TYPE_CHAR) {
                        fprintf(e->out, "sigil_char_to_string(");
                    } else {
                        fprintf(e->out, "sigil_int_to_string(");
                    }
                    emit_expr(e, arg);
                    fprintf(e->out, ")");
                }
                break;
            }

            /* Map operations — special-cased before builtins */
            if (is_map_op(node->call.call_name)) {
                const char *op = node->call.call_name;
                if (strcmp(op, "mapnew") == 0) {
                    fprintf(e->out, "sigil_map_new()");
                } else if (strcmp(op, "set") == 0 && node->call.args.count == 4) {
                    /* 4-arg set: double-indexed map set (e.g., matrix m[i][j] = v) */
                    fprintf(e->out, "sigil_map_set(sigil_unbox_map(sigil_map_get(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, ")), ");
                    emit_boxed(e, node->call.args.items[2]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[3]);
                    fprintf(e->out, ")");
                } else if (strcmp(op, "set") == 0 && node->call.args.count == 3) {
                    fprintf(e->out, "sigil_map_set(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[2]);
                    fprintf(e->out, ")");
                } else if (strcmp(op, "get") == 0 && node->call.args.count == 3) {
                    /* 3-arg get: double-indexed map access (e.g., matrix m[i][j]) */
                    TypeRef *ret = node->resolved_type;
                    emit_unbox_prefix(e, ret);
                    fprintf(e->out, "sigil_map_get(sigil_unbox_map(sigil_map_get(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, ")), ");
                    emit_boxed(e, node->call.args.items[2]);
                    fprintf(e->out, "))");
                } else if (strcmp(op, "get") == 0 && node->call.args.count == 2) {
                    /* Determine unbox type: use resolved_type, or map's val_type */
                    TypeRef *ret = node->resolved_type;
                    if (!ret || ret->kind == TYPE_UNKNOWN) {
                        TypeRef *map_t = infer_expr_type(e, node->call.args.items[0]);
                        if (map_t && map_t->kind == TYPE_MAP && map_t->val_type)
                            ret = map_t->val_type;
                    }
                    emit_unbox_prefix(e, ret);
                    fprintf(e->out, "sigil_map_get(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, "))");
                } else if (strcmp(op, "has") == 0 && node->call.args.count == 2) {
                    fprintf(e->out, "sigil_map_has(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, ")");
                } else if (strcmp(op, "remove") == 0 && node->call.args.count == 2) {
                    fprintf(e->out, "sigil_map_remove(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, ")");
                } else if ((strcmp(op, "mapcount") == 0 || strcmp(op, "length") == 0) &&
                           node->call.args.count == 1) {
                    TypeRef *arg_t = infer_expr_type(e, node->call.args.items[0]);
                    if (arg_t && (arg_t->kind == TYPE_MAP || arg_t->kind == TYPE_NAMED)) {
                        fprintf(e->out, "sigil_map_count(");
                        emit_expr(e, node->call.args.items[0]);
                        fprintf(e->out, ")");
                    } else {
                        /* Arg type unknown — emit as sigil_unbox_map to handle
                           cases like length(get(m, i)) where get returns unknown type
                           but the value is actually a map */
                        fprintf(e->out, "sigil_map_count(sigil_unbox_map(");
                        emit_raw_map_get(e, node->call.args.items[0]);
                        fprintf(e->out, "))");
                    }
                } else if (strcmp(op, "append") == 0 && node->call.args.count == 2) {
                    fprintf(e->out, "sigil_map_append(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, ")");
                } else if (strcmp(op, "clone") == 0 && node->call.args.count == 1) {
                    fprintf(e->out, "sigil_map_copy(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ")");
                } else if (strcmp(op, "keys") == 0 && node->call.args.count == 1) {
                    fprintf(e->out, "sigil_map_keys(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ")");
                } else if (strcmp(op, "values") == 0 && node->call.args.count == 1) {
                    fprintf(e->out, "sigil_map_values(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ")");
                } else if (strcmp(op, "concat") == 0 && node->call.args.count == 2) {
                    fprintf(e->out, "sigil_string_concat(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_expr(e, node->call.args.items[1]);
                    fprintf(e->out, ")");
                } else {
                    /* No matching map op signature — fall through to general fn call */
                    goto map_op_fallthrough;
                }
                break;
            }
            map_op_fallthrough: ;
            const BuiltinOp *bi = find_builtin(node->call.call_name);
            /* Skip builtin if any arg has a non-primitive type (map, named) —
               the user has a custom fn overload for that type combination */
            if (bi && node->call.args.count >= 1 && strcmp(bi->name, "print") != 0) {
                bool has_complex_arg = false;
                for (int i = 0; i < node->call.args.count; i++) {
                    TypeRef *at = infer_expr_type(e, node->call.args.items[i]);
                    if (at && (at->kind == TYPE_MAP || at->kind == TYPE_NAMED)) {
                        has_complex_arg = true;
                        break;
                    }
                }
                if (has_complex_arg) bi = NULL;
            }
            if (bi) {
                /* Type-aware print dispatch */
                if (strcmp(bi->name, "print") == 0 && node->call.args.count == 1) {
                    TypeRef *arg_type = infer_expr_type(e, node->call.args.items[0]);
                    const char *print_fn = "sigil_print_int";
                    if (arg_type) {
                        switch (arg_type->kind) {
                            case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                                print_fn = "sigil_print_float"; break;
                            case TYPE_BOOL:
                                print_fn = "sigil_print_bool"; break;
                            case TYPE_CHAR:
                                print_fn = "sigil_print_char"; break;
                            case TYPE_MAP:
                                if (arg_type->val_type && arg_type->val_type->kind == TYPE_CHAR)
                                    print_fn = "sigil_print_string";
                                else
                                    print_fn = "sigil_print_map";
                                break;
                            case TYPE_NAMED:
                                print_fn = "sigil_print_map";
                                break;
                            default:
                                print_fn = "sigil_print_int"; break;
                        }
                    }
                    fprintf(e->out, "%s(", print_fn);
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ")");
                } else if (bi->is_unary_prefix && node->call.args.count == 1) {
                    /* Unary: negate -> (-x), not -> (!x) */
                    const char *op = strcmp(bi->name, "negate") == 0 ? "-" : "!";
                    fprintf(e->out, "(%s", op);
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ")");
                } else if (bi->c_op && node->call.args.count == 2) {
                    /* Binary infix */
                    fprintf(e->out, "(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, " %s ", bi->c_op);
                    emit_expr(e, node->call.args.items[1]);
                    fprintf(e->out, ")");
                } else if (bi->c_func) {
                    fprintf(e->out, "%s(", bi->c_func);
                    for (int i = 0; i < node->call.args.count; i++) {
                        if (i > 0) fprintf(e->out, ", ");
                        emit_expr(e, node->call.args.items[i]);
                    }
                    fprintf(e->out, ")");
                } else {
                    /* Fallback: emit as function call */
                    emit_fn_name(e, node->call.call_name);
                    fprintf(e->out, "(");
                    for (int i = 0; i < node->call.args.count; i++) {
                        if (i > 0) fprintf(e->out, ", ");
                        emit_expr(e, node->call.args.items[i]);
                    }
                    fprintf(e->out, ")");
                }
            } else {
                /* User-defined function call */
                /* Check if any param is var (mutable ref) -> pass &arg */
                FnEntry *fn = NULL;
                /* Prefer the overload that matches concrete arg types */
                if (fn_name_is_overloaded(e, node->call.call_name)) {
                    TypeRef *arg_types[16] = {0};
                    int argc = node->call.args.count < 16 ? node->call.args.count : 16;
                    for (int ai = 0; ai < argc; ai++)
                        arg_types[ai] = infer_expr_type(e, node->call.args.items[ai]);
                    for (FnEntry *f = e->tc->fn_registry; f; f = f->next) {
                        if (strcmp(f->name, node->call.call_name) != 0) continue;
                        if (f->param_count != argc) continue;
                        bool match = true;
                        for (int ai = 0; ai < argc && match; ai++) {
                            if (!arg_types[ai] || !f->param_types[ai]) continue;
                            if (f->param_types[ai]->kind == TYPE_GENERIC ||
                                f->param_types[ai]->kind == TYPE_TRAIT_BOUND) continue;
                            if (!types_equal(arg_types[ai], f->param_types[ai]))
                                match = false;
                        }
                        if (match) { fn = f; break; }
                    }
                }
                if (!fn) {
                    for (FnEntry *f = e->tc->fn_registry; f; f = f->next) {
                        if (strcmp(f->name, node->call.call_name) == 0) { fn = f; break; }
                    }
                    if (fn && node->call.args.count > 0) {
                        TypeRef *arg_types[16] = {0};
                        int argc = node->call.args.count < 16 ? node->call.args.count : 16;
                        for (int ai = 0; ai < argc; ai++)
                            arg_types[ai] = infer_expr_type(e, node->call.args.items[ai]);
                        bool current_match = true;
                        for (int ai = 0; ai < argc && ai < fn->param_count; ai++) {
                            if (!arg_types[ai] || !fn->param_types[ai]) continue;
                            if (fn->param_types[ai]->kind == TYPE_GENERIC) continue;
                            if (!types_equal(arg_types[ai], fn->param_types[ai])) {
                                current_match = false;
                                break;
                            }
                        }
                        if (!current_match) {
                            for (FnEntry *f = e->tc->fn_registry; f; f = f->next) {
                                if (f->param_count != argc) continue;
                                bool match = true;
                                for (int ai = 0; ai < argc && match; ai++) {
                                    if (!arg_types[ai] || !f->param_types[ai]) continue;
                                    if (f->param_types[ai]->kind == TYPE_GENERIC) continue;
                                    if (!types_equal(arg_types[ai], f->param_types[ai]))
                                        match = false;
                                }
                                if (match) { fn = f; break; }
                            }
                        }
                    }
                }

                /* Determine thunk func_id for this call */
                int thunk_id = -1;
                bool is_generic_call = fn && fn_entry_is_generic(fn);
                int call_arity = node->call.args.count;
                TypeRef *call_ret = node->resolved_type;

                if (is_generic_call) {
                    /* Look up mono instance func_id */
                    const char *tv_names[16];
                    int tvc = 0;
                    ASTNode *fn_ast = NULL;
                    TypeRef *conc[16] = {0};
                    for (MonoInstance *mi = e->mono_instances; mi; mi = mi->next) {
                        if (strcmp(mi->fn_name, node->call.call_name) == 0) {
                            fn_ast = mi->fn_node;
                            break;
                        }
                    }
                    if (fn_ast) {
                        extract_type_vars(fn_ast, tv_names, &tvc);
                        for (int pi = 0; pi < fn_ast->fn_decl.pattern.count; pi++) {
                            PatElem *pe = &fn_ast->fn_decl.pattern.items[pi];
                            if (pe->kind != PAT_PARAM) continue;
                            if (pe->type && (pe->type->kind == TYPE_GENERIC || pe->type->kind == TYPE_TRAIT_BOUND)) {
                                int arg_idx = 0;
                                for (int j = 0; j < pi; j++)
                                    if (fn_ast->fn_decl.pattern.items[j].kind == PAT_PARAM)
                                        arg_idx++;
                                TypeRef *arg_t = NULL;
                                if (arg_idx < node->call.args.count)
                                    arg_t = infer_expr_type(e, node->call.args.items[arg_idx]);
                                if (!arg_t) arg_t = make_type(e->arena, TYPE_INT);
                                for (int k = 0; k < tvc; k++) {
                                    if (strcmp(tv_names[k], pe->type->name) == 0)
                                        conc[k] = arg_t;
                                }
                            }
                        }
                        for (int k = 0; k < tvc; k++)
                            if (!conc[k]) conc[k] = make_type(e->arena, TYPE_INT);
                        thunk_id = lookup_thunk_mono_id(e, node->call.call_name, tvc, conc);
                    }
                } else {
                    /* Infer arg types for overload-aware lookup */
                    TypeRef *lookup_types[16] = {0};
                    int lookup_argc = call_arity < 16 ? call_arity : 16;
                    for (int ai = 0; ai < lookup_argc; ai++)
                        lookup_types[ai] = infer_expr_type(e, node->call.args.items[ai]);
                    thunk_id = lookup_thunk_fn_id(e, node->call.call_name, call_arity, lookup_types);
                }

                /* Skip thunk wrapping for var-param (mutable ref) fns — can't lazily mutate */
                bool has_var_param = false;
                if (fn) {
                    for (int i = 0; i < fn->param_count; i++) {
                        if (fn->param_mutable[i]) { has_var_param = true; break; }
                    }
                }

                /* If inside a fn body, emit direct C call (no thunk wrapping) */
                if (e->in_fn_body && thunk_id >= 0 && !(fn && fn->has_repeats) && !has_var_param) {
                    /* Direct call to the C function */
                    if (is_generic_call) {
                        const char *tv_names[16];
                        int tvc = 0;
                        ASTNode *fn_ast = NULL;
                        TypeRef *conc[16] = {0};
                        for (MonoInstance *mi = e->mono_instances; mi; mi = mi->next) {
                            if (strcmp(mi->fn_name, node->call.call_name) == 0) {
                                fn_ast = mi->fn_node;
                                break;
                            }
                        }
                        if (fn_ast) {
                            extract_type_vars(fn_ast, tv_names, &tvc);
                            for (int pi = 0; pi < fn_ast->fn_decl.pattern.count; pi++) {
                                PatElem *pe = &fn_ast->fn_decl.pattern.items[pi];
                                if (pe->kind != PAT_PARAM) continue;
                                if (pe->type && (pe->type->kind == TYPE_GENERIC || pe->type->kind == TYPE_TRAIT_BOUND)) {
                                    int arg_idx = 0;
                                    for (int j = 0; j < pi; j++)
                                        if (fn_ast->fn_decl.pattern.items[j].kind == PAT_PARAM) arg_idx++;
                                    TypeRef *arg_t = NULL;
                                    if (arg_idx < node->call.args.count)
                                        arg_t = infer_expr_type(e, node->call.args.items[arg_idx]);
                                    if (!arg_t) arg_t = make_type(e->arena, TYPE_INT);
                                    for (int k = 0; k < tvc; k++) {
                                        if (strcmp(tv_names[k], pe->type->name) == 0)
                                            conc[k] = arg_t;
                                    }
                                }
                            }
                            for (int k = 0; k < tvc; k++)
                                if (!conc[k]) conc[k] = make_type(e->arena, TYPE_INT);
                            fprintf(e->out, "sigil_%s", node->call.call_name);
                            for (int k = 0; k < tvc; k++)
                                fprintf(e->out, "_%s", type_suffix(conc[k]));
                        } else {
                            emit_mangled_fn_name(e, node->call.call_name, call_arity, NULL);
                        }
                    } else {
                        int mpc = 0;
                        TypeRef *mptypes[16] = {0};
                        for (int ai = 0; ai < call_arity && ai < 16; ai++)
                            mptypes[mpc++] = infer_expr_type(e, node->call.args.items[ai]);
                        emit_mangled_fn_name(e, node->call.call_name, mpc, mptypes);
                    }
                    fprintf(e->out, "(");
                    for (int i = 0; i < call_arity; i++) {
                        if (i > 0) fprintf(e->out, ", ");
                        emit_expr(e, node->call.args.items[i]);
                    }
                    fprintf(e->out, ")");
                }
                /* If we have a thunk_id, emit thunk creation + immediate force */
                else if (thunk_id >= 0 && !(fn && fn->has_repeats) && !has_var_param) {
                    /* Determine return type for unboxing */
                    if (!call_ret && fn)
                        call_ret = fn->return_type;
                    /* In mono context, substitute generic return types */
                    if (call_ret) {
                        TypeRef *sub = mono_substitute(e, call_ret);
                        if (sub) call_ret = sub;
                    }
                    /* Also try inferring from the call expression */
                    if (!call_ret || call_ret->kind == TYPE_GENERIC ||
                        call_ret->kind == TYPE_TRAIT_BOUND || call_ret->kind == TYPE_UNKNOWN) {
                        TypeRef *inferred = infer_expr_type(e, node);
                        if (inferred && inferred->kind != TYPE_UNKNOWN &&
                            inferred->kind != TYPE_GENERIC)
                            call_ret = inferred;
                    }
                    /* Emit thunk with lazy child thunks and execute_val forcing */
                    fprintf(e->out, "({ SigilThunk *_tk = thunk_alloc(&_arena, %d, %d);",
                            thunk_id, call_arity);
                    for (int i = 0; i < call_arity; i++) {
                        fprintf(e->out, " _tk->args[%d] = ", i);
                        emit_arg_as_thunk(e, node->call.args.items[i]);
                        fprintf(e->out, ";");
                    }
                    fprintf(e->out, " ");
                    /* Force via executor pipeline (expand → classify → dispatch) and unbox */
                    if (call_ret) {
                        switch (call_ret->kind) {
                            case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
                                fprintf(e->out, "sigil_unbox_int(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))"); break;
                            case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                                fprintf(e->out, "sigil_unbox_float(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))"); break;
                            case TYPE_BOOL:
                                fprintf(e->out, "sigil_unbox_bool(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))"); break;
                            case TYPE_CHAR:
                                fprintf(e->out, "sigil_unbox_char(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))"); break;
                            case TYPE_MAP: case TYPE_NAMED:
                                fprintf(e->out, "sigil_unbox_map(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))"); break;
                            case TYPE_FN:
                                fprintf(e->out, "sigil_unbox_closure(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))"); break;
                            case TYPE_VOID:
                                fprintf(e->out, "(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw), (void)0)"); break;
                            default:
                                fprintf(e->out, "sigil_unbox_int(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))"); break;
                        }
                    } else {
                        fprintf(e->out, "sigil_unbox_int(sigil_execute_val(sigil_val_thunk(_tk), &_arena, &_hw))");
                    }
                    fprintf(e->out, "; })");
                } else {
                    /* Fallback: direct call (no thunk_id found, or repeats params) */
                    if (is_generic_call) {
                        const char *tv_names[16];
                        int tvc = 0;
                        ASTNode *fn_ast = NULL;
                        for (MonoInstance *mi = e->mono_instances; mi; mi = mi->next) {
                            if (strcmp(mi->fn_name, node->call.call_name) == 0) {
                                fn_ast = mi->fn_node;
                                break;
                            }
                        }
                        if (fn_ast) {
                            extract_type_vars(fn_ast, tv_names, &tvc);
                            TypeRef *conc[16] = {0};
                            for (int pi = 0; pi < fn_ast->fn_decl.pattern.count; pi++) {
                                PatElem *pe = &fn_ast->fn_decl.pattern.items[pi];
                                if (pe->kind != PAT_PARAM) continue;
                                if (pe->type && (pe->type->kind == TYPE_GENERIC || pe->type->kind == TYPE_TRAIT_BOUND)) {
                                    int arg_idx = 0;
                                    for (int j = 0; j < pi; j++)
                                        if (fn_ast->fn_decl.pattern.items[j].kind == PAT_PARAM)
                                            arg_idx++;
                                    TypeRef *arg_t = NULL;
                                    if (arg_idx < node->call.args.count)
                                        arg_t = infer_expr_type(e, node->call.args.items[arg_idx]);
                                    if (!arg_t) arg_t = make_type(e->arena, TYPE_INT);
                                    for (int k = 0; k < tvc; k++) {
                                        if (strcmp(tv_names[k], pe->type->name) == 0)
                                            conc[k] = arg_t;
                                    }
                                }
                            }
                            fprintf(e->out, "sigil_%s", node->call.call_name);
                            for (int k = 0; k < tvc; k++)
                                fprintf(e->out, "_%s", type_suffix(conc[k] ? conc[k] : make_type(e->arena, TYPE_INT)));
                        } else {
                            emit_fn_name(e, node->call.call_name);
                        }
                    } else if (fn) {
                        emit_mangled_fn_name(e, fn->name, fn->param_count, fn->param_types);
                    } else {
                        emit_fn_name(e, node->call.call_name);
                    }
                    fprintf(e->out, "(");
                    if (fn && fn->has_repeats) {
                        for (int i = 0; i < fn->repeats_start_idx && i < node->call.args.count; i++) {
                            if (i > 0) fprintf(e->out, ", ");
                            emit_boxed(e, node->call.args.items[i]);
                        }
                        int rcount = node->call.args.count - fn->repeats_start_idx;
                        if (rcount < 0) rcount = 0;
                        if (fn->repeats_start_idx > 0) fprintf(e->out, ", ");
                        fprintf(e->out, "%d, (SigilVal[]){", rcount);
                        for (int i = fn->repeats_start_idx; i < node->call.args.count; i++) {
                            if (i > fn->repeats_start_idx) fprintf(e->out, ", ");
                            emit_boxed(e, node->call.args.items[i]);
                        }
                        fprintf(e->out, "}");
                    } else {
                        for (int i = 0; i < node->call.args.count; i++) {
                            if (i > 0) fprintf(e->out, ", ");
                            if (fn && i < fn->param_count && fn->param_mutable[i]) {
                                fprintf(e->out, "&");
                            }
                            emit_expr(e, node->call.args.items[i]);
                        }
                    }
                    fprintf(e->out, ")");
                }
            }
            call_done:
            break;
        }

        case NODE_BEGIN_END: {
            /* GCC statement expression */
            fprintf(e->out, "({");
            for (int i = 0; i < node->block.stmts.count; i++) {
                ASTNode *s = node->block.stmts.items[i];
                if (i == node->block.stmts.count - 1 &&
                    (s->kind == NODE_CALL || s->kind == NODE_IDENT ||
                     s->kind == NODE_INT_LIT || s->kind == NODE_FLOAT_LIT ||
                     s->kind == NODE_BOOL_LIT || s->kind == NODE_STRING_LIT)) {
                    /* Last expression is the value */
                    fprintf(e->out, " ");
                    emit_expr(e, s);
                    fprintf(e->out, ";");
                } else {
                    fprintf(e->out, " ");
                    emit_stmt(e, s);
                }
            }
            fprintf(e->out, " })");
            break;
        }

        case NODE_LAMBDA: {
            /* Lambda was pre-collected; reuse its assigned ID */
            int id = node->lambda.lambda_id;
            if (id < 0) {
                /* Fallback: not pre-collected (shouldn't happen) */
                CaptureList caps = {.count = 0};
                const char *bound[32];
                int bc = node->lambda.lambda_param_count < 32 ? node->lambda.lambda_param_count : 32;
                for (int i = 0; i < bc; i++)
                    bound[i] = node->lambda.lambda_param_names[i];
                collect_free_vars(e, node->lambda.lambda_body, bound, bc, &caps);
                id = register_lambda(e, node, &caps);
            }

            /* Find capture info from the registered lambda */
            int cap_count = 0;
            LambdaEntry *le_found = NULL;
            for (LambdaEntry *le = e->lambdas; le; le = le->next) {
                if (le->id == id) { le_found = le; cap_count = le->capture_count; break; }
            }

            /* Emit closure creation as GCC statement expression */
            fprintf(e->out, "({ SigilClosure *_cl = sigil_closure_new((void*)_sigil_lambda_%d, %d);",
                    id, cap_count);
            if (le_found) {
                for (int i = 0; i < le_found->capture_count; i++) {
                    TypeRef *ct = le_found->capture_types[i];
                    const char *box_fn = "sigil_val_int";
                    if (ct) {
                        switch (ct->kind) {
                            case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                                box_fn = "sigil_val_float"; break;
                            case TYPE_BOOL: box_fn = "sigil_val_bool"; break;
                            case TYPE_CHAR: box_fn = "sigil_val_char"; break;
                            case TYPE_MAP: case TYPE_NAMED: box_fn = "sigil_val_map"; break;
                            case TYPE_FN: box_fn = "sigil_val_closure"; break;
                            default: box_fn = "sigil_val_int"; break;
                        }
                    }
                    fprintf(e->out, " sigil_closure_set_capture(_cl, %d, %s(%s));",
                            i, box_fn, le_found->capture_names[i]);
                }
            }
            fprintf(e->out, " _cl; })");
            break;
        }

        case NODE_COMPREHENSION: {
            if (node->parallel) {
                ParallelAnnotation *ann = (ParallelAnnotation *)node->parallel;
                fprintf(e->out, "/* PAR_STRATEGY: %s", parallel_strategy_name(ann->strategy));
                if (ann->is_pure) fprintf(e->out, " [pure]");
                fprintf(e->out, " */ ");
            }
            /* Emit as GCC statement expression */
            fprintf(e->out, "({ SigilMap *_comp_result = sigil_map_new();\n");

            /* Determine source type for iteration */
            TypeRef *src_type = infer_expr_type(e, node->comprehension.comp_source);

            /* Check if source is a range call */
            ASTNode *src = node->comprehension.comp_source;
            bool is_range = (src->kind == NODE_CALL && strcmp(src->call.call_name, "range") == 0);

            if (is_range) {
                fprintf(e->out, "SigilIter _comp_it = sigil_range(");
                if (src->call.args.count >= 2) {
                    emit_expr(e, src->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_expr(e, src->call.args.items[1]);
                }
                fprintf(e->out, ");\n");
            } else if (src_type && (src_type->kind == TYPE_MAP || src_type->kind == TYPE_NAMED)) {
                /* Check for Associative trait */
                const char *type_name = NULL;
                if (src_type->kind == TYPE_NAMED) type_name = src_type->name;
                else type_name = "map";

                bool is_assoc = type_has_trait(e, type_name, "Associative");
                (void)is_assoc; /* Used for future key-preserving semantics */

                fprintf(e->out, "SigilIter _comp_it = sigil_map_iter(");
                emit_expr(e, src);
                fprintf(e->out, ");\n");
            } else {
                fprintf(e->out, "SigilIter _comp_it = ");
                emit_expr(e, src);
                fprintf(e->out, ";\n");
            }

            fprintf(e->out, "int64_t _comp_idx = 0;\n");
            fprintf(e->out, "while (sigil_iter_has_next(&_comp_it)) {\n");

            /* Bind iteration variable */
            if (is_range) {
                fprintf(e->out, "int64_t %s = sigil_unbox_int(sigil_iter_next(&_comp_it));\n",
                        node->comprehension.comp_var);
            } else if (src_type && src_type->kind == TYPE_MAP && src_type->key_type) {
                emit_c_type(e, src_type->key_type);
                fprintf(e->out, " %s = ", node->comprehension.comp_var);
                emit_unbox_prefix(e, src_type->key_type);
                fprintf(e->out, "sigil_iter_next(&_comp_it));\n");
            } else {
                fprintf(e->out, "int64_t %s = sigil_unbox_int(sigil_iter_next(&_comp_it));\n",
                        node->comprehension.comp_var);
            }

            /* Filter */
            if (node->comprehension.comp_filter) {
                fprintf(e->out, "if (!(");
                emit_expr(e, node->comprehension.comp_filter);
                fprintf(e->out, ")) continue;\n");
            }

            /* Transform and store */
            fprintf(e->out, "sigil_map_set(_comp_result, sigil_val_int(_comp_idx), ");
            emit_boxed(e, node->comprehension.comp_transform);
            fprintf(e->out, ");\n");
            fprintf(e->out, "_comp_idx++;\n");
            fprintf(e->out, "}\n");
            fprintf(e->out, "_comp_result; })");
            break;
        }

        default:
            fprintf(e->out, "/* <unsupported expr %d> */0", node->kind);
            break;
    }
}

/* ── Statement Emission ──────────────────────────────────────────── */

static void emit_stmt(CEmitter *e, ASTNode *node) {
    if (!node) return;

    switch (node->kind) {
        case NODE_LET:
            emit_indent(e);
            {
                TypeRef *t = infer_expr_type(e, node->binding.value);
                /* Don't use const for pointer types (map, UDT, closures) — causes warnings */
                if (t && (t->kind == TYPE_MAP || t->kind == TYPE_NAMED || t->kind == TYPE_FN))
                    ; /* no const */
                else
                    fprintf(e->out, "const ");
                emit_c_type(e, t);
            }
            fprintf(e->out, " %s = ", node->binding.bind_name);
            emit_expr(e, node->binding.value);
            fprintf(e->out, ";\n");
            break;

        case NODE_VAR:
            emit_indent(e);
            {
                TypeRef *t = infer_expr_type(e, node->binding.value);
                emit_c_type(e, t);
            }
            fprintf(e->out, " %s = ", node->binding.bind_name);
            emit_expr(e, node->binding.value);
            fprintf(e->out, ";\n");
            break;

        case NODE_ASSIGN:
            emit_indent(e);
            if (is_var_param(e, node->assign.assign_name)) {
                fprintf(e->out, "(*%s) = ", node->assign.assign_name);
            } else {
                fprintf(e->out, "%s = ", node->assign.assign_name);
            }
            emit_expr(e, node->assign.value);
            fprintf(e->out, ";\n");
            break;

        case NODE_RETURN:
            emit_indent(e);
            fprintf(e->out, "return ");
            emit_expr(e, node->ret.value);
            fprintf(e->out, ";\n");
            break;

        case NODE_IF:
            emit_indent(e);
            fprintf(e->out, "if (");
            emit_expr(e, node->if_stmt.condition);
            fprintf(e->out, ") {\n");
            e->indent++;
            emit_body(e, node->if_stmt.then_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "}");
            for (int i = 0; i < node->if_stmt.elifs.count; i += 2) {
                fprintf(e->out, " else if (");
                emit_expr(e, node->if_stmt.elifs.items[i]);
                fprintf(e->out, ") {\n");
                e->indent++;
                emit_body(e, node->if_stmt.elifs.items[i + 1]);
                e->indent--;
                emit_indent(e);
                fprintf(e->out, "}");
            }
            if (node->if_stmt.else_body) {
                fprintf(e->out, " else {\n");
                e->indent++;
                emit_body(e, node->if_stmt.else_body);
                e->indent--;
                emit_indent(e);
                fprintf(e->out, "}");
            }
            fprintf(e->out, "\n");
            break;

        case NODE_WHILE:
            emit_indent(e);
            fprintf(e->out, "while (");
            emit_expr(e, node->while_stmt.condition);
            fprintf(e->out, ") {\n");
            e->indent++;
            emit_body(e, node->while_stmt.while_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "}\n");
            break;

        case NODE_FOR:
            if (node->parallel) {
                ParallelAnnotation *ann = (ParallelAnnotation *)node->parallel;
                emit_indent(e);
                fprintf(e->out, "/* PAR_STRATEGY: %s", parallel_strategy_name(ann->strategy));
                if (ann->reduction_fn)
                    fprintf(e->out, " (reduction over \"%s\")", ann->reduction_fn);
                if (ann->is_pure)
                    fprintf(e->out, " [pure]");
                fprintf(e->out, " */\n");
            }
            emit_indent(e);
            fprintf(e->out, "{\n");
            e->indent++;
            emit_indent(e);
            {
                TypeRef *iter_type = infer_expr_type(e, node->for_stmt.iterable);
                /* Detect range call (may be wrapped in begin/end) */
                ASTNode *iter_inner = node->for_stmt.iterable;
                if (iter_inner->kind == NODE_BEGIN_END && iter_inner->block.stmts.count == 1)
                    iter_inner = iter_inner->block.stmts.items[0];
                bool is_range = (iter_inner->kind == NODE_CALL &&
                                 strcmp(iter_inner->call.call_name, "range") == 0);
                if (is_range) {
                    fprintf(e->out, "SigilIter _it = sigil_range(");
                    if (iter_inner->call.args.count >= 2) {
                        emit_expr(e, iter_inner->call.args.items[0]);
                        fprintf(e->out, ", ");
                        emit_expr(e, iter_inner->call.args.items[1]);
                    }
                    fprintf(e->out, ");\n");
                } else if (iter_type && iter_type->kind == TYPE_MAP) {
                    fprintf(e->out, "SigilIter _it = sigil_map_iter(");
                    emit_expr(e, node->for_stmt.iterable);
                    fprintf(e->out, ");\n");
                } else {
                    fprintf(e->out, "SigilIter _it = ");
                    emit_expr(e, node->for_stmt.iterable);
                    fprintf(e->out, ";\n");
                }
                emit_indent(e);
                fprintf(e->out, "while (sigil_iter_has_next(&_it)) {\n");
                e->indent++;
                emit_indent(e);
                /* Auto-unbox: determine element type */
                if (is_range) {
                    fprintf(e->out, "int64_t %s = sigil_unbox_int(sigil_iter_next(&_it));\n",
                            node->for_stmt.var_name);
                } else if (iter_type && iter_type->kind == TYPE_MAP && iter_type->key_type) {
                    emit_c_type(e, iter_type->key_type);
                    fprintf(e->out, " %s = ", node->for_stmt.var_name);
                    emit_unbox_prefix(e, iter_type->key_type);
                    fprintf(e->out, "sigil_iter_next(&_it));\n");
                } else {
                    fprintf(e->out, "SigilVal %s = sigil_iter_next(&_it);\n",
                            node->for_stmt.var_name);
                }
            }
            emit_body(e, node->for_stmt.for_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "}\n");
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "}\n");
            break;

        case NODE_MATCH:
            emit_indent(e);
            fprintf(e->out, "{\n");
            e->indent++;
            emit_indent(e);
            {
                TypeRef *mt = infer_expr_type(e, node->match_stmt.match_value);
                emit_c_type(e, mt);
            }
            fprintf(e->out, " _match_val = ");
            emit_expr(e, node->match_stmt.match_value);
            fprintf(e->out, ";\n");
            for (int i = 0; i < node->match_stmt.cases.count; i++) {
                ASTNode *c = node->match_stmt.cases.items[i];
                if (c->kind == NODE_DEFAULT) {
                    if (i > 0) {
                        fprintf(e->out, " else {\n");
                    } else {
                        emit_indent(e);
                        fprintf(e->out, "{\n");
                    }
                    e->indent++;
                    emit_body(e, c->default_branch.default_body);
                    e->indent--;
                    emit_indent(e);
                    fprintf(e->out, "}");
                } else if (c->kind == NODE_CASE) {
                    emit_indent(e);
                    if (i > 0) fprintf(e->out, "else ");
                    fprintf(e->out, "if (_match_val == ");
                    emit_expr(e, c->case_branch.case_pattern);
                    fprintf(e->out, ") {\n");
                    e->indent++;
                    emit_body(e, c->case_branch.case_body);
                    e->indent--;
                    emit_indent(e);
                    fprintf(e->out, "}");
                }
            }
            fprintf(e->out, "\n");
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "}\n");
            break;

        case NODE_CALL:
        case NODE_CHAIN:
            emit_indent(e);
            emit_expr(e, node);
            fprintf(e->out, ";\n");
            break;

        case NODE_BREAK:
            emit_indent(e);
            fprintf(e->out, "break;\n");
            break;

        case NODE_CONTINUE:
            emit_indent(e);
            fprintf(e->out, "continue;\n");
            break;

        case NODE_TYPE_DECL:
            /* Type declarations don't emit C code */
            break;

        default:
            /* Expressions used as statements */
            emit_indent(e);
            emit_expr(e, node);
            fprintf(e->out, ";\n");
            break;
    }
}

/* ── Body Emission ───────────────────────────────────────────────── */

static void emit_body(CEmitter *e, ASTNode *node) {
    if (!node) return;
    if (node->kind == NODE_BLOCK || node->kind == NODE_BEGIN_END) {
        for (int i = 0; i < node->block.stmts.count; i++)
            emit_stmt(e, node->block.stmts.items[i]);
    } else {
        emit_stmt(e, node);
    }
}

/* ── Function Declaration ────────────────────────────────────────── */

static void emit_fn_prototype(CEmitter *e, ASTNode *fn) {
    if (fn->fn_decl.is_primitive) return;
    if (!fn->fn_decl.body && find_builtin(fn->fn_decl.fn_name)) return;

    /* Collect param types for mangling */
    int pc = 0;
    TypeRef *ptypes[32];
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM && pc < 32)
            ptypes[pc++] = fn->fn_decl.pattern.items[i].type;
    }

    emit_c_type(e, fn->fn_decl.return_type);
    fprintf(e->out, " ");
    emit_mangled_fn_name(e, fn->fn_decl.fn_name, pc, ptypes);
    fprintf(e->out, "(");

    bool first = true;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_REPEATS) {
            if (!first) fprintf(e->out, ", ");
            first = false;
            fprintf(e->out, "int _repeats_count, SigilVal *_repeats_data");
            continue;
        }
        if (pe->kind != PAT_PARAM) continue;
        if (!first) fprintf(e->out, ", ");
        first = false;

        if (pe->is_mutable) {
            emit_c_type(e, pe->type);
            fprintf(e->out, " *%s", pe->param_name);
        } else {
            emit_c_type(e, pe->type);
            fprintf(e->out, " %s", pe->param_name);
        }
    }

    if (first) fprintf(e->out, "void");
    fprintf(e->out, ")");
}

static void emit_fn_decl(CEmitter *e, ASTNode *fn) {
    if (fn->fn_decl.is_primitive) return;
    if (!fn->fn_decl.body) return;

    emit_fn_prototype(e, fn);
    fprintf(e->out, " {\n");
    e->indent++;

    /* Track var params for dereference in body */
    const char **prev_var_params = e->var_params;
    int prev_var_param_count = e->var_param_count;
    const char *vp[32];
    int vpc = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM && pe->is_mutable && pe->param_name && vpc < 32)
            vp[vpc++] = pe->param_name;
    }
    e->var_params = vp;
    e->var_param_count = vpc;

    bool prev_in_fn_body = e->in_fn_body;
    const char *prev_fn_name = e->current_fn_name;
    e->in_fn_body = true;
    e->current_fn_name = fn->fn_decl.fn_name;

    emit_body(e, fn->fn_decl.body);

    e->in_fn_body = prev_in_fn_body;
    e->current_fn_name = prev_fn_name;
    e->var_params = prev_var_params;
    e->var_param_count = prev_var_param_count;
    e->indent--;
    fprintf(e->out, "}\n\n");
}

/* ── Collect all fn declarations (recursive) ─────────────────────── */

typedef struct { ASTNode **items; int count; int capacity; } FnList;

static void collect_fns(ASTNode *node, FnList *fns) {
    if (!node) return;
    switch (node->kind) {
        case NODE_FN_DECL:
            if (fns->count >= fns->capacity) {
                fns->capacity = fns->capacity ? fns->capacity * 2 : 32;
                fns->items = realloc(fns->items, fns->capacity * sizeof(ASTNode *));
            }
            fns->items[fns->count++] = node;
            break;
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.top_level.count; i++)
                collect_fns(node->program.top_level.items[i], fns);
            break;
        case NODE_ALGEBRA:
        case NODE_LIBRARY:
            for (int i = 0; i < node->algebra.declarations.count; i++)
                collect_fns(node->algebra.declarations.items[i], fns);
            break;
        case NODE_TRAIT_DECL:
            for (int i = 0; i < node->trait_decl.methods.count; i++)
                collect_fns(node->trait_decl.methods.items[i], fns);
            break;
        case NODE_IMPLEMENT:
            for (int i = 0; i < node->implement.methods.count; i++)
                collect_fns(node->implement.methods.items[i], fns);
            break;
        case NODE_IMPORT:
            for (int i = 0; i < node->import_decl.declarations.count; i++)
                collect_fns(node->import_decl.declarations.items[i], fns);
            break;
        case NODE_USE:
            collect_fns(node->use_block.body, fns);
            break;
        case NODE_BLOCK:
        case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                collect_fns(node->block.stmts.items[i], fns);
            break;
        default:
            break;
    }
}

/* ── Monomorphization: collect and emit generic specializations ───── */

static bool mono_instance_exists(CEmitter *e, const char *fn_name, int tvc, TypeRef **conc) {
    for (MonoInstance *m = e->mono_instances; m; m = m->next) {
        if (strcmp(m->fn_name, fn_name) != 0 || m->type_var_count != tvc) continue;
        bool match = true;
        for (int i = 0; i < tvc; i++) {
            if (!types_equal(m->concrete_types[i], conc[i])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static FnEntry *find_fn_entry(CEmitter *e, const char *name) {
    for (FnEntry *f = e->tc->fn_registry; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static bool fn_entry_is_generic(FnEntry *fn) {
    for (int i = 0; i < fn->param_count; i++) {
        if (fn->param_types[i] &&
            (fn->param_types[i]->kind == TYPE_GENERIC || fn->param_types[i]->kind == TYPE_TRAIT_BOUND))
            return true;
    }
    return false;
}

/* Extract type var names and count from a generic fn AST node */
static void extract_type_vars(ASTNode *fn, const char **names, int *count) {
    *count = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM && pe->type &&
            (pe->type->kind == TYPE_GENERIC || pe->type->kind == TYPE_TRAIT_BOUND)) {
            const char *tv = pe->type->name;
            /* Dedup: don't add same type var twice */
            bool dup = false;
            for (int j = 0; j < *count; j++)
                if (strcmp(names[j], tv) == 0) { dup = true; break; }
            if (!dup && *count < 16)
                names[(*count)++] = tv;
        }
    }
}

static void add_mono_instance(CEmitter *e, ASTNode *fn_node, const char *fn_name,
                               int tvc, const char **tv_names, TypeRef **conc) {
    if (mono_instance_exists(e, fn_name, tvc, conc)) return;
    MonoInstance *m = arena_alloc(e->arena, sizeof(MonoInstance));
    m->fn_node = fn_node;
    m->fn_name = fn_name;
    m->type_var_count = tvc;
    m->type_var_names = arena_alloc(e->arena, tvc * sizeof(const char *));
    m->concrete_types = arena_alloc(e->arena, tvc * sizeof(TypeRef *));
    for (int i = 0; i < tvc; i++) {
        m->type_var_names[i] = tv_names[i];
        m->concrete_types[i] = conc[i];
    }
    m->next = e->mono_instances;
    e->mono_instances = m;
}

/* Find the fn AST node by name in the collected fn list */
static ASTNode *find_fn_ast(FnList *fns, const char *name) {
    for (int i = 0; i < fns->count; i++) {
        if (strcmp(fns->items[i]->fn_decl.fn_name, name) == 0 && fn_is_generic(fns->items[i]))
            return fns->items[i];
    }
    return NULL;
}

static void collect_mono_from_node(CEmitter *e, ASTNode *node, FnList *fns);

static void collect_mono_from_children(CEmitter *e, ASTNode *node, FnList *fns) {
    if (!node) return;
    switch (node->kind) {
        case NODE_CALL:
            for (int i = 0; i < node->call.args.count; i++)
                collect_mono_from_node(e, node->call.args.items[i], fns);
            break;
        case NODE_CHAIN:
            for (int i = 0; i < node->chain.chain_operands.count; i++)
                collect_mono_from_node(e, node->chain.chain_operands.items[i], fns);
            break;
        case NODE_BLOCK: case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                collect_mono_from_node(e, node->block.stmts.items[i], fns);
            break;
        case NODE_LET: case NODE_VAR:
            collect_mono_from_node(e, node->binding.value, fns);
            break;
        case NODE_ASSIGN:
            collect_mono_from_node(e, node->assign.value, fns);
            break;
        case NODE_RETURN:
            collect_mono_from_node(e, node->ret.value, fns);
            break;
        case NODE_IF:
            collect_mono_from_node(e, node->if_stmt.condition, fns);
            collect_mono_from_node(e, node->if_stmt.then_body, fns);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                collect_mono_from_node(e, node->if_stmt.elifs.items[i], fns);
            collect_mono_from_node(e, node->if_stmt.else_body, fns);
            break;
        case NODE_WHILE:
            collect_mono_from_node(e, node->while_stmt.condition, fns);
            collect_mono_from_node(e, node->while_stmt.while_body, fns);
            break;
        case NODE_FOR:
            collect_mono_from_node(e, node->for_stmt.iterable, fns);
            collect_mono_from_node(e, node->for_stmt.for_body, fns);
            break;
        case NODE_MATCH:
            collect_mono_from_node(e, node->match_stmt.match_value, fns);
            for (int i = 0; i < node->match_stmt.cases.count; i++)
                collect_mono_from_node(e, node->match_stmt.cases.items[i], fns);
            break;
        case NODE_CASE:
            collect_mono_from_node(e, node->case_branch.case_pattern, fns);
            collect_mono_from_node(e, node->case_branch.case_body, fns);
            break;
        case NODE_DEFAULT:
            collect_mono_from_node(e, node->default_branch.default_body, fns);
            break;
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.top_level.count; i++)
                collect_mono_from_node(e, node->program.top_level.items[i], fns);
            break;
        case NODE_ALGEBRA: case NODE_LIBRARY:
            for (int i = 0; i < node->algebra.declarations.count; i++)
                collect_mono_from_node(e, node->algebra.declarations.items[i], fns);
            break;
        case NODE_IMPORT:
            for (int i = 0; i < node->import_decl.declarations.count; i++)
                collect_mono_from_node(e, node->import_decl.declarations.items[i], fns);
            break;
        case NODE_USE:
            collect_mono_from_node(e, node->use_block.body, fns);
            break;
        case NODE_FN_DECL:
            /* Only collect from non-generic fn bodies; generic bodies are handled per-instance */
            if (!fn_is_generic(node))
                collect_mono_from_node(e, node->fn_decl.body, fns);
            break;
        case NODE_IMPLEMENT:
            for (int i = 0; i < node->implement.methods.count; i++)
                collect_mono_from_node(e, node->implement.methods.items[i], fns);
            break;
        case NODE_TRAIT_DECL:
            for (int i = 0; i < node->trait_decl.methods.count; i++)
                collect_mono_from_node(e, node->trait_decl.methods.items[i], fns);
            break;
        case NODE_LAMBDA:
            collect_mono_from_node(e, node->lambda.lambda_body, fns);
            break;
        case NODE_COMPREHENSION:
            collect_mono_from_node(e, node->comprehension.comp_source, fns);
            collect_mono_from_node(e, node->comprehension.comp_filter, fns);
            collect_mono_from_node(e, node->comprehension.comp_transform, fns);
            break;
        default:
            break;
    }
}

/* Build mono instance from a generic call site. Returns true if a new instance was added. */
static bool try_add_mono_from_call(CEmitter *e, ASTNode *node, FnList *fns) {
    if (!node || node->kind != NODE_CALL) return false;
    const char *cn = node->call.call_name;
    FnEntry *fe = find_fn_entry(e, cn);
    if (!fe || !fn_entry_is_generic(fe)) return false;
    ASTNode *fn_ast = find_fn_ast(fns, cn);
    if (!fn_ast) return false;

    const char *tv_names[16];
    int tvc = 0;
    extract_type_vars(fn_ast, tv_names, &tvc);

    TypeRef *conc[16] = {0};
    int param_idx = 0;
    for (int i = 0; i < fn_ast->fn_decl.pattern.count && param_idx < tvc; i++) {
        PatElem *pe = &fn_ast->fn_decl.pattern.items[i];
        if (pe->kind != PAT_PARAM) continue;
        if (pe->type && (pe->type->kind == TYPE_GENERIC || pe->type->kind == TYPE_TRAIT_BOUND)) {
            const char *tv = pe->type->name;
            int arg_idx = 0;
            for (int j = 0; j < i; j++)
                if (fn_ast->fn_decl.pattern.items[j].kind == PAT_PARAM)
                    arg_idx++;
            TypeRef *arg_t = NULL;
            if (arg_idx < node->call.args.count)
                arg_t = infer_expr_type(e, node->call.args.items[arg_idx]);
            if (!arg_t) arg_t = make_type(e->arena, TYPE_INT);
            for (int k = 0; k < tvc; k++) {
                if (strcmp(tv_names[k], tv) == 0) {
                    conc[k] = arg_t;
                    break;
                }
            }
        }
        param_idx++;
    }

    for (int i = 0; i < tvc; i++)
        if (!conc[i]) conc[i] = make_type(e->arena, TYPE_INT);

    bool existed = mono_instance_exists(e, cn, tvc, conc);
    add_mono_instance(e, fn_ast, cn, tvc, tv_names, conc);
    return !existed;
}

static void collect_mono_from_node(CEmitter *e, ASTNode *node, FnList *fns) {
    if (!node) return;
    if (node->kind == NODE_CALL)
        try_add_mono_from_call(e, node, fns);
    collect_mono_from_children(e, node, fns);
}

/* Walk a generic fn body with a substitution to discover transitive mono instances */
static void collect_transitive_mono(CEmitter *e, ASTNode *node, FnList *fns,
                                      MonoInstance *outer) {
    if (!node) return;
    if (node->kind == NODE_CALL) {
        const char *cn = node->call.call_name;
        FnEntry *fe = find_fn_entry(e, cn);
        if (fe && fn_entry_is_generic(fe)) {
            ASTNode *fn_ast = find_fn_ast(fns, cn);
            if (fn_ast) {
                /* Set current_mono so infer_expr_type applies substitution */
                MonoInstance *prev = e->current_mono;
                e->current_mono = outer;
                try_add_mono_from_call(e, node, fns);
                e->current_mono = prev;
            }
        }
        /* Recurse into args */
        for (int i = 0; i < node->call.args.count; i++)
            collect_transitive_mono(e, node->call.args.items[i], fns, outer);
        return;
    }
    if (node->kind == NODE_CHAIN) {
        for (int i = 0; i < node->chain.chain_operands.count; i++)
            collect_transitive_mono(e, node->chain.chain_operands.items[i], fns, outer);
        return;
    }
    /* Recurse into children */
    switch (node->kind) {
        case NODE_BLOCK: case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                collect_transitive_mono(e, node->block.stmts.items[i], fns, outer);
            break;
        case NODE_LET: case NODE_VAR:
            collect_transitive_mono(e, node->binding.value, fns, outer); break;
        case NODE_ASSIGN:
            collect_transitive_mono(e, node->assign.value, fns, outer); break;
        case NODE_RETURN:
            collect_transitive_mono(e, node->ret.value, fns, outer); break;
        case NODE_IF:
            collect_transitive_mono(e, node->if_stmt.condition, fns, outer);
            collect_transitive_mono(e, node->if_stmt.then_body, fns, outer);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                collect_transitive_mono(e, node->if_stmt.elifs.items[i], fns, outer);
            collect_transitive_mono(e, node->if_stmt.else_body, fns, outer);
            break;
        case NODE_WHILE:
            collect_transitive_mono(e, node->while_stmt.condition, fns, outer);
            collect_transitive_mono(e, node->while_stmt.while_body, fns, outer);
            break;
        case NODE_FOR:
            collect_transitive_mono(e, node->for_stmt.iterable, fns, outer);
            collect_transitive_mono(e, node->for_stmt.for_body, fns, outer);
            break;
        case NODE_MATCH:
            collect_transitive_mono(e, node->match_stmt.match_value, fns, outer);
            for (int i = 0; i < node->match_stmt.cases.count; i++)
                collect_transitive_mono(e, node->match_stmt.cases.items[i], fns, outer);
            break;
        case NODE_CASE:
            collect_transitive_mono(e, node->case_branch.case_pattern, fns, outer);
            collect_transitive_mono(e, node->case_branch.case_body, fns, outer);
            break;
        case NODE_DEFAULT:
            collect_transitive_mono(e, node->default_branch.default_body, fns, outer);
            break;
        case NODE_LAMBDA:
            collect_transitive_mono(e, node->lambda.lambda_body, fns, outer);
            break;
        case NODE_COMPREHENSION:
            collect_transitive_mono(e, node->comprehension.comp_source, fns, outer);
            collect_transitive_mono(e, node->comprehension.comp_filter, fns, outer);
            collect_transitive_mono(e, node->comprehension.comp_transform, fns, outer);
            break;
        default: break;
    }
}

/* Emit a mono instance prototype */
static void emit_mono_prototype(CEmitter *e, MonoInstance *m) {
    MonoInstance *prev = e->current_mono;
    e->current_mono = m;

    ASTNode *fn = m->fn_node;
    if (fn->fn_decl.is_primitive || !fn->fn_decl.body) { e->current_mono = prev; return; }

    /* Return type (substitute if generic) */
    emit_c_type(e, fn->fn_decl.return_type);
    fprintf(e->out, " sigil_%s", m->fn_name);
    /* Add type suffixes for mangling */
    for (int i = 0; i < m->type_var_count; i++)
        fprintf(e->out, "_%s", type_suffix(m->concrete_types[i]));

    fprintf(e->out, "(");
    bool first = true;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_REPEATS) {
            if (!first) fprintf(e->out, ", ");
            first = false;
            fprintf(e->out, "int _repeats_count, SigilVal *_repeats_data");
            continue;
        }
        if (pe->kind != PAT_PARAM) continue;
        if (!first) fprintf(e->out, ", ");
        first = false;
        if (pe->is_mutable) {
            emit_c_type(e, pe->type);
            fprintf(e->out, " *%s", pe->param_name);
        } else {
            emit_c_type(e, pe->type);
            fprintf(e->out, " %s", pe->param_name);
        }
    }
    if (first) fprintf(e->out, "void");
    fprintf(e->out, ")");

    e->current_mono = prev;
}

/* Emit a mono instance body */
static void emit_mono_body(CEmitter *e, MonoInstance *m) {
    MonoInstance *prev = e->current_mono;
    e->current_mono = m;

    ASTNode *fn = m->fn_node;
    if (fn->fn_decl.is_primitive || !fn->fn_decl.body) { e->current_mono = prev; return; }

    emit_mono_prototype(e, m);
    fprintf(e->out, " {\n");
    e->indent++;

    /* Track var params */
    const char **prev_var_params = e->var_params;
    int prev_vpc = e->var_param_count;
    const char *vp[32];
    int vpc = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM && pe->is_mutable && pe->param_name && vpc < 32)
            vp[vpc++] = pe->param_name;
    }
    e->var_params = vp;
    e->var_param_count = vpc;

    bool prev_in_fn_body = e->in_fn_body;
    const char *prev_fn_name = e->current_fn_name;
    e->in_fn_body = true;
    e->current_fn_name = fn->fn_decl.fn_name;

    emit_body(e, fn->fn_decl.body);

    e->in_fn_body = prev_in_fn_body;
    e->current_fn_name = prev_fn_name;
    e->var_params = prev_var_params;
    e->var_param_count = prev_vpc;
    e->indent--;
    fprintf(e->out, "}\n\n");

    e->current_mono = prev;
}

/* ── Pre-collect lambdas from the AST ────────────────────────────── */

static void precollect_lambdas(CEmitter *e, ASTNode *node) {
    if (!node) return;
    if (node->kind == NODE_LAMBDA) {
        CaptureList caps = {.count = 0};
        const char *bound[32];
        int bc = node->lambda.lambda_param_count < 32 ? node->lambda.lambda_param_count : 32;
        for (int i = 0; i < bc; i++)
            bound[i] = node->lambda.lambda_param_names[i];
        collect_free_vars(e, node->lambda.lambda_body, bound, bc, &caps);
        register_lambda(e, node, &caps);
        /* Recurse into lambda body for nested lambdas */
        precollect_lambdas(e, node->lambda.lambda_body);
        return;
    }
    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.top_level.count; i++)
                precollect_lambdas(e, node->program.top_level.items[i]);
            break;
        case NODE_ALGEBRA: case NODE_LIBRARY:
            for (int i = 0; i < node->algebra.declarations.count; i++)
                precollect_lambdas(e, node->algebra.declarations.items[i]);
            break;
        case NODE_IMPORT:
            for (int i = 0; i < node->import_decl.declarations.count; i++)
                precollect_lambdas(e, node->import_decl.declarations.items[i]);
            break;
        case NODE_USE:
            precollect_lambdas(e, node->use_block.body);
            break;
        case NODE_BLOCK: case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                precollect_lambdas(e, node->block.stmts.items[i]);
            break;
        case NODE_FN_DECL:
            precollect_lambdas(e, node->fn_decl.body);
            break;
        case NODE_IMPLEMENT:
            for (int i = 0; i < node->implement.methods.count; i++)
                precollect_lambdas(e, node->implement.methods.items[i]);
            break;
        case NODE_TRAIT_DECL:
            for (int i = 0; i < node->trait_decl.methods.count; i++)
                precollect_lambdas(e, node->trait_decl.methods.items[i]);
            break;
        case NODE_LET: case NODE_VAR:
            precollect_lambdas(e, node->binding.value);
            break;
        case NODE_ASSIGN:
            precollect_lambdas(e, node->assign.value);
            break;
        case NODE_RETURN:
            precollect_lambdas(e, node->ret.value);
            break;
        case NODE_CALL:
            for (int i = 0; i < node->call.args.count; i++)
                precollect_lambdas(e, node->call.args.items[i]);
            break;
        case NODE_CHAIN:
            for (int i = 0; i < node->chain.chain_operands.count; i++)
                precollect_lambdas(e, node->chain.chain_operands.items[i]);
            break;
        case NODE_IF:
            precollect_lambdas(e, node->if_stmt.condition);
            precollect_lambdas(e, node->if_stmt.then_body);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                precollect_lambdas(e, node->if_stmt.elifs.items[i]);
            precollect_lambdas(e, node->if_stmt.else_body);
            break;
        case NODE_WHILE:
            precollect_lambdas(e, node->while_stmt.condition);
            precollect_lambdas(e, node->while_stmt.while_body);
            break;
        case NODE_FOR:
            precollect_lambdas(e, node->for_stmt.iterable);
            precollect_lambdas(e, node->for_stmt.for_body);
            break;
        case NODE_COMPREHENSION:
            precollect_lambdas(e, node->comprehension.comp_source);
            precollect_lambdas(e, node->comprehension.comp_filter);
            precollect_lambdas(e, node->comprehension.comp_transform);
            break;
        case NODE_MATCH:
            precollect_lambdas(e, node->match_stmt.match_value);
            for (int i = 0; i < node->match_stmt.cases.count; i++)
                precollect_lambdas(e, node->match_stmt.cases.items[i]);
            break;
        case NODE_CASE:
            precollect_lambdas(e, node->case_branch.case_pattern);
            precollect_lambdas(e, node->case_branch.case_body);
            break;
        case NODE_DEFAULT:
            precollect_lambdas(e, node->default_branch.default_body);
            break;
        default:
            break;
    }
}

/* ── Collect top-level statements (non-fn) ───────────────────────── */

static bool is_executable_stmt(ASTNode *node) {
    if (!node) return false;
    switch (node->kind) {
        case NODE_LET: case NODE_VAR: case NODE_ASSIGN: case NODE_RETURN:
        case NODE_BREAK: case NODE_CONTINUE:
        case NODE_IF: case NODE_WHILE: case NODE_FOR: case NODE_MATCH:
        case NODE_CALL: case NODE_CHAIN: case NODE_IDENT: case NODE_INT_LIT:
        case NODE_FLOAT_LIT: case NODE_BOOL_LIT: case NODE_STRING_LIT:
        case NODE_BEGIN_END: case NODE_BLOCK:
        case NODE_LAMBDA: case NODE_COMPREHENSION:
            return true;
        default:
            return false;
    }
}

static void collect_top_stmts(ASTNode *node, FnList *stmts) {
    if (!node) return;
    if (node->kind == NODE_PROGRAM) {
        for (int i = 0; i < node->program.top_level.count; i++)
            collect_top_stmts(node->program.top_level.items[i], stmts);
    } else if (node->kind == NODE_USE) {
        collect_top_stmts(node->use_block.body, stmts);
    } else if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmts.count; i++)
            collect_top_stmts(node->block.stmts.items[i], stmts);
    } else if (node->kind == NODE_IMPORT) {
        /* Do NOT recurse into imports — imported fn bodies are emitted
           as separate function definitions, not as top-level statements */
    } else if (node->kind == NODE_ALGEBRA || node->kind == NODE_LIBRARY) {
        for (int i = 0; i < node->algebra.declarations.count; i++)
            collect_top_stmts(node->algebra.declarations.items[i], stmts);
    } else if (is_executable_stmt(node)) {
        if (stmts->count >= stmts->capacity) {
            stmts->capacity = stmts->capacity ? stmts->capacity * 2 : 32;
            stmts->items = realloc(stmts->items, stmts->capacity * sizeof(ASTNode *));
        }
        stmts->items[stmts->count++] = node;
    }
}

/* ── Top-level Emission ──────────────────────────────────────────── */

/* ── Thunk evaluator wrapper emission ────────────────────────────── */

/* ── Evaluator body walking (graph-driven execution) ─────────────── */

/* Counter for child index in evaluator body emission */
static int eval_child_idx;

/* Emit an unbox call for a given type around a SigilVal expression */
static const char *eval_unbox_func(TypeRef *t) {
    if (t) {
        switch (t->kind) {
            case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
                return "sigil_unbox_int";
            case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                return "sigil_unbox_float";
            case TYPE_BOOL: return "sigil_unbox_bool";
            case TYPE_CHAR: return "sigil_unbox_char";
            case TYPE_MAP: case TYPE_NAMED: return "sigil_unbox_map";
            case TYPE_FN: return "sigil_unbox_closure";
            default: break;
        }
    }
    return "sigil_unbox_int";
}

/* Emit an expression in evaluator context: _argN for params, inline builtins,
   thunk_force for recursive children, direct C calls for other user fns */
static void emit_eval_expr(CEmitter *e, ASTNode *node, const char *self_name,
                            int param_count, const char **pnames, TypeRef *ret_type) {
    if (!node) { fprintf(e->out, "0"); return; }
    if (node->kind == NODE_CHAIN) { emit_eval_expr(e, chain_to_calls(e->arena, node), self_name, param_count, pnames, ret_type); return; }
    switch (node->kind) {
        case NODE_INT_LIT:
            fprintf(e->out, "%lldLL", (long long)node->int_lit.int_val);
            return;
        case NODE_FLOAT_LIT:
            fprintf(e->out, "%g", node->float_lit.float_val);
            return;
        case NODE_BOOL_LIT:
            fprintf(e->out, "%s", node->bool_lit.bool_val ? "true" : "false");
            return;
        case NODE_IDENT:
            for (int i = 0; i < param_count; i++) {
                if (strcmp(node->ident.ident, pnames[i]) == 0) {
                    fprintf(e->out, "_arg%d", i);
                    return;
                }
            }
            fprintf(e->out, "%s", node->ident.ident);
            return;
        case NODE_CALL: {
            /* Recursive self-call → force child from graph */
            if (strcmp(node->call.call_name, self_name) == 0) {
                fprintf(e->out, "%s(sigil_force_val(thunk_force(_t->children[%d], _arena), _arena))",
                        eval_unbox_func(ret_type), eval_child_idx);
                eval_child_idx++;
                return;
            }
            /* Builtin binary op */
            const BuiltinOp *bi = find_builtin(node->call.call_name);
            if (bi && bi->c_op && node->call.args.count == 2) {
                fprintf(e->out, "(");
                emit_eval_expr(e, node->call.args.items[0], self_name, param_count, pnames, ret_type);
                fprintf(e->out, " %s ", bi->c_op);
                emit_eval_expr(e, node->call.args.items[1], self_name, param_count, pnames, ret_type);
                fprintf(e->out, ")");
                return;
            }
            /* Builtin unary op */
            if (bi && bi->is_unary_prefix && node->call.args.count == 1) {
                const char *op = strcmp(bi->name, "negate") == 0 ? "-" : "!";
                fprintf(e->out, "(%s", op);
                emit_eval_expr(e, node->call.args.items[0], self_name, param_count, pnames, ret_type);
                fprintf(e->out, ")");
                return;
            }
            /* Other user fn: direct C call */
            fprintf(e->out, "sigil_%s(", node->call.call_name);
            for (int i = 0; i < node->call.args.count; i++) {
                if (i > 0) fprintf(e->out, ", ");
                emit_eval_expr(e, node->call.args.items[i], self_name, param_count, pnames, ret_type);
            }
            fprintf(e->out, ")");
            return;
        }
        case NODE_BEGIN_END: case NODE_BLOCK:
            if (node->block.stmts.count == 1) {
                emit_eval_expr(e, node->block.stmts.items[0], self_name, param_count, pnames, ret_type);
            } else if (node->block.stmts.count > 0) {
                emit_eval_expr(e, node->block.stmts.items[node->block.stmts.count - 1],
                               self_name, param_count, pnames, ret_type);
            } else {
                fprintf(e->out, "0");
            }
            return;
        case NODE_RETURN:
            emit_eval_expr(e, node->ret.value, self_name, param_count, pnames, ret_type);
            return;
        default:
            fprintf(e->out, "0");
            return;
    }
}

/* Emit an evaluator-mode condition (uses _argN instead of _aN_v) */
static void emit_eval_cond_expr(CEmitter *e, ASTNode *node, int param_count, const char **pnames) {
    if (!node) { fprintf(e->out, "0"); return; }
    if (node->kind == NODE_CHAIN) { emit_eval_cond_expr(e, chain_to_calls(e->arena, node), param_count, pnames); return; }
    switch (node->kind) {
        case NODE_INT_LIT:
            fprintf(e->out, "%lldLL", (long long)node->int_lit.int_val);
            return;
        case NODE_FLOAT_LIT:
            fprintf(e->out, "%g", node->float_lit.float_val);
            return;
        case NODE_BOOL_LIT:
            fprintf(e->out, "%s", node->bool_lit.bool_val ? "1" : "0");
            return;
        case NODE_IDENT:
            for (int i = 0; i < param_count; i++) {
                if (strcmp(node->ident.ident, pnames[i]) == 0) {
                    fprintf(e->out, "_arg%d", i);
                    return;
                }
            }
            fprintf(e->out, "0");
            return;
        case NODE_CALL: {
            const BuiltinOp *bi = find_builtin(node->call.call_name);
            if (bi && bi->c_op && node->call.args.count == 2) {
                fprintf(e->out, "(");
                emit_eval_cond_expr(e, node->call.args.items[0], param_count, pnames);
                fprintf(e->out, " %s ", bi->c_op);
                emit_eval_cond_expr(e, node->call.args.items[1], param_count, pnames);
                fprintf(e->out, ")");
                return;
            }
            if (bi && bi->is_unary_prefix && node->call.args.count == 1) {
                const char *op = strcmp(bi->name, "not") == 0 ? "!" : "-";
                fprintf(e->out, "(%s", op);
                emit_eval_cond_expr(e, node->call.args.items[0], param_count, pnames);
                fprintf(e->out, ")");
                return;
            }
            fprintf(e->out, "1");
            return;
        }
        default:
            fprintf(e->out, "0");
            return;
    }
}

static void emit_eval_cond(CEmitter *e, ASTNode *cond, int param_count, const char **pnames) {
    if (!cond) { fprintf(e->out, "1"); return; }
    emit_eval_cond_expr(e, cond, param_count, pnames);
}

/* Walk if/else for evaluator */
static void emit_eval_if(CEmitter *e, ASTNode *if_node, const char *self_name,
                           int param_count, const char **pnames, TypeRef *ret_type);

/* Emit evaluator body walking the AST */
static void emit_eval_body_walk(CEmitter *e, ASTNode *body, const char *self_name,
                                  int param_count, const char **pnames, TypeRef *ret_type) {
    if (!body) return;
    switch (body->kind) {
        case NODE_IF:
            emit_eval_if(e, body, self_name, param_count, pnames, ret_type);
            break;
        case NODE_RETURN:
            eval_child_idx = 0;
            fprintf(e->out, "    return ");
            /* Emit boxed return expression */
            if (ret_type) {
                switch (ret_type->kind) {
                    case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
                        fprintf(e->out, "sigil_val_int("); break;
                    case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                        fprintf(e->out, "sigil_val_float("); break;
                    case TYPE_BOOL:
                        fprintf(e->out, "sigil_val_bool("); break;
                    case TYPE_CHAR:
                        fprintf(e->out, "sigil_val_char("); break;
                    case TYPE_MAP: case TYPE_NAMED:
                        fprintf(e->out, "sigil_val_map("); break;
                    default:
                        fprintf(e->out, "sigil_val_int("); break;
                }
            } else {
                fprintf(e->out, "sigil_val_int(");
            }
            emit_eval_expr(e, body->ret.value, self_name, param_count, pnames, ret_type);
            fprintf(e->out, ");\n");
            break;
        case NODE_BLOCK: case NODE_BEGIN_END:
            for (int i = 0; i < body->block.stmts.count; i++)
                emit_eval_body_walk(e, body->block.stmts.items[i], self_name, param_count, pnames, ret_type);
            break;
        default:
            break;
    }
}

/* Emit evaluator if/else branching */
static void emit_eval_if(CEmitter *e, ASTNode *if_node, const char *self_name,
                           int param_count, const char **pnames, TypeRef *ret_type) {
    fprintf(e->out, "    if (");
    emit_eval_cond(e, if_node->if_stmt.condition, param_count, pnames);
    fprintf(e->out, ") {\n");
    emit_eval_body_walk(e, if_node->if_stmt.then_body, self_name, param_count, pnames, ret_type);
    fprintf(e->out, "    }");
    for (int i = 0; i < if_node->if_stmt.elifs.count; i += 2) {
        fprintf(e->out, " else if (");
        emit_eval_cond(e, if_node->if_stmt.elifs.items[i], param_count, pnames);
        fprintf(e->out, ") {\n");
        emit_eval_body_walk(e, if_node->if_stmt.elifs.items[i + 1], self_name, param_count, pnames, ret_type);
        fprintf(e->out, "    }");
    }
    if (if_node->if_stmt.else_body) {
        fprintf(e->out, " else {\n");
        emit_eval_body_walk(e, if_node->if_stmt.else_body, self_name, param_count, pnames, ret_type);
        fprintf(e->out, "    }");
    }
    fprintf(e->out, "\n");
}

/* Emit _eval_N function: forces child thunks, walks graph for recursive fns */
static void emit_thunk_evaluator(CEmitter *e, int func_id, ASTNode *fn) {
    /* Collect param info */
    int pc = 0;
    const char *pnames[32];
    TypeRef *ptypes[32];
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM && pc < 32) {
            pnames[pc] = pe->param_name;
            ptypes[pc] = pe->type;
            pc++;
        }
    }

    /* Detect if this fn is recursive */
    int recursive = count_recursive_calls(e, fn->fn_decl.body, fn->fn_decl.fn_name);

    fprintf(e->out, "static SigilVal _eval_%d(SigilThunk *_t, ThunkArena *_arena) {\n", func_id);
    /* Force each arg thunk (sigil_force_val handles nested thunks) */
    for (int i = 0; i < pc; i++) {
        fprintf(e->out, "    SigilVal _arg%d_v = sigil_force_val(thunk_force(_t->args[%d], _arena), _arena);\n", i, i);
        fprintf(e->out, "    ");
        emit_c_type(e, ptypes[i]);
        fprintf(e->out, " _arg%d = ", i);
        /* Unbox based on type */
        if (ptypes[i]) {
            switch (ptypes[i]->kind) {
                case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
                    fprintf(e->out, "sigil_unbox_int(_arg%d_v)", i); break;
                case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                    fprintf(e->out, "sigil_unbox_float(_arg%d_v)", i); break;
                case TYPE_BOOL:
                    fprintf(e->out, "sigil_unbox_bool(_arg%d_v)", i); break;
                case TYPE_CHAR:
                    fprintf(e->out, "sigil_unbox_char(_arg%d_v)", i); break;
                case TYPE_MAP: case TYPE_NAMED:
                    fprintf(e->out, "sigil_unbox_map(_arg%d_v)", i); break;
                case TYPE_FN:
                    fprintf(e->out, "sigil_unbox_closure(_arg%d_v)", i); break;
                default:
                    fprintf(e->out, "sigil_unbox_int(_arg%d_v)", i); break;
            }
        } else {
            fprintf(e->out, "sigil_unbox_int(_arg%d_v)", i);
        }
        fprintf(e->out, ";\n");
    }

    if (recursive > 0 && fn->fn_decl.body) {
        /* Graph-driven evaluator: walk AST, force children instead of calling C fn */
        fprintf(e->out, "    (void)_arg0_v;\n");
        eval_child_idx = 0;
        emit_eval_body_walk(e, fn->fn_decl.body, fn->fn_decl.fn_name, pc, pnames, fn->fn_decl.return_type);
        fprintf(e->out, "    return sigil_val_int(0);\n");
    } else {
        /* Non-recursive: call the C function directly */
        fprintf(e->out, "    ");
        TypeRef *ret = fn->fn_decl.return_type;
        bool is_void = ret && ret->kind == TYPE_VOID;
        if (!is_void) {
            emit_c_type(e, ret);
            fprintf(e->out, " _result = ");
        }
        int mpc = 0;
        TypeRef *mptypes[32];
        for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
            if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM && mpc < 32)
                mptypes[mpc++] = fn->fn_decl.pattern.items[i].type;
        }
        emit_mangled_fn_name(e, fn->fn_decl.fn_name, mpc, mptypes);
        fprintf(e->out, "(");
        for (int i = 0; i < pc; i++) {
            if (i > 0) fprintf(e->out, ", ");
            fprintf(e->out, "_arg%d", i);
        }
        fprintf(e->out, ");\n");

        if (is_void) {
            fprintf(e->out, "    return sigil_val_int(0);\n");
        } else {
            fprintf(e->out, "    return ");
            if (ret) {
                switch (ret->kind) {
                    case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
                        fprintf(e->out, "sigil_val_int(_result)"); break;
                    case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                        fprintf(e->out, "sigil_val_float(_result)"); break;
                    case TYPE_BOOL:
                        fprintf(e->out, "sigil_val_bool(_result)"); break;
                    case TYPE_CHAR:
                        fprintf(e->out, "sigil_val_char(_result)"); break;
                    case TYPE_MAP: case TYPE_NAMED:
                        fprintf(e->out, "sigil_val_map(_result)"); break;
                    case TYPE_FN:
                        fprintf(e->out, "sigil_val_closure(_result)"); break;
                    default:
                        fprintf(e->out, "sigil_val_int(_result)"); break;
                }
            } else {
                fprintf(e->out, "sigil_val_int(_result)");
            }
            fprintf(e->out, ";\n");
        }
    }
    fprintf(e->out, "}\n\n");
}

/* ── Constructor helpers ──────────────────────────────────────────── */

/* Counter for recursive child assignment in constructors */
static int ctor_child_idx;

/* Count recursive calls to self_name in an expression tree */
static int count_recursive_calls(CEmitter *e, ASTNode *node, const char *fn_name) {
    (void)e;
    if (!node) return 0;
    if (node->kind == NODE_CHAIN) return count_recursive_calls(e, chain_to_calls(e->arena, node), fn_name);
    if (node->kind == NODE_CALL) {
        int count = (strcmp(node->call.call_name, fn_name) == 0) ? 1 : 0;
        for (int i = 0; i < node->call.args.count; i++)
            count += count_recursive_calls(e, node->call.args.items[i], fn_name);
        return count;
    }
    switch (node->kind) {
        case NODE_BEGIN_END: case NODE_BLOCK:
            { int c = 0; for (int i = 0; i < node->block.stmts.count; i++)
                c += count_recursive_calls(e, node->block.stmts.items[i], fn_name);
              return c; }
        case NODE_RETURN:
            return count_recursive_calls(e, node->ret.value, fn_name);
        case NODE_IF:
            { int c = count_recursive_calls(e, node->if_stmt.then_body, fn_name);
              if (node->if_stmt.else_body) c += count_recursive_calls(e, node->if_stmt.else_body, fn_name);
              return c; }
        default: return 0;
    }
}

/* Emit a completed thunk wrapping a forced arg value for constructor contexts */
static void emit_ctor_arg_val(CEmitter *e, ASTNode *node, int param_count, const char **pnames) {
    if (node && node->kind == NODE_CHAIN) { emit_ctor_arg_val(e, chain_to_calls(e->arena, node), param_count, pnames); return; }
    if (!node) {
        fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int(0))");
        return;
    }
    switch (node->kind) {
        case NODE_INT_LIT:
            fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int(%lldLL))",
                    (long long)node->int_lit.int_val);
            return;
        case NODE_FLOAT_LIT:
            fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_float(%g))",
                    node->float_lit.float_val);
            return;
        case NODE_BOOL_LIT:
            fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_bool(%s))",
                    node->bool_lit.bool_val ? "true" : "false");
            return;
        case NODE_IDENT:
            for (int i = 0; i < param_count; i++) {
                if (strcmp(node->ident.ident, pnames[i]) == 0) {
                    fprintf(e->out, "thunk_alloc_completed(_arena, _a%d_v)", i);
                    return;
                }
            }
            fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int(0))");
            return;
        case NODE_CALL: {
            /* Evaluate builtins inline on forced args */
            const BuiltinOp *bi = find_builtin(node->call.call_name);
            if (bi && bi->c_op && node->call.args.count == 2) {
                fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int(sigil_unbox_int(");
                emit_ctor_expr_raw(e, node->call.args.items[0], param_count, pnames);
                fprintf(e->out, ") %s sigil_unbox_int(", bi->c_op);
                emit_ctor_expr_raw(e, node->call.args.items[1], param_count, pnames);
                fprintf(e->out, ")))");
                return;
            }
            if (bi && bi->is_unary_prefix && node->call.args.count == 1) {
                const char *op = strcmp(bi->name, "negate") == 0 ? "-" : "!";
                fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int((int64_t)(%s sigil_unbox_int(", op);
                emit_ctor_expr_raw(e, node->call.args.items[0], param_count, pnames);
                fprintf(e->out, "))))");
                return;
            }
            fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int(0))");
            return;
        }
        case NODE_BEGIN_END: case NODE_BLOCK:
            if (node->block.stmts.count > 0)
                emit_ctor_arg_val(e, node->block.stmts.items[node->block.stmts.count - 1], param_count, pnames);
            else
                fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int(0))");
            return;
        default:
            fprintf(e->out, "thunk_alloc_completed(_arena, sigil_val_int(0))");
            return;
    }
}

/* Emit raw SigilVal expression (for inline builtin evaluation in constructors) */
static void emit_ctor_expr_raw(CEmitter *e, ASTNode *node, int param_count, const char **pnames) {
    if (node && node->kind == NODE_CHAIN) { emit_ctor_expr_raw(e, chain_to_calls(e->arena, node), param_count, pnames); return; }
    (void)e;
    if (!node) { fprintf(e->out, "sigil_val_int(0)"); return; }
    switch (node->kind) {
        case NODE_INT_LIT:
            fprintf(e->out, "sigil_val_int(%lldLL)", (long long)node->int_lit.int_val);
            return;
        case NODE_IDENT:
            for (int i = 0; i < param_count; i++) {
                if (strcmp(node->ident.ident, pnames[i]) == 0) {
                    fprintf(e->out, "_a%d_v", i);
                    return;
                }
            }
            fprintf(e->out, "sigil_val_int(0)");
            return;
        case NODE_CALL: {
            const BuiltinOp *bi = find_builtin(node->call.call_name);
            if (bi && bi->c_op && node->call.args.count == 2) {
                fprintf(e->out, "sigil_val_int(sigil_unbox_int(");
                emit_ctor_expr_raw(e, node->call.args.items[0], param_count, pnames);
                fprintf(e->out, ") %s sigil_unbox_int(", bi->c_op);
                emit_ctor_expr_raw(e, node->call.args.items[1], param_count, pnames);
                fprintf(e->out, "))");
                return;
            }
            if (bi && bi->is_unary_prefix && node->call.args.count == 1) {
                const char *op = strcmp(bi->name, "negate") == 0 ? "-" : "!";
                fprintf(e->out, "sigil_val_int((int64_t)(%s sigil_unbox_int(", op);
                emit_ctor_expr_raw(e, node->call.args.items[0], param_count, pnames);
                fprintf(e->out, ")))");
                return;
            }
            fprintf(e->out, "sigil_val_int(0)");
            return;
        }
        case NODE_BEGIN_END: case NODE_BLOCK:
            if (node->block.stmts.count > 0)
                emit_ctor_expr_raw(e, node->block.stmts.items[node->block.stmts.count - 1], param_count, pnames);
            else
                fprintf(e->out, "sigil_val_int(0)");
            return;
        default:
            fprintf(e->out, "sigil_val_int(0)");
            return;
    }
}

/* Emit condition for constructor guards */
static void emit_ctor_cond(CEmitter *e, ASTNode *cond, int param_count, const char **pnames) {
    if (!cond) { fprintf(e->out, "1"); return; }
    if (cond->kind == NODE_CHAIN) { emit_ctor_cond(e, chain_to_calls(e->arena, cond), param_count, pnames); return; }
    if (cond->kind == NODE_BOOL_LIT) {
        fprintf(e->out, "%s", cond->bool_lit.bool_val ? "1" : "0");
        return;
    }
    if (cond->kind == NODE_CALL) {
        const BuiltinOp *bi = find_builtin(cond->call.call_name);
        if (bi && bi->c_op && cond->call.args.count == 2) {
            fprintf(e->out, "(sigil_unbox_int(");
            emit_ctor_expr_raw(e, cond->call.args.items[0], param_count, pnames);
            fprintf(e->out, ") %s sigil_unbox_int(", bi->c_op);
            emit_ctor_expr_raw(e, cond->call.args.items[1], param_count, pnames);
            fprintf(e->out, "))");
            return;
        }
        if (bi && bi->is_unary_prefix && cond->call.args.count == 1) {
            const char *op = strcmp(bi->name, "not") == 0 ? "!" : "-";
            fprintf(e->out, "(%s sigil_unbox_int(", op);
            emit_ctor_expr_raw(e, cond->call.args.items[0], param_count, pnames);
            fprintf(e->out, "))");
            return;
        }
    }
    fprintf(e->out, "1");
}

/* Find the return expression in a body (unwrapping blocks) */
static ASTNode *find_return_expr(ASTNode *body) {
    if (!body) return NULL;
    if (body->kind == NODE_RETURN) return body->ret.value;
    if (body->kind == NODE_BLOCK || body->kind == NODE_BEGIN_END) {
        for (int i = 0; i < body->block.stmts.count; i++) {
            ASTNode *r = find_return_expr(body->block.stmts.items[i]);
            if (r) return r;
        }
    }
    return NULL;
}

/* Emit constructor body for a branch: find recursive calls and emit child thunks */
static void emit_ctor_branch(CEmitter *e, ASTNode *body, const char *self_name, int self_id,
                              int param_count, const char **pnames) {
    if (!body) { fprintf(e->out, "    return NULL;\n"); return; }

    /* Count recursive calls in this branch */
    int rc = count_recursive_calls(e, body, self_name);

    if (rc == 0) {
        /* Base case: no recursive calls. Try to compute the return value inline. */
        ASTNode *ret_expr = find_return_expr(body);
        if (ret_expr) {
            fprintf(e->out, "    return thunk_alloc_completed(_arena, ");
            emit_ctor_expr_raw(e, ret_expr, param_count, pnames);
            fprintf(e->out, ");\n");
        } else {
            fprintf(e->out, "    return NULL;\n");
        }
    } else {
        /* Recursive case: collect recursive sub-call thunks as children */
        fprintf(e->out, "    {\n");
        fprintf(e->out, "    SigilThunk *_result = thunk_alloc(_arena, %d, %d);\n", self_id, rc);
        ctor_child_idx = 0;
        collect_recursive_thunks_to_result(e, body, self_name, self_id, param_count, pnames);
        fprintf(e->out, "    return _result;\n");
        fprintf(e->out, "    }\n");
    }
}

/* Walk body and emit _result->args[idx] assignments for each recursive call found */
static void collect_recursive_thunks_to_result(CEmitter *e, ASTNode *node, const char *self_name,
                                                int self_id, int param_count, const char **pnames) {
    if (!node) return;
    switch (node->kind) {
        case NODE_RETURN:
            emit_recursive_child_assignments(e, node->ret.value, self_name, self_id, param_count, pnames);
            break;
        case NODE_BLOCK: case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                collect_recursive_thunks_to_result(e, node->block.stmts.items[i], self_name, self_id, param_count, pnames);
            break;
        default:
            emit_recursive_child_assignments(e, node, self_name, self_id, param_count, pnames);
            break;
    }
}


static void emit_recursive_child_assignments(CEmitter *e, ASTNode *node, const char *self_name,
                                              int self_id, int param_count, const char **pnames) {
    if (!node) return;
    if (node->kind == NODE_CHAIN) { emit_recursive_child_assignments(e, chain_to_calls(e->arena, node), self_name, self_id, param_count, pnames); return; }
    if (node->kind == NODE_CALL && strcmp(node->call.call_name, self_name) == 0) {
        int ca = node->call.args.count;
        fprintf(e->out, "    _result->args[%d] = ({ SigilThunk *_ct = thunk_alloc(_arena, %d, %d);",
                ctor_child_idx, self_id, ca);
        for (int i = 0; i < ca; i++) {
            fprintf(e->out, " _ct->args[%d] = ", i);
            emit_ctor_arg_val(e, node->call.args.items[i], param_count, pnames);
            fprintf(e->out, ";");
        }
        fprintf(e->out, " _ct; });\n");
        ctor_child_idx++;
        return;
    }
    if (node->kind == NODE_CALL) {
        for (int i = 0; i < node->call.args.count; i++)
            emit_recursive_child_assignments(e, node->call.args.items[i], self_name, self_id, param_count, pnames);
        return;
    }
    if (node->kind == NODE_BEGIN_END || node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmts.count; i++)
            emit_recursive_child_assignments(e, node->block.stmts.items[i], self_name, self_id, param_count, pnames);
    }
}

/* Walk if/else to emit constructor branching logic */
static void emit_ctor_if(CEmitter *e, ASTNode *if_node, const char *self_name, int self_id,
                          int param_count, const char **pnames) {
    fprintf(e->out, "    if (");
    emit_ctor_cond(e, if_node->if_stmt.condition, param_count, pnames);
    fprintf(e->out, ") {\n");
    emit_ctor_branch(e, if_node->if_stmt.then_body, self_name, self_id, param_count, pnames);
    fprintf(e->out, "    }");
    for (int i = 0; i < if_node->if_stmt.elifs.count; i += 2) {
        fprintf(e->out, " else if (");
        emit_ctor_cond(e, if_node->if_stmt.elifs.items[i], param_count, pnames);
        fprintf(e->out, ") {\n");
        emit_ctor_branch(e, if_node->if_stmt.elifs.items[i + 1], self_name, self_id, param_count, pnames);
        fprintf(e->out, "    }");
    }
    if (if_node->if_stmt.else_body) {
        fprintf(e->out, " else {\n");
        emit_ctor_branch(e, if_node->if_stmt.else_body, self_name, self_id, param_count, pnames);
        fprintf(e->out, "    }");
    }
    fprintf(e->out, "\n");
}

/* Walk body to find if/else or return at top level */
static void emit_ctor_body_walk(CEmitter *e, ASTNode *body, const char *self_name, int self_id,
                                 int param_count, const char **pnames) {
    if (!body) return;
    switch (body->kind) {
        case NODE_IF:
            emit_ctor_if(e, body, self_name, self_id, param_count, pnames);
            break;
        case NODE_RETURN:
            emit_ctor_branch(e, body, self_name, self_id, param_count, pnames);
            break;
        case NODE_BLOCK: case NODE_BEGIN_END:
            for (int i = 0; i < body->block.stmts.count; i++)
                emit_ctor_body_walk(e, body->block.stmts.items[i], self_name, self_id, param_count, pnames);
            break;
        default:
            break;
    }
}

/* Emit _ctor_N function: real constructor that analyzes fn body */
static void emit_thunk_constructor(CEmitter *e, int func_id, ASTNode *fn) {
    int pc = 0;
    const char *pnames[32];
    TypeRef *ptypes[32];
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM && pc < 32) {
            pnames[pc] = pe->param_name;
            ptypes[pc] = pe->type;
            pc++;
        }
    }
    (void)ptypes;

    fprintf(e->out, "static SigilThunk* _ctor_%d(ThunkArena *_arena, SigilThunk **_args) {\n", func_id);

    ASTNode *body = fn->fn_decl.body;
    int recursive = count_recursive_calls(e, body, fn->fn_decl.fn_name);

    if (recursive == 0 || !body) {
        /* Non-recursive fn: return NULL to signal evaluator-only */
        fprintf(e->out, "    (void)_arena; (void)_args;\n");
        fprintf(e->out, "    return NULL;\n");
        fprintf(e->out, "}\n\n");
        return;
    }

    /* Force each arg to get concrete values for guard evaluation */
    if (pc > 0) {
        fprintf(e->out, "    if (!_args) return NULL;\n");
    }
    for (int i = 0; i < pc; i++) {
        fprintf(e->out, "    SigilVal _a%d_v = _args[%d] ? sigil_force_val(thunk_force(_args[%d], _arena), _arena) : sigil_val_int(0);\n",
                i, i, i);
    }

    /* Reset child index counter and walk body */
    ctor_child_idx = 0;
    emit_ctor_body_walk(e, body, fn->fn_decl.fn_name, func_id, pc, pnames);

    fprintf(e->out, "    return NULL;\n");
    fprintf(e->out, "}\n\n");
}

/* Emit mono evaluator */
static void emit_thunk_mono_evaluator(CEmitter *e, int func_id, MonoInstance *m) {
    ASTNode *fn = m->fn_node;
    int pc = 0;
    const char *pnames[32];
    TypeRef *ptypes[32];

    MonoInstance *prev = e->current_mono;
    e->current_mono = m;

    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        PatElem *pe = &fn->fn_decl.pattern.items[i];
        if (pe->kind == PAT_PARAM && pc < 32) {
            pnames[pc] = pe->param_name;
            ptypes[pc] = pe->type;
            pc++;
        }
    }

    fprintf(e->out, "static SigilVal _eval_%d(SigilThunk *_t, ThunkArena *_arena) {\n", func_id);
    for (int i = 0; i < pc; i++) {
        fprintf(e->out, "    SigilVal _arg%d_v = sigil_force_val(thunk_force(_t->args[%d], _arena), _arena);\n", i, i);
        fprintf(e->out, "    ");
        emit_c_type(e, ptypes[i]);
        fprintf(e->out, " _arg%d = ", i);
        TypeRef *sub = mono_substitute(e, ptypes[i]);
        TypeRef *actual = sub ? sub : ptypes[i];
        if (actual) {
            switch (actual->kind) {
                case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
                    fprintf(e->out, "sigil_unbox_int(_arg%d_v)", i); break;
                case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                    fprintf(e->out, "sigil_unbox_float(_arg%d_v)", i); break;
                case TYPE_BOOL:
                    fprintf(e->out, "sigil_unbox_bool(_arg%d_v)", i); break;
                case TYPE_CHAR:
                    fprintf(e->out, "sigil_unbox_char(_arg%d_v)", i); break;
                case TYPE_MAP: case TYPE_NAMED:
                    fprintf(e->out, "sigil_unbox_map(_arg%d_v)", i); break;
                case TYPE_FN:
                    fprintf(e->out, "sigil_unbox_closure(_arg%d_v)", i); break;
                default:
                    fprintf(e->out, "sigil_unbox_int(_arg%d_v)", i); break;
            }
        } else {
            fprintf(e->out, "sigil_unbox_int(_arg%d_v)", i);
        }
        fprintf(e->out, ";\n");
    }

    /* Check if recursive */
    int recursive = count_recursive_calls(e, fn->fn_decl.body, fn->fn_decl.fn_name);
    TypeRef *ret = fn->fn_decl.return_type;
    TypeRef *ret_sub = mono_substitute(e, ret);
    if (ret_sub) ret = ret_sub;

    if (recursive > 0 && fn->fn_decl.body) {
        /* Graph-driven evaluator for recursive mono fn */
        fprintf(e->out, "    (void)_arg0_v;\n");
        eval_child_idx = 0;
        emit_eval_body_walk(e, fn->fn_decl.body, fn->fn_decl.fn_name, pc, pnames, ret);
        fprintf(e->out, "    return sigil_val_int(0);\n");
    } else {
        /* Non-recursive: call the mono-mangled fn */
        fprintf(e->out, "    ");
        bool is_void = ret && ret->kind == TYPE_VOID;
        if (!is_void) {
            emit_c_type(e, ret);
            fprintf(e->out, " _result = ");
        }
        fprintf(e->out, "sigil_%s", m->fn_name);
        for (int i = 0; i < m->type_var_count; i++)
            fprintf(e->out, "_%s", type_suffix(m->concrete_types[i]));
        fprintf(e->out, "(");
        for (int i = 0; i < pc; i++) {
            if (i > 0) fprintf(e->out, ", ");
            fprintf(e->out, "_arg%d", i);
        }
        fprintf(e->out, ");\n");

        if (is_void) {
            fprintf(e->out, "    return sigil_val_int(0);\n");
        } else {
            fprintf(e->out, "    return ");
            if (ret) {
                switch (ret->kind) {
                    case TYPE_INT: case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
                        fprintf(e->out, "sigil_val_int(_result)"); break;
                    case TYPE_FLOAT: case TYPE_FLOAT32: case TYPE_FLOAT64:
                        fprintf(e->out, "sigil_val_float(_result)"); break;
                    case TYPE_BOOL:
                        fprintf(e->out, "sigil_val_bool(_result)"); break;
                    case TYPE_CHAR:
                        fprintf(e->out, "sigil_val_char(_result)"); break;
                    case TYPE_MAP: case TYPE_NAMED:
                        fprintf(e->out, "sigil_val_map(_result)"); break;
                    case TYPE_FN:
                        fprintf(e->out, "sigil_val_closure(_result)"); break;
                    default:
                        fprintf(e->out, "sigil_val_int(_result)"); break;
                }
            } else {
                fprintf(e->out, "sigil_val_int(_result)");
            }
            fprintf(e->out, ";\n");
        }
    }
    fprintf(e->out, "}\n\n");

    e->current_mono = prev;
}

void c_emit(CEmitter *e, ASTNode *node) {
    /* Header */
    fprintf(e->out, "#include <stdint.h>\n");
    fprintf(e->out, "#include <stdbool.h>\n");
    fprintf(e->out, "#include <stdio.h>\n");
    fprintf(e->out, "#include \"sigil_runtime.h\"\n");
    fprintf(e->out, "#include \"sigil_thunk.h\"\n");
    fprintf(e->out, "#include \"sigil_executor.h\"\n");
    fprintf(e->out, "#include \"sigil_hardware.h\"\n\n");
    fprintf(e->out, "static ThunkArena _arena;\n");
    fprintf(e->out, "static HardwareProfile _hw;\n\n");

    /* Collect all function declarations */
    FnList fns = {NULL, 0, 0};
    collect_fns(node, &fns);

    /* Collect monomorphization instances */
    collect_mono_from_node(e, node, &fns);

    /* Transitive mono fixpoint */
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (MonoInstance *m = e->mono_instances; m; m = m->next) {
                int count_before = 0;
                for (MonoInstance *c = e->mono_instances; c; c = c->next) count_before++;
                collect_transitive_mono(e, m->fn_node->fn_decl.body, &fns, m);
                int count_after = 0;
                for (MonoInstance *c = e->mono_instances; c; c = c->next) count_after++;
                if (count_after > count_before) changed = true;
            }
        }
    }

    /* Assign func_ids to all user-defined fns (skip var-param fns) */
    for (int i = 0; i < fns.count; i++) {
        ASTNode *fn = fns.items[i];
        if (fn->fn_decl.is_primitive) continue;
        if (!fn->fn_decl.body) continue;
        if (fn_is_generic(fn)) continue;
        /* Skip var-param (mutable ref) fns — can't thunkify mutation */
        bool has_var = false;
        for (int j = 0; j < fn->fn_decl.pattern.count; j++) {
            if (fn->fn_decl.pattern.items[j].kind == PAT_PARAM &&
                fn->fn_decl.pattern.items[j].is_mutable) { has_var = true; break; }
        }
        if (has_var) continue;
        int pc = 0;
        TypeRef *ptypes[32];
        for (int j = 0; j < fn->fn_decl.pattern.count; j++) {
            if (fn->fn_decl.pattern.items[j].kind == PAT_PARAM && pc < 32)
                ptypes[pc++] = fn->fn_decl.pattern.items[j].type;
        }
        TypeRef **pt = (TypeRef **)arena_alloc(e->arena, pc * sizeof(TypeRef *));
        for (int j = 0; j < pc; j++) pt[j] = ptypes[j];
        register_thunk_fn(e, fn->fn_decl.fn_name, pc, pt, false, 0, NULL);
    }
    /* Assign func_ids to mono instances */
    for (MonoInstance *m = e->mono_instances; m; m = m->next) {
        ASTNode *fn = m->fn_node;
        int pc = 0;
        for (int j = 0; j < fn->fn_decl.pattern.count; j++) {
            if (fn->fn_decl.pattern.items[j].kind == PAT_PARAM) pc++;
        }
        TypeRef **conc = (TypeRef **)arena_alloc(e->arena, m->type_var_count * sizeof(TypeRef *));
        for (int j = 0; j < m->type_var_count; j++) conc[j] = m->concrete_types[j];
        register_thunk_fn(e, m->fn_name, pc, NULL, true, m->type_var_count, conc);
    }

    /* Pre-collect and emit lambda functions */
    precollect_lambdas(e, node);
    for (LambdaEntry *le = e->lambdas; le; le = le->next)
        emit_lambda_function(e, le);

    /* Pass 1: prototypes for original fns */
    int proto_count = 0;
    for (int i = 0; i < fns.count; i++) {
        ASTNode *fn = fns.items[i];
        if (fn->fn_decl.is_primitive) continue;
        if (!fn->fn_decl.body && find_builtin(fn->fn_decl.fn_name)) continue;
        if (!fn->fn_decl.body) continue;
        if (fn_is_generic(fn)) continue;
        emit_fn_prototype(e, fn);
        fprintf(e->out, ";\n");
        proto_count++;
    }
    for (MonoInstance *m = e->mono_instances; m; m = m->next) {
        emit_mono_prototype(e, m);
        fprintf(e->out, ";\n");
        proto_count++;
    }
    if (proto_count > 0) fprintf(e->out, "\n");

    /* Forward-declare evaluators and constructors */
    for (int id = 0; id < e->thunk_fn_counter; id++) {
        fprintf(e->out, "static SigilVal _eval_%d(SigilThunk *_t, ThunkArena *_arena);\n", id);
        fprintf(e->out, "static SigilThunk* _ctor_%d(ThunkArena *_arena, SigilThunk **_args);\n", id);
    }
    if (e->thunk_fn_counter > 0) fprintf(e->out, "\n");

    /* Dispatch tables */
    if (e->thunk_fn_counter > 0) {
        fprintf(e->out, "static ThunkConstructor _ctor_table[] = {");
        for (int id = 0; id < e->thunk_fn_counter; id++) {
            if (id > 0) fprintf(e->out, ",");
            fprintf(e->out, " _ctor_%d", id);
        }
        fprintf(e->out, " };\n");
        fprintf(e->out, "static ThunkEvaluator _eval_table[] = {");
        for (int id = 0; id < e->thunk_fn_counter; id++) {
            if (id > 0) fprintf(e->out, ",");
            fprintf(e->out, " _eval_%d", id);
        }
        fprintf(e->out, " };\n\n");
    }

    /* Pass 2: original fn bodies */
    for (int i = 0; i < fns.count; i++) {
        if (fn_is_generic(fns.items[i])) continue;
        emit_fn_decl(e, fns.items[i]);
    }
    for (MonoInstance *m = e->mono_instances; m; m = m->next) {
        emit_mono_body(e, m);
    }

    /* Pass 3: evaluators and constructors */
    {
        int id = 0;
        for (int i = 0; i < fns.count; i++) {
            ASTNode *fn = fns.items[i];
            if (fn->fn_decl.is_primitive) continue;
            if (!fn->fn_decl.body) continue;
            if (fn_is_generic(fn)) continue;
            /* Skip var-param fns */
            bool has_var = false;
            for (int j = 0; j < fn->fn_decl.pattern.count; j++) {
                if (fn->fn_decl.pattern.items[j].kind == PAT_PARAM &&
                    fn->fn_decl.pattern.items[j].is_mutable) { has_var = true; break; }
            }
            if (has_var) continue;

            emit_thunk_evaluator(e, id, fn);
            emit_thunk_constructor(e, id, fn);
            id++;
        }
        for (MonoInstance *m = e->mono_instances; m; m = m->next) {
            emit_thunk_mono_evaluator(e, id, m);
            emit_thunk_constructor(e, id, m->fn_node);
            id++;
        }
    }

    /* Collect top-level executable statements */
    FnList top_stmts = {NULL, 0, 0};
    collect_top_stmts(node, &top_stmts);

    /* main() with thunk infrastructure */
    if (top_stmts.count > 0) {
        fprintf(e->out, "int main(void) {\n");
        e->indent = 1;
        emit_indent(e);
        fprintf(e->out, "thunk_arena_init(&_arena, 1024 * 1024);\n");
        emit_indent(e);
        fprintf(e->out, "calibrate_hardware(&_hw);\n");
        if (e->thunk_fn_counter > 0) {
            emit_indent(e);
            fprintf(e->out, "sigil_constructors = _ctor_table;\n");
            emit_indent(e);
            fprintf(e->out, "sigil_evaluators = _eval_table;\n");
            emit_indent(e);
            fprintf(e->out, "sigil_thunk_fn_count = %d;\n", e->thunk_fn_counter);
        }
        for (int i = 0; i < top_stmts.count; i++)
            emit_stmt(e, top_stmts.items[i]);
        emit_indent(e);
        fprintf(e->out, "thunk_arena_destroy(&_arena);\n");
        fprintf(e->out, "    return 0;\n");
        fprintf(e->out, "}\n");
        e->indent = 0;
    }

    free(fns.items);
    free(top_stmts.items);
}
