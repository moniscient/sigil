#include "c_emitter.h"
#include <string.h>
#include <ctype.h>

void c_emitter_init(CEmitter *e, FILE *out, TypeChecker *tc, Arena *arena) {
    e->out = out;
    e->indent = 0;
    e->tc = tc;
    e->arena = arena;
    e->var_params = NULL;
    e->var_param_count = 0;
    e->in_implement = false;
    e->implement_type = NULL;
    e->mono_instances = NULL;
    e->current_mono = NULL;
}

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
           strcmp(name, "length") == 0;
}

/* ── UDT helpers ─────────────────────────────────────────────────── */

static TypeDef *find_type_def(CEmitter *e, const char *name) {
    return type_def_lookup(e->tc, name);
}

/* ── Forward declarations ────────────────────────────────────────── */

static void emit_expr(CEmitter *e, ASTNode *node);
static void emit_stmt(CEmitter *e, ASTNode *node);
static void emit_body(CEmitter *e, ASTNode *node);
static TypeRef *infer_expr_type(CEmitter *e, ASTNode *node);
static bool fn_entry_is_generic(FnEntry *fn);
static void extract_type_vars(ASTNode *fn, const char **names, int *count);

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
    if (node->kind == NODE_BEGIN_END && node->block.stmts.count > 0) {
        /* Type of begin/end is the type of the last statement */
        return infer_expr_type(e, node->block.stmts.items[node->block.stmts.count - 1]);
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
        default:
            fprintf(e->out, "sigil_unbox_int("); break;
    }
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

            /* Map operations — special-cased before builtins */
            if (is_map_op(node->call.call_name)) {
                const char *op = node->call.call_name;
                if (strcmp(op, "mapnew") == 0) {
                    fprintf(e->out, "sigil_map_new()");
                } else if (strcmp(op, "set") == 0 && node->call.args.count == 3) {
                    fprintf(e->out, "sigil_map_set(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[1]);
                    fprintf(e->out, ", ");
                    emit_boxed(e, node->call.args.items[2]);
                    fprintf(e->out, ")");
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
                    fprintf(e->out, "sigil_map_count(");
                    emit_expr(e, node->call.args.items[0]);
                    fprintf(e->out, ")");
                }
                break;
            }
            const BuiltinOp *bi = find_builtin(node->call.call_name);
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
                                    print_fn = "sigil_print_int";
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
                /* Check if any param is var (mutable ref) -> pass &arg */
                FnEntry *fn = NULL;
                /* When inside a mono body, prefer the overload that matches concrete arg types */
                if (e->current_mono && fn_name_is_overloaded(e, node->call.call_name)) {
                    /* Infer concrete arg types */
                    TypeRef *arg_types[16] = {0};
                    int argc = node->call.args.count < 16 ? node->call.args.count : 16;
                    for (int ai = 0; ai < argc; ai++)
                        arg_types[ai] = infer_expr_type(e, node->call.args.items[ai]);
                    /* Find best matching FnEntry */
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
                }

                if (fn && fn_entry_is_generic(fn)) {
                    /* Generic fn call: mangle with concrete arg types */
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
                    emit_mangled_fn_name(e, node->call.call_name, fn->param_count, fn->param_types);
                } else {
                    emit_fn_name(e, node->call.call_name);
                }
                fprintf(e->out, "(");
                for (int i = 0; i < node->call.args.count; i++) {
                    if (i > 0) fprintf(e->out, ", ");
                    if (fn && i < fn->param_count && fn->param_mutable[i]) {
                        fprintf(e->out, "&");
                    }
                    emit_expr(e, node->call.args.items[i]);
                }
                fprintf(e->out, ")");
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
                /* Don't use const for pointer types (map, UDT) — causes warnings */
                if (t && (t->kind == TYPE_MAP || t->kind == TYPE_NAMED))
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
            emit_indent(e);
            emit_expr(e, node);
            fprintf(e->out, ";\n");
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

    emit_body(e, fn->fn_decl.body);

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

    emit_body(e, fn->fn_decl.body);

    e->var_params = prev_var_params;
    e->var_param_count = prev_vpc;
    e->indent--;
    fprintf(e->out, "}\n\n");

    e->current_mono = prev;
}

/* ── Collect top-level statements (non-fn) ───────────────────────── */

static bool is_executable_stmt(ASTNode *node) {
    if (!node) return false;
    switch (node->kind) {
        case NODE_LET: case NODE_VAR: case NODE_ASSIGN: case NODE_RETURN:
        case NODE_IF: case NODE_WHILE: case NODE_FOR: case NODE_MATCH:
        case NODE_CALL: case NODE_IDENT: case NODE_INT_LIT:
        case NODE_FLOAT_LIT: case NODE_BOOL_LIT: case NODE_STRING_LIT:
        case NODE_BEGIN_END: case NODE_BLOCK:
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

void c_emit(CEmitter *e, ASTNode *node) {
    /* Header */
    fprintf(e->out, "#include <stdint.h>\n");
    fprintf(e->out, "#include <stdbool.h>\n");
    fprintf(e->out, "#include <stdio.h>\n");
    fprintf(e->out, "#include \"sigil_runtime.h\"\n\n");

    /* Collect all function declarations */
    FnList fns = {NULL, 0, 0};
    collect_fns(node, &fns);

    /* Collect monomorphization instances */
    collect_mono_from_node(e, node, &fns);

    /* Transitive: walk each mono instance's body to find inner generic calls */
    /* Iterate until no new instances are added (fixpoint) */
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

    /* Pass 1: prototypes */
    int proto_count = 0;
    for (int i = 0; i < fns.count; i++) {
        ASTNode *fn = fns.items[i];
        if (fn->fn_decl.is_primitive) continue;
        if (!fn->fn_decl.body && find_builtin(fn->fn_decl.fn_name)) continue;
        if (!fn->fn_decl.body) continue;
        if (fn_is_generic(fn)) continue; /* skip generic fns — mono instances emitted instead */
        emit_fn_prototype(e, fn);
        fprintf(e->out, ";\n");
        proto_count++;
    }
    /* Mono instance prototypes */
    for (MonoInstance *m = e->mono_instances; m; m = m->next) {
        emit_mono_prototype(e, m);
        fprintf(e->out, ";\n");
        proto_count++;
    }
    if (proto_count > 0) fprintf(e->out, "\n");

    /* Pass 2: bodies */
    for (int i = 0; i < fns.count; i++) {
        if (fn_is_generic(fns.items[i])) continue; /* skip generic fns */
        emit_fn_decl(e, fns.items[i]);
    }
    /* Mono instance bodies */
    for (MonoInstance *m = e->mono_instances; m; m = m->next) {
        emit_mono_body(e, m);
    }

    /* Collect top-level executable statements */
    FnList top_stmts = {NULL, 0, 0};
    collect_top_stmts(node, &top_stmts);

    /* main() wrapper for top-level code */
    if (top_stmts.count > 0) {
        fprintf(e->out, "int main(void) {\n");
        e->indent = 1;
        for (int i = 0; i < top_stmts.count; i++)
            emit_stmt(e, top_stmts.items[i]);
        fprintf(e->out, "    return 0;\n");
        fprintf(e->out, "}\n");
        e->indent = 0;
    }

    free(fns.items);
    free(top_stmts.items);
}
