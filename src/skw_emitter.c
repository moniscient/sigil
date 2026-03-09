#include "skw_emitter.h"
#include <stdlib.h>
#include <string.h>

void skw_emitter_init(SkwEmitter *e, FILE *out) {
    e->out = out;
    e->indent = 0;
}

static void emit_indent(SkwEmitter *e) {
    for (int i = 0; i < e->indent; i++)
        fprintf(e->out, "  ");
}

static void emit_type(SkwEmitter *e, TypeRef *t) {
    if (!t) { fprintf(e->out, "unknown"); return; }
    switch (t->kind) {
        case TYPE_BOOL:    fprintf(e->out, "bool"); break;
        case TYPE_INT:     fprintf(e->out, "int"); break;
        case TYPE_FLOAT:   fprintf(e->out, "float"); break;
        case TYPE_CHAR:    fprintf(e->out, "char"); break;
        case TYPE_MAP:     fprintf(e->out, "map"); break;
        case TYPE_VOID:    fprintf(e->out, "void"); break;
        case TYPE_INT8:    fprintf(e->out, "int8"); break;
        case TYPE_INT16:   fprintf(e->out, "int16"); break;
        case TYPE_INT32:   fprintf(e->out, "int32"); break;
        case TYPE_INT64:   fprintf(e->out, "int64"); break;
        case TYPE_FLOAT32: fprintf(e->out, "float32"); break;
        case TYPE_FLOAT64: fprintf(e->out, "float64"); break;
        case TYPE_ITER:    fprintf(e->out, "iter"); break;
        case TYPE_NAMED:   fprintf(e->out, "%s", t->name); break;
        case TYPE_GENERIC: fprintf(e->out, "%s", t->name); break;
        case TYPE_TRAIT_BOUND: fprintf(e->out, "%s %s", t->trait_name, t->name); break;
        case TYPE_FN:      fprintf(e->out, "fn"); break;
        case TYPE_UNKNOWN: fprintf(e->out, "unknown"); break;
    }
}

/* Emit a block body (NODE_BLOCK or NODE_BEGIN_END) as multi-line indented statements */
static void emit_body(SkwEmitter *e, ASTNode *node) {
    if (!node) return;
    if (node->kind == NODE_BLOCK || node->kind == NODE_BEGIN_END) {
        for (int i = 0; i < node->block.stmts.count; i++) {
            emit_indent(e);
            skw_emit(e, node->block.stmts.items[i]);
            fprintf(e->out, "\n");
        }
    } else {
        emit_indent(e);
        skw_emit(e, node);
        fprintf(e->out, "\n");
    }
}

void skw_emit(SkwEmitter *e, ASTNode *node) {
    if (!node) return;

    switch (node->kind) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->program.top_level.count; i++) {
                skw_emit(e, node->program.top_level.items[i]);
                fprintf(e->out, "\n");
            }
            break;

        case NODE_IMPORT:
            emit_indent(e);
            fprintf(e->out, "import \"%s\"\n", node->import_decl.import_path);
            break;

        case NODE_FN_DECL:
            emit_indent(e);
            fprintf(e->out, "fn %s", node->fn_decl.fn_name);
            /* Emit pattern (keyword-prefix: just types and names, no sigils) */
            for (int i = 0; i < node->fn_decl.pattern.count; i++) {
                PatElem *pe = &node->fn_decl.pattern.items[i];
                if (pe->kind == PAT_PARAM) {
                    fprintf(e->out, " ");
                    if (pe->is_mutable) fprintf(e->out, "var ");
                    if (pe->type) { emit_type(e, pe->type); fprintf(e->out, " "); }
                    if (pe->param_name) fprintf(e->out, "%s", pe->param_name);
                }
            }
            if (node->fn_decl.return_type) {
                fprintf(e->out, " returns ");
                emit_type(e, node->fn_decl.return_type);
            }
            fprintf(e->out, "\n");
            if (node->fn_decl.body) {
                e->indent++;
                emit_body(e, node->fn_decl.body);
                e->indent--;
            }
            break;

        case NODE_CALL:
            fprintf(e->out, "%s", node->call.call_name);
            for (int i = 0; i < node->call.args.count; i++) {
                fprintf(e->out, " ");
                ASTNode *arg = node->call.args.items[i];
                if (arg->kind == NODE_CALL) {
                    fprintf(e->out, "begin ");
                    skw_emit(e, arg);
                    fprintf(e->out, " end");
                } else if (arg->kind == NODE_BEGIN_END) {
                    skw_emit(e, arg);
                } else {
                    skw_emit(e, arg);
                }
            }
            break;

        case NODE_IDENT:
            fprintf(e->out, "%s", node->ident.ident);
            break;

        case NODE_INT_LIT:
            fprintf(e->out, "%lld", (long long)node->int_lit.int_val);
            break;

        case NODE_FLOAT_LIT:
            fprintf(e->out, "%g", node->float_lit.float_val);
            break;

        case NODE_BOOL_LIT:
            fprintf(e->out, "%s", node->bool_lit.bool_val ? "true" : "false");
            break;

        case NODE_STRING_LIT:
            fprintf(e->out, "\"");
            for (const char *s = node->string_lit.str_val; *s; s++) {
                switch (*s) {
                    case '"':  fprintf(e->out, "\\\""); break;
                    case '\\': fprintf(e->out, "\\\\"); break;
                    case '\n': fprintf(e->out, "\\n"); break;
                    case '\t': fprintf(e->out, "\\t"); break;
                    case '\r': fprintf(e->out, "\\r"); break;
                    default:   fputc(*s, e->out); break;
                }
            }
            fprintf(e->out, "\"");
            break;

        case NODE_LET:
            fprintf(e->out, "let %s ", node->binding.bind_name);
            skw_emit(e, node->binding.value);
            break;

        case NODE_VAR:
            fprintf(e->out, "var %s ", node->binding.bind_name);
            skw_emit(e, node->binding.value);
            break;

        case NODE_ASSIGN:
            fprintf(e->out, "assign %s ", node->assign.assign_name);
            skw_emit(e, node->assign.value);
            break;

        case NODE_RETURN:
            fprintf(e->out, "return ");
            skw_emit(e, node->ret.value);
            break;

        case NODE_IF:
            fprintf(e->out, "if ");
            skw_emit(e, node->if_stmt.condition);
            fprintf(e->out, " begin\n");
            e->indent++;
            emit_body(e, node->if_stmt.then_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end");
            for (int i = 0; i < node->if_stmt.elifs.count; i += 2) {
                fprintf(e->out, " elif ");
                skw_emit(e, node->if_stmt.elifs.items[i]);
                fprintf(e->out, " begin\n");
                e->indent++;
                emit_body(e, node->if_stmt.elifs.items[i + 1]);
                e->indent--;
                emit_indent(e);
                fprintf(e->out, "end");
            }
            if (node->if_stmt.else_body) {
                fprintf(e->out, " else begin\n");
                e->indent++;
                emit_body(e, node->if_stmt.else_body);
                e->indent--;
                emit_indent(e);
                fprintf(e->out, "end");
            }
            break;

        case NODE_WHILE:
            fprintf(e->out, "while ");
            skw_emit(e, node->while_stmt.condition);
            fprintf(e->out, " begin\n");
            e->indent++;
            emit_body(e, node->while_stmt.while_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end");
            break;

        case NODE_FOR:
            fprintf(e->out, "for %s in ", node->for_stmt.var_name);
            skw_emit(e, node->for_stmt.iterable);
            fprintf(e->out, " begin\n");
            e->indent++;
            emit_body(e, node->for_stmt.for_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end");
            break;

        case NODE_MATCH:
            fprintf(e->out, "match ");
            skw_emit(e, node->match_stmt.match_value);
            fprintf(e->out, " begin\n");
            e->indent++;
            for (int i = 0; i < node->match_stmt.cases.count; i++)
                skw_emit(e, node->match_stmt.cases.items[i]);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end");
            break;

        case NODE_CASE:
            emit_indent(e);
            fprintf(e->out, "case ");
            skw_emit(e, node->case_branch.case_pattern);
            fprintf(e->out, " begin\n");
            e->indent++;
            emit_body(e, node->case_branch.case_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end\n");
            break;

        case NODE_DEFAULT:
            emit_indent(e);
            fprintf(e->out, "default begin\n");
            e->indent++;
            emit_body(e, node->default_branch.default_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end\n");
            break;

        case NODE_BLOCK:
            for (int i = 0; i < node->block.stmts.count; i++) {
                emit_indent(e);
                skw_emit(e, node->block.stmts.items[i]);
                fprintf(e->out, "\n");
            }
            break;

        case NODE_BEGIN_END:
            fprintf(e->out, "begin ");
            for (int i = 0; i < node->block.stmts.count; i++) {
                if (i > 0) fprintf(e->out, " ");
                skw_emit(e, node->block.stmts.items[i]);
            }
            fprintf(e->out, " end");
            break;

        case NODE_TRAIT_DECL:
            emit_indent(e);
            fprintf(e->out, "trait %s %s", node->trait_decl.trait_name, node->trait_decl.type_var);
            for (int i = 0; i < node->trait_decl.requires.count; i++)
                fprintf(e->out, " requires %s", node->trait_decl.requires.items[i]);
            fprintf(e->out, " begin\n");
            e->indent++;
            for (int i = 0; i < node->trait_decl.methods.count; i++) {
                skw_emit(e, node->trait_decl.methods.items[i]);
                fprintf(e->out, "\n");
            }
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end");
            break;

        case NODE_IMPLEMENT:
            emit_indent(e);
            fprintf(e->out, "implement %s for %s begin\n", node->implement.trait_name,
                    node->implement.concrete_type);
            e->indent++;
            for (int i = 0; i < node->implement.methods.count; i++) {
                skw_emit(e, node->implement.methods.items[i]);
                fprintf(e->out, "\n");
            }
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end");
            break;

        case NODE_ALGEBRA:
        case NODE_LIBRARY:
            fprintf(e->out, "%s %s\n\n",
                    node->kind == NODE_LIBRARY ? "library" : "algebra",
                    node->algebra.algebra_name);
            e->indent++;
            for (int i = 0; i < node->algebra.declarations.count; i++) {
                skw_emit(e, node->algebra.declarations.items[i]);
                fprintf(e->out, "\n");
            }
            e->indent--;
            break;

        case NODE_USE:
            emit_indent(e);
            fprintf(e->out, "use %s\n", node->use_block.algebra_name);
            e->indent++;
            emit_body(e, node->use_block.body);
            e->indent--;
            break;

        case NODE_PRECEDENCE:
            /* Omitted in .skw output - precedence is only for sigil layer */
            break;

        case NODE_SIGIL_EXPR:
            fprintf(e->out, "<%s>", node->sigil_expr.sigil);
            break;

        case NODE_TYPE_DECL:
            emit_indent(e);
            fprintf(e->out, "type %s", node->type_decl.type_name);
            for (int i = 0; i < node->type_decl.field_count; i++) {
                fprintf(e->out, " ");
                emit_type(e, node->type_decl.field_types[i]);
                fprintf(e->out, " %s", node->type_decl.field_names[i]);
            }
            break;

        case NODE_LAMBDA:
            fprintf(e->out, "lambda");
            for (int i = 0; i < node->lambda.lambda_param_count; i++) {
                fprintf(e->out, " ");
                emit_type(e, node->lambda.lambda_param_types[i]);
                fprintf(e->out, " %s", node->lambda.lambda_param_names[i]);
            }
            if (node->lambda.lambda_return_type) {
                fprintf(e->out, " returns ");
                emit_type(e, node->lambda.lambda_return_type);
            }
            fprintf(e->out, " begin\n");
            e->indent++;
            emit_body(e, node->lambda.lambda_body);
            e->indent--;
            emit_indent(e);
            fprintf(e->out, "end");
            break;

        case NODE_COMPREHENSION:
            fprintf(e->out, "collect from %s in ", node->comprehension.comp_var);
            skw_emit(e, node->comprehension.comp_source);
            if (node->comprehension.comp_filter) {
                fprintf(e->out, " where ");
                skw_emit(e, node->comprehension.comp_filter);
            }
            fprintf(e->out, " apply ");
            skw_emit(e, node->comprehension.comp_transform);
            break;

        case NODE_PARAM:
        case NODE_TYPE_REF:
            break;
    }
}

char *skw_emit_to_string(ASTNode *node) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f) return NULL;
    SkwEmitter e;
    skw_emitter_init(&e, f);
    skw_emit(&e, node);
    fclose(f);
    return buf;
}
