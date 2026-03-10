#include "parser.h"
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

/* ── ImportSet ───────────────────────────────────────────────────── */

void import_set_init(ImportSet *is) {
    is->paths = NULL;
    is->count = is->capacity = 0;
}

bool import_set_contains(ImportSet *is, const char *path) {
    for (int i = 0; i < is->count; i++)
        if (strcmp(is->paths[i], path) == 0) return true;
    return false;
}

void import_set_add(ImportSet *is, const char *path) {
    if (import_set_contains(is, path)) return;
    if (is->count >= is->capacity) {
        is->capacity = is->capacity ? is->capacity * 2 : 16;
        is->paths = realloc(is->paths, is->capacity * sizeof(const char *));
    }
    is->paths[is->count++] = path;
}

/* ── File reading (for import) ───────────────────────────────────── */

static char *read_file_for_import(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* ── Parser init ─────────────────────────────────────────────────── */

void parser_init(Parser *p, TokenList tokens, Arena *arena,
                 InternTable *intern_tab, ErrorList *errors) {
    p->tokens = tokens;
    p->pos = 0;
    p->arena = arena;
    p->intern_tab = intern_tab;
    p->errors = errors;
    p->in_sigil_mode = false;
    p->file_path = NULL;
    p->imports = NULL;
    p->compounds = NULL;
}

/* ── Token Access ────────────────────────────────────────────────── */

static Token *cur(Parser *p) {
    return &p->tokens.items[p->pos];
}

static Token *peek(Parser *p, int offset) {
    int idx = p->pos + offset;
    if (idx >= p->tokens.count) return &p->tokens.items[p->tokens.count - 1];
    return &p->tokens.items[idx];
}

static bool at_eof(Parser *p) {
    return cur(p)->kind == TOK_EOF;
}

static bool at(Parser *p, TokenKind kind) {
    return cur(p)->kind == kind;
}

static Token *eat(Parser *p) {
    Token *t = cur(p);
    if (!at_eof(p)) p->pos++;
    return t;
}

/* Skip over newline tokens — used everywhere except parse_keyword_call */
static void skip_newlines(Parser *p) {
    while (cur(p)->kind == TOK_NEWLINE) p->pos++;
}

static bool at_text(Parser *p, const char *text) {
    skip_newlines(p);
    return strcmp(cur(p)->text, text) == 0;
}

static Token *expect(Parser *p, TokenKind kind, const char *what) {
    skip_newlines(p);
    if (cur(p)->kind != kind) {
        error_add(p->errors, ERR_PARSER, cur(p)->loc, "expected %s, got '%s'", what, cur(p)->text);
        return cur(p);
    }
    return eat(p);
}

static Token *expect_text(Parser *p, const char *text) {
    skip_newlines(p);
    if (strcmp(cur(p)->text, text) != 0) {
        error_add(p->errors, ERR_PARSER, cur(p)->loc, "expected '%s', got '%s'", text, cur(p)->text);
        return cur(p);
    }
    return eat(p);
}

/* ── Type Parsing ────────────────────────────────────────────────── */

static TypeRef *parse_type(Parser *p) {
    TypeRef *t = (TypeRef *)arena_alloc(p->arena, sizeof(TypeRef));
    memset(t, 0, sizeof(TypeRef));
    const char *name = cur(p)->text;

    if (strcmp(name, "bool") == 0)   { t->kind = TYPE_BOOL; eat(p); }
    else if (strcmp(name, "int") == 0)    { t->kind = TYPE_INT; eat(p); }
    else if (strcmp(name, "float") == 0)  { t->kind = TYPE_FLOAT; eat(p); }
    else if (strcmp(name, "char") == 0)   { t->kind = TYPE_CHAR; eat(p); }
    else if (strcmp(name, "map") == 0)    {
        t->kind = TYPE_MAP;
        eat(p);
        /* Optionally parse key and value types: map int float */
        if (!at_eof(p) && cur(p)->kind == TOK_KEYWORD && is_type_keyword(cur(p)->text) &&
            strcmp(cur(p)->text, "true") != 0 && strcmp(cur(p)->text, "false") != 0) {
            t->key_type = parse_type(p);
            if (!at_eof(p) && cur(p)->kind == TOK_KEYWORD && is_type_keyword(cur(p)->text) &&
                strcmp(cur(p)->text, "true") != 0 && strcmp(cur(p)->text, "false") != 0) {
                t->val_type = parse_type(p);
            }
        }
    }
    else if (strcmp(name, "void") == 0)   { t->kind = TYPE_VOID; eat(p); }
    else if (strcmp(name, "int8") == 0)   { t->kind = TYPE_INT8; eat(p); }
    else if (strcmp(name, "int16") == 0)  { t->kind = TYPE_INT16; eat(p); }
    else if (strcmp(name, "int32") == 0)  { t->kind = TYPE_INT32; eat(p); }
    else if (strcmp(name, "int64") == 0)  { t->kind = TYPE_INT64; eat(p); }
    else if (strcmp(name, "float32") == 0){ t->kind = TYPE_FLOAT32; eat(p); }
    else if (strcmp(name, "float64") == 0){ t->kind = TYPE_FLOAT64; eat(p); }
    else if (strcmp(name, "iter") == 0)   { t->kind = TYPE_ITER; eat(p); }
    else {
        /* Could be a trait-bounded generic: TraitName TypeVar */
        /* Or just a named type */
        const char *first = cur(p)->text;
        eat(p);
        /* Check if next token is a single uppercase letter (type var) */
        if (!at_eof(p) && cur(p)->kind == TOK_IDENT && strlen(cur(p)->text) == 1 &&
            cur(p)->text[0] >= 'A' && cur(p)->text[0] <= 'Z') {
            t->kind = TYPE_TRAIT_BOUND;
            t->trait_name = first;
            t->name = cur(p)->text;
            eat(p);
        } else {
            /* Single uppercase letter alone = generic type var */
            if (strlen(first) == 1 && first[0] >= 'A' && first[0] <= 'Z') {
                t->kind = TYPE_GENERIC;
                t->name = first;
            } else {
                t->kind = TYPE_NAMED;
                t->name = first;
            }
        }
    }
    return t;
}

/* ── Forward Declarations ────────────────────────────────────────── */

static ASTNode *parse_begin_end(Parser *p);
static ASTNode *parse_do_end(Parser *p);
static ASTNode *parse_keyword_call(Parser *p, const char *keyword, SrcLoc loc);
static ASTNode *parse_lambda(Parser *p);
static ASTNode *parse_collect(Parser *p);

/* ── Expression Parsing ──────────────────────────────────────────── */

static ASTNode *parse_atom(Parser *p) {
    skip_newlines(p);
    Token *t = cur(p);

    if (t->kind == TOK_INT_LIT) {
        ASTNode *n = ast_new(p->arena, NODE_INT_LIT, t->loc);
        n->int_lit.int_val = t->int_val;
        eat(p);
        /* Optional type suffix: 42 int32 */
        if (!at_eof(p) && !at(p, TOK_NEWLINE) && cur(p)->kind == TOK_KEYWORD &&
            is_type_keyword(cur(p)->text) &&
            strcmp(cur(p)->text, "true") != 0 && strcmp(cur(p)->text, "false") != 0) {
            n->resolved_type = parse_type(p);
        }
        return n;
    }
    if (t->kind == TOK_FLOAT_LIT) {
        ASTNode *n = ast_new(p->arena, NODE_FLOAT_LIT, t->loc);
        n->float_lit.float_val = t->float_val;
        eat(p);
        /* Optional type suffix: 3.14 float32 */
        if (!at_eof(p) && !at(p, TOK_NEWLINE) && cur(p)->kind == TOK_KEYWORD &&
            is_type_keyword(cur(p)->text) &&
            strcmp(cur(p)->text, "true") != 0 && strcmp(cur(p)->text, "false") != 0) {
            n->resolved_type = parse_type(p);
        }
        return n;
    }
    if (t->kind == TOK_STRING_LIT) {
        ASTNode *n = ast_new(p->arena, NODE_STRING_LIT, t->loc);
        n->string_lit.str_val = t->text;
        n->string_lit.str_len = (int)strlen(t->text);
        eat(p);
        return n;
    }
    if (t->kind == TOK_KEYWORD && (strcmp(t->text, "true") == 0 || strcmp(t->text, "false") == 0)) {
        ASTNode *n = ast_new(p->arena, NODE_BOOL_LIT, t->loc);
        n->bool_lit.bool_val = strcmp(t->text, "true") == 0;
        eat(p);
        return n;
    }
    if (t->kind == TOK_KEYWORD && strcmp(t->text, "lambda") == 0) {
        return parse_lambda(p);
    }
    if (t->kind == TOK_KEYWORD && strcmp(t->text, "collect") == 0) {
        return parse_collect(p);
    }
    if (t->kind == TOK_DO) {
        return parse_do_end(p);
    }
    if (t->kind == TOK_IDENT) {
        ASTNode *n = ast_new(p->arena, NODE_IDENT, t->loc);
        n->ident.ident = t->text;
        eat(p);
        return n;
    }
    if (t->kind == TOK_SIGIL) {
        /* Sigil as expression (nullary or prefix in sigil mode) */
        ASTNode *n = ast_new(p->arena, NODE_SIGIL_EXPR, t->loc);
        n->sigil_expr.sigil = t->text;
        da_init(&n->sigil_expr.operands);
        n->sigil_expr.expr_fixity = FIXITY_NULLARY;
        eat(p);
        return n;
    }

    error_add(p->errors, ERR_PARSER, t->loc, "unexpected token '%s'", t->text);
    eat(p);
    return ast_new(p->arena, NODE_IDENT, t->loc);
}

ASTNode *parse_expression(Parser *p) {
    return parse_atom(p);
}

/* ── Begin/End Block (body delimiter) ────────────────────────────── */

static ASTNode *parse_begin_end(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect(p, TOK_BEGIN, "begin");
    ASTNode *n = ast_new(p->arena, NODE_BLOCK, loc);
    da_init(&n->block.stmts);

    while (!at_eof(p) && !at(p, TOK_END)) {
        skip_newlines(p);
        if (at(p, TOK_END)) break;
        ASTNode *stmt = parse_statement(p);
        if (stmt) da_push(&n->block.stmts, stmt);
    }
    expect(p, TOK_END, "end");
    return n;
}

/* ── Do/End Expression Group ─────────────────────────────────────── */

static ASTNode *parse_do_end(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect(p, TOK_DO, "do");
    ASTNode *n = ast_new(p->arena, NODE_BEGIN_END, loc);
    da_init(&n->block.stmts);

    while (!at_eof(p) && !at(p, TOK_END)) {
        skip_newlines(p);
        if (at(p, TOK_END)) break;
        ASTNode *stmt = parse_statement(p);
        if (stmt) da_push(&n->block.stmts, stmt);
    }
    expect(p, TOK_END, "end");
    return n;
}

/* ── Keyword-Prefix Call Parsing ─────────────────────────────────── */

/* Parse a single argument in keyword-prefix context: either do/end group or atom */
static ASTNode *parse_kw_arg(Parser *p) {
    if (at(p, TOK_DO)) return parse_do_end(p);
    return parse_atom(p);
}

/* Parse a keyword-prefix call: keyword arg1 arg2 ... */
static ASTNode *parse_keyword_call(Parser *p, const char *keyword, SrcLoc loc) {
    ASTNode *n = ast_new(p->arena, NODE_CALL, loc);
    n->call.call_name = keyword;
    da_init(&n->call.args);

    /* Collect args until we hit: keyword, end, begin, EOF, newline, or another statement boundary */
    while (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_BEGIN) && !at(p, TOK_NEWLINE) &&
           !(cur(p)->kind == TOK_KEYWORD && (is_structural_keyword(cur(p)->text) ||
                                              is_primitive_keyword(cur(p)->text)))) {
        ASTNode *arg = parse_kw_arg(p);
        da_push(&n->call.args, arg);
    }
    return n;
}

/* ── fn Pattern Parsing ──────────────────────────────────────────── */

static void parse_fn_pattern(Parser *p, PatList *pattern) {
    da_init(pattern);
    /* Parse until 'returns' or 'begin' or EOF */
    while (!at_eof(p) && !at_text(p, "returns") && !at(p, TOK_BEGIN)) {
        if (at_text(p, "repeats")) {
            PatElem pe;
            pe.kind = PAT_REPEATS;
            pe.sigil = NULL;
            pe.type = NULL;
            pe.param_name = NULL;
            pe.is_mutable = false;
            da_push(pattern, pe);
            eat(p);
        } else if (at(p, TOK_SIGIL)) {
            PatElem pe;
            pe.kind = PAT_SIGIL;
            pe.sigil = cur(p)->text;
            pe.type = NULL;
            pe.param_name = NULL;
            pe.is_mutable = false;
            da_push(pattern, pe);
            eat(p);
        } else if (at_text(p, "var") || is_type_keyword(cur(p)->text) || cur(p)->kind == TOK_IDENT) {
            /* Check for 'var' annotation (mutable reference parameter) */
            bool mutable = false;
            if (at_text(p, "var")) {
                mutable = true;
                eat(p);
            }
            /* Could be: type name (parameter) OR just an ident that's part of a type */
            /* Try to parse as: type param_name */
            Token *first = cur(p);
            /* Check if this looks like a type followed by a param name */
            if (is_type_keyword(first->text) ||
                (first->kind == TOK_IDENT &&
                 peek(p, 1)->kind == TOK_IDENT &&
                 !is_keyword(peek(p, 1)->text))) {
                TypeRef *type = parse_type(p);
                if (!at_eof(p) && cur(p)->kind == TOK_IDENT && !is_keyword(cur(p)->text)) {
                    PatElem pe;
                    pe.kind = PAT_PARAM;
                    pe.sigil = NULL;
                    pe.type = type;
                    pe.param_name = cur(p)->text;
                    pe.is_mutable = mutable;
                    da_push(pattern, pe);
                    eat(p);
                } else {
                    /* Type without param name - treat the type name as a param reference */
                    PatElem pe;
                    pe.kind = PAT_PARAM;
                    pe.sigil = NULL;
                    pe.type = type;
                    pe.param_name = NULL;
                    pe.is_mutable = mutable;
                    da_push(pattern, pe);
                }
            } else {
                /* Just an identifier, probably a parameter name without type annotation */
                PatElem pe;
                pe.kind = PAT_PARAM;
                pe.sigil = NULL;
                pe.type = NULL;
                pe.param_name = cur(p)->text;
                pe.is_mutable = mutable;
                da_push(pattern, pe);
                eat(p);
            }
        } else {
            break;
        }
    }
}

/* Analyze a fn pattern to determine fixity and extract sigils */
Fixity analyze_fn_pattern(PatList *pattern, StrList *sigils_out) {
    da_init(sigils_out);

    /* Extract all sigils from the pattern */
    int sigil_count = 0;
    int param_count = 0;
    int first_sigil_idx = -1;
    int last_sigil_idx = -1;
    int first_param_idx = -1;
    int last_param_idx = -1;

    for (int i = 0; i < pattern->count; i++) {
        if (pattern->items[i].kind == PAT_SIGIL) {
            da_push(sigils_out, pattern->items[i].sigil);
            sigil_count++;
            if (first_sigil_idx < 0) first_sigil_idx = i;
            last_sigil_idx = i;
        } else if (pattern->items[i].kind == PAT_PARAM) {
            param_count++;
            if (first_param_idx < 0) first_param_idx = i;
            last_param_idx = i;
        }
    }

    if (sigil_count == 0) return FIXITY_NONE;
    if (param_count == 0) return FIXITY_NULLARY;

    /* Check for bracketed: sigils on both sides enclosing inner params.
     * e.g., mat m [ int i , int j ] — sigils [ , ] with params between them.
     * Key signal: at least 2 sigils, and some param is between two sigils. */
    if (sigil_count >= 2) {
        bool has_inner_param = false;
        for (int i = 0; i < pattern->count; i++) {
            if (pattern->items[i].kind == PAT_PARAM && i > first_sigil_idx && i < last_sigil_idx) {
                has_inner_param = true;
                break;
            }
        }
        if (has_inner_param) return FIXITY_BRACKETED;
    }

    /* Prefix: sigil(s) before all params */
    if (last_sigil_idx < first_param_idx) return FIXITY_PREFIX;

    /* Postfix: sigil(s) after all params */
    if (first_sigil_idx > last_param_idx) return FIXITY_POSTFIX;

    /* Infix: one sigil between params, with params on both sides */
    if (sigil_count == 1 && param_count >= 2 &&
        first_param_idx < first_sigil_idx && last_param_idx > first_sigil_idx) {
        return FIXITY_INFIX;
    }

    /* Multiple sigils with params — likely compound/bracketed */
    if (sigil_count >= 2) return FIXITY_BRACKETED;

    /* Fallback for single sigil between params */
    if (first_param_idx < first_sigil_idx) return FIXITY_INFIX;

    return FIXITY_PREFIX;
}

/* ── Declaration Parsing ─────────────────────────────────────────── */

static ASTNode *parse_fn_decl(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "fn");

    /* fn name — may be a primitive keyword (e.g., add, negate) or an identifier */
    const char *name = cur(p)->text;
    if (cur(p)->kind == TOK_IDENT || cur(p)->kind == TOK_KEYWORD)
        eat(p);
    else
        expect(p, TOK_IDENT, "function name");

    /* Pattern */
    ASTNode *n = ast_new(p->arena, NODE_FN_DECL, loc);
    n->fn_decl.fn_name = name;
    parse_fn_pattern(p, &n->fn_decl.pattern);

    /* Analyze pattern */
    n->fn_decl.fixity = analyze_fn_pattern(&n->fn_decl.pattern, &n->fn_decl.sigils);

    /* returns type — required on every fn */
    if (at_text(p, "returns")) {
        eat(p);
        n->fn_decl.return_type = parse_type(p);
    } else {
        error_add(p->errors, ERR_PARSER, cur(p)->loc,
                 "fn '%s' missing 'returns' clause — every fn must declare its return type",
                 name);
        n->fn_decl.return_type = NULL;
    }

    /* Body */
    n->fn_decl.body = NULL;
    n->fn_decl.is_primitive = false;
    skip_newlines(p);
    if (at(p, TOK_BEGIN)) {
        n->fn_decl.body = parse_begin_end(p);
    } else if (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
               !(cur(p)->kind == TOK_KEYWORD && is_structural_keyword(cur(p)->text))) {
        /* Single-expression body or <primitive> */
        if (at(p, TOK_SIGIL) && strcmp(cur(p)->text, "<") == 0) {
            /* skip <primitive> */
            while (!at_eof(p) && !(at(p, TOK_SIGIL) && strcmp(cur(p)->text, ">") == 0))
                eat(p);
            if (!at_eof(p)) eat(p); /* skip > */
            n->fn_decl.is_primitive = true;
        } else {
            /* Body could be a keyword-prefix call or an expression */
            if (cur(p)->kind == TOK_KEYWORD && is_primitive_keyword(cur(p)->text)) {
                const char *kw = cur(p)->text;
                SrcLoc kloc = cur(p)->loc;
                eat(p);
                n->fn_decl.body = parse_keyword_call(p, kw, kloc);
            } else if (cur(p)->kind == TOK_IDENT) {
                const char *nm = cur(p)->text;
                SrcLoc kloc = cur(p)->loc;
                eat(p);
                n->fn_decl.body = parse_keyword_call(p, nm, kloc);
            } else {
                n->fn_decl.body = parse_expression(p);
            }
        }
    }
    return n;
}

static ASTNode *parse_trait_decl(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "trait");

    ASTNode *n = ast_new(p->arena, NODE_TRAIT_DECL, loc);
    n->trait_decl.trait_name = cur(p)->text;
    expect(p, TOK_IDENT, "trait name");
    n->trait_decl.type_var = cur(p)->text;
    expect(p, TOK_IDENT, "type variable");

    da_init(&n->trait_decl.methods);
    da_init(&n->trait_decl.requires);

    /* Parse requires clauses */
    while (at_text(p, "requires")) {
        eat(p);
        const char *req_trait = cur(p)->text;
        eat(p);
        /* skip type var after requires */
        if (!at_eof(p) && cur(p)->kind == TOK_IDENT) eat(p);
        da_push(&n->trait_decl.requires, req_trait);
    }

    /* Parse method signatures: require begin/end block */
    if (at(p, TOK_BEGIN)) {
        eat(p); /* begin */
        while (!at_eof(p) && !at(p, TOK_END) && at_text(p, "fn")) {
            ASTNode *method = parse_fn_decl(p);
            da_push(&n->trait_decl.methods, method);
        }
        if (at(p, TOK_END)) eat(p); /* end */
    } else {
        /* Legacy: consume fns until non-fn (backward compat for single-method traits) */
        while (!at_eof(p) && at_text(p, "fn")) {
            ASTNode *method = parse_fn_decl(p);
            da_push(&n->trait_decl.methods, method);
        }
    }

    return n;
}

static ASTNode *parse_implement(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "implement");

    ASTNode *n = ast_new(p->arena, NODE_IMPLEMENT, loc);
    n->implement.trait_name = cur(p)->text;
    eat(p); /* trait name */
    /* Skip optional 'for' keyword */
    if (at_text(p, "for")) eat(p);
    n->implement.concrete_type = cur(p)->text;
    eat(p); /* concrete type */

    da_init(&n->implement.methods);

    /* Parse methods: require begin/end block */
    if (at(p, TOK_BEGIN)) {
        eat(p); /* begin */
        while (!at_eof(p) && !at(p, TOK_END) && at_text(p, "fn")) {
            ASTNode *method = parse_fn_decl(p);
            da_push(&n->implement.methods, method);
        }
        if (at(p, TOK_END)) eat(p); /* end */
    } else {
        /* Legacy: consume fns until non-fn */
        while (!at_eof(p) && at_text(p, "fn")) {
            ASTNode *method = parse_fn_decl(p);
            da_push(&n->implement.methods, method);
        }
    }
    return n;
}

static ASTNode *parse_algebra_or_library(Parser *p, bool is_library) {
    SrcLoc loc = cur(p)->loc;
    eat(p); /* algebra or library */

    ASTNode *n = ast_new(p->arena, is_library ? NODE_LIBRARY : NODE_ALGEBRA, loc);
    n->algebra.algebra_name = cur(p)->text;
    expect(p, TOK_IDENT, "algebra name");
    da_init(&n->algebra.declarations);

    /* Parse declarations until EOF or another algebra/library */
    while (!at_eof(p) &&
           !at_text(p, "algebra") && !at_text(p, "library")) {
        ASTNode *decl = parse_statement(p);
        if (decl) da_push(&n->algebra.declarations, decl);
    }
    return n;
}

static ASTNode *parse_use(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "use");

    ASTNode *n = ast_new(p->arena, NODE_USE, loc);
    n->use_block.algebra_name = cur(p)->text;
    expect(p, TOK_IDENT, "algebra name");

    /* The body is indented statements until we hit a top-level construct */
    ASTNode *body = ast_new(p->arena, NODE_BLOCK, loc);
    da_init(&body->block.stmts);

    /* Parse body: everything until next top-level keyword or EOF */
    while (!at_eof(p) &&
           !at_text(p, "algebra") && !at_text(p, "library") &&
           !at_text(p, "import") &&
           !(at_text(p, "use") && peek(p, 1)->kind == TOK_IDENT)) {
        ASTNode *stmt = parse_statement(p);
        if (stmt) da_push(&body->block.stmts, stmt);
    }
    n->use_block.body = body;
    return n;
}

/* ── Import ──────────────────────────────────────────────────────── */

static bool is_decl_node(NodeKind kind) {
    return kind == NODE_FN_DECL || kind == NODE_TRAIT_DECL ||
           kind == NODE_IMPLEMENT || kind == NODE_ALGEBRA ||
           kind == NODE_LIBRARY || kind == NODE_PRECEDENCE ||
           kind == NODE_TYPE_DECL || kind == NODE_IMPORT ||
           kind == NODE_ALIAS;
}

static ASTNode *parse_import(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "import");

    if (at_eof(p) || cur(p)->kind != TOK_STRING_LIT) {
        error_add(p->errors, ERR_PARSER, loc, "expected file path string after 'import'");
        return NULL;
    }

    const char *raw_path = cur(p)->text;
    eat(p);

    /* Resolve path relative to current file's directory */
    char resolved[4096];
    if (raw_path[0] == '/') {
        /* Absolute path */
        if (!realpath(raw_path, resolved)) {
            error_add(p->errors, ERR_PARSER, loc, "cannot resolve import path '%s'", raw_path);
            return NULL;
        }
    } else {
        /* Relative to current file's directory */
        char dir_buf[4096];
        if (p->file_path) {
            snprintf(dir_buf, sizeof(dir_buf), "%s", p->file_path);
            char *dir = dirname(dir_buf);
            char joined[4096];
            snprintf(joined, sizeof(joined), "%s/%s", dir, raw_path);
            if (!realpath(joined, resolved)) {
                error_add(p->errors, ERR_PARSER, loc, "cannot resolve import path '%s'", raw_path);
                return NULL;
            }
        } else {
            /* No file path context (e.g., string input) — try cwd */
            if (!realpath(raw_path, resolved)) {
                error_add(p->errors, ERR_PARSER, loc, "cannot resolve import path '%s'", raw_path);
                return NULL;
            }
        }
    }

    const char *interned_path = intern_cstr(p->intern_tab, resolved);

    /* Dedup: skip if already imported */
    if (p->imports && import_set_contains(p->imports, interned_path)) {
        ASTNode *n = ast_new(p->arena, NODE_IMPORT, loc);
        n->import_decl.import_path = interned_path;
        da_init(&n->import_decl.declarations);
        return n; /* empty — already imported */
    }

    if (p->imports)
        import_set_add(p->imports, interned_path);

    /* Read the file */
    char *source = read_file_for_import(resolved);
    if (!source) {
        error_add(p->errors, ERR_PARSER, loc, "cannot read import file '%s'", raw_path);
        return NULL;
    }

    /* Pre-scan for compound sigils if .sigil file */
    size_t plen = strlen(resolved);
    bool is_sigil_file = (plen > 6 && strcmp(resolved + plen - 6, ".sigil") == 0);
    if (is_sigil_file && p->compounds) {
        prescan_compound_sigils(source, interned_path, p->compounds, p->intern_tab);
    }

    /* Tokenize the imported file */
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, source, interned_path, p->intern_tab, p->errors, p->compounds);
    TokenList tokens = tokenize_all(&tokenizer);

    /* Parse the imported file */
    Parser child;
    parser_init(&child, tokens, p->arena, p->intern_tab, p->errors);
    child.file_path = interned_path;
    child.imports = p->imports;
    child.compounds = p->compounds;
    ASTNode *imported_prog = parse_program(&child);

    da_free(&tokens);
    free(source);

    /* Filter: keep only declaration nodes */
    ASTNode *n = ast_new(p->arena, NODE_IMPORT, loc);
    n->import_decl.import_path = interned_path;
    da_init(&n->import_decl.declarations);

    if (imported_prog && imported_prog->kind == NODE_PROGRAM) {
        for (int i = 0; i < imported_prog->program.top_level.count; i++) {
            ASTNode *child_node = imported_prog->program.top_level.items[i];
            if (child_node && is_decl_node(child_node->kind)) {
                da_push(&n->import_decl.declarations, child_node);
            }
        }
    }

    return n;
}

static ASTNode *parse_precedence(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "precedence");

    ASTNode *n = ast_new(p->arena, NODE_PRECEDENCE, loc);
    da_init(&n->precedence.sigils);

    /* Collect sigils until end of line / next keyword */
    while (!at_eof(p) && !at(p, TOK_NEWLINE) && (at(p, TOK_SIGIL) || at(p, TOK_IDENT))) {
        if (is_structural_keyword(cur(p)->text)) break;
        da_push(&n->precedence.sigils, cur(p)->text);
        eat(p);
    }
    return n;
}

static ASTNode *parse_alias(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "alias");

    ASTNode *n = ast_new(p->arena, NODE_ALIAS, loc);

    /* alias FROM TO — both can be sigils, keywords, or identifiers */
    if (at_eof(p) || at(p, TOK_NEWLINE)) {
        error_add(p->errors, ERR_PARSER, loc, "expected source token after 'alias'");
        return n;
    }
    n->alias.alias_from = cur(p)->text;
    eat(p);

    if (at_eof(p) || at(p, TOK_NEWLINE)) {
        error_add(p->errors, ERR_PARSER, loc, "expected target token after alias source");
        return n;
    }
    n->alias.alias_to = cur(p)->text;
    eat(p);

    return n;
}

static ASTNode *parse_type_decl(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "type");

    ASTNode *n = ast_new(p->arena, NODE_TYPE_DECL, loc);
    n->type_decl.type_name = cur(p)->text;
    if (cur(p)->kind == TOK_IDENT || cur(p)->kind == TOK_KEYWORD)
        eat(p);
    else
        expect(p, TOK_IDENT, "type name");

    /* Parse field pairs: type name, repeated */
    TypeRef *ftypes[64];
    const char *fnames[64];
    int fc = 0;

    while (!at_eof(p) && fc < 64) {
        skip_newlines(p);
        if (at_eof(p)) break;
        if (!(is_type_keyword(cur(p)->text) ||
              (cur(p)->kind == TOK_IDENT && peek(p, 1)->kind == TOK_IDENT))) break;
        ftypes[fc] = parse_type(p);
        fnames[fc] = cur(p)->text;
        if (cur(p)->kind == TOK_IDENT)
            eat(p);
        else
            expect(p, TOK_IDENT, "field name");
        fc++;
    }

    n->type_decl.field_count = fc;
    n->type_decl.field_types = (TypeRef **)arena_alloc(p->arena, fc * sizeof(TypeRef *));
    n->type_decl.field_names = (const char **)arena_alloc(p->arena, fc * sizeof(const char *));
    for (int i = 0; i < fc; i++) {
        n->type_decl.field_types[i] = ftypes[i];
        n->type_decl.field_names[i] = fnames[i];
    }
    return n;
}

/* ── Lambda Parsing ──────────────────────────────────────────────── */

/* lambda int x int y returns int begin ... end */
static ASTNode *parse_lambda(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "lambda");

    /* Parse parameter pairs: type name, until 'returns' */
    TypeRef *ptypes[32];
    const char *pnames[32];
    int pc = 0;

    while (!at_eof(p) && !at_text(p, "returns") && !at(p, TOK_BEGIN) && pc < 32) {
        ptypes[pc] = parse_type(p);
        pnames[pc] = cur(p)->text;
        if (cur(p)->kind == TOK_IDENT)
            eat(p);
        else
            expect(p, TOK_IDENT, "parameter name");
        pc++;
    }

    /* returns type */
    TypeRef *ret_type = NULL;
    if (at_text(p, "returns")) {
        eat(p);
        ret_type = parse_type(p);
    } else {
        ret_type = (TypeRef *)arena_alloc(p->arena, sizeof(TypeRef));
        memset(ret_type, 0, sizeof(TypeRef));
        ret_type->kind = TYPE_VOID;
    }

    /* Body: begin...end */
    ASTNode *body = parse_begin_end(p);

    ASTNode *n = ast_new(p->arena, NODE_LAMBDA, loc);
    n->lambda.lambda_param_count = pc;
    n->lambda.lambda_param_types = (TypeRef **)arena_alloc(p->arena, pc * sizeof(TypeRef *));
    n->lambda.lambda_param_names = (const char **)arena_alloc(p->arena, pc * sizeof(const char *));
    for (int i = 0; i < pc; i++) {
        n->lambda.lambda_param_types[i] = ptypes[i];
        n->lambda.lambda_param_names[i] = pnames[i];
    }
    n->lambda.lambda_return_type = ret_type;
    n->lambda.lambda_body = body;
    n->lambda.lambda_id = -1; /* assigned during emission */
    return n;
}

/* ── Comprehension Parsing ───────────────────────────────────────── */

/* collect from i in source [where cond] apply transform */
static ASTNode *parse_collect(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "collect");

    ASTNode *n = ast_new(p->arena, NODE_COMPREHENSION, loc);

    /* from var in source */
    expect_text(p, "from");
    n->comprehension.comp_var = cur(p)->text;
    expect(p, TOK_IDENT, "comprehension variable");
    expect_text(p, "in");

    /* Parse source expression — stops at 'where', 'apply', begin, newline */
    {
        Token *t = cur(p);
        if (t->kind == TOK_KEYWORD && is_primitive_keyword(t->text)) {
            const char *kw = t->text;
            SrcLoc kloc = t->loc;
            eat(p);
            /* Collect args stopping at where/apply */
            ASTNode *call = ast_new(p->arena, NODE_CALL, kloc);
            call->call.call_name = kw;
            da_init(&call->call.args);
            while (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
                   !at_text(p, "where") && !at_text(p, "apply") &&
                   !(cur(p)->kind == TOK_KEYWORD && is_structural_keyword(cur(p)->text) &&
                     strcmp(cur(p)->text, "where") != 0 && strcmp(cur(p)->text, "apply") != 0)) {
                ASTNode *arg = parse_kw_arg(p);
                da_push(&call->call.args, arg);
            }
            n->comprehension.comp_source = call;
        } else if (t->kind == TOK_IDENT) {
            /* Could be a variable name or a function call */
            const char *name = t->text;
            SrcLoc kloc = t->loc;
            eat(p);
            /* Check if followed by args before where/apply */
            if (!at_eof(p) && !at_text(p, "where") && !at_text(p, "apply") &&
                !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
                !(cur(p)->kind == TOK_KEYWORD && is_structural_keyword(cur(p)->text) &&
                  strcmp(cur(p)->text, "where") != 0 && strcmp(cur(p)->text, "apply") != 0)) {
                ASTNode *call = ast_new(p->arena, NODE_CALL, kloc);
                call->call.call_name = name;
                da_init(&call->call.args);
                while (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
                       !at_text(p, "where") && !at_text(p, "apply")) {
                    ASTNode *arg = parse_kw_arg(p);
                    da_push(&call->call.args, arg);
                }
                n->comprehension.comp_source = call;
            } else {
                ASTNode *ident = ast_new(p->arena, NODE_IDENT, kloc);
                ident->ident.ident = name;
                n->comprehension.comp_source = ident;
            }
        } else {
            n->comprehension.comp_source = parse_expression(p);
        }
    }

    /* Optional: where condition */
    n->comprehension.comp_filter = NULL;
    if (at_text(p, "where")) {
        eat(p);
        Token *t = cur(p);
        if (t->kind == TOK_KEYWORD && is_primitive_keyword(t->text)) {
            const char *kw = t->text;
            SrcLoc kloc = t->loc;
            eat(p);
            ASTNode *call = ast_new(p->arena, NODE_CALL, kloc);
            call->call.call_name = kw;
            da_init(&call->call.args);
            while (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
                   !at_text(p, "apply")) {
                ASTNode *arg = parse_kw_arg(p);
                da_push(&call->call.args, arg);
            }
            n->comprehension.comp_filter = call;
        } else if (t->kind == TOK_IDENT) {
            const char *name = t->text;
            SrcLoc kloc = t->loc;
            eat(p);
            if (!at_eof(p) && !at_text(p, "apply") &&
                !at(p, TOK_END) && !at(p, TOK_NEWLINE)) {
                ASTNode *call = ast_new(p->arena, NODE_CALL, kloc);
                call->call.call_name = name;
                da_init(&call->call.args);
                while (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
                       !at_text(p, "apply")) {
                    ASTNode *arg = parse_kw_arg(p);
                    da_push(&call->call.args, arg);
                }
                n->comprehension.comp_filter = call;
            } else {
                ASTNode *ident = ast_new(p->arena, NODE_IDENT, kloc);
                ident->ident.ident = name;
                n->comprehension.comp_filter = ident;
            }
        } else {
            n->comprehension.comp_filter = parse_expression(p);
        }
    }

    /* apply transform */
    expect_text(p, "apply");
    {
        Token *t = cur(p);
        if (t->kind == TOK_KEYWORD && is_primitive_keyword(t->text)) {
            const char *kw = t->text;
            SrcLoc kloc = t->loc;
            eat(p);
            n->comprehension.comp_transform = parse_keyword_call(p, kw, kloc);
        } else if (t->kind == TOK_IDENT) {
            const char *name = t->text;
            SrcLoc kloc = t->loc;
            eat(p);
            if (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
                !(cur(p)->kind == TOK_KEYWORD && (is_structural_keyword(cur(p)->text) ||
                                                   is_primitive_keyword(cur(p)->text)))) {
                n->comprehension.comp_transform = parse_keyword_call(p, name, kloc);
            } else {
                ASTNode *ident = ast_new(p->arena, NODE_IDENT, kloc);
                ident->ident.ident = name;
                n->comprehension.comp_transform = ident;
            }
        } else if (t->kind == TOK_KEYWORD && strcmp(t->text, "lambda") == 0) {
            n->comprehension.comp_transform = parse_lambda(p);
        } else {
            n->comprehension.comp_transform = parse_expression(p);
        }
    }

    return n;
}

/* ── Control Flow ────────────────────────────────────────────────── */

/* Parse a keyword-prefix call that stops at begin (for conditions/iterables).
   With do/end for expression grouping and begin/end for blocks, any begin
   token is unambiguously a block delimiter — no lookahead needed. */
static ASTNode *parse_condition_call(Parser *p, const char *keyword, SrcLoc loc) {
    ASTNode *n = ast_new(p->arena, NODE_CALL, loc);
    n->call.call_name = keyword;
    da_init(&n->call.args);
    while (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
           !at(p, TOK_BEGIN) &&
           !(cur(p)->kind == TOK_KEYWORD && (is_structural_keyword(cur(p)->text) ||
                                              is_primitive_keyword(cur(p)->text)))) {
        ASTNode *arg = parse_kw_arg(p);
        da_push(&n->call.args, arg);
    }
    return n;
}

/* Parse a condition: keyword call or expression, stops at begin */
static ASTNode *parse_condition(Parser *p) {
    skip_newlines(p);
    Token *t = cur(p);
    /* Primitive keyword call: equal x 0, less i 10, etc. */
    if (t->kind == TOK_KEYWORD && is_primitive_keyword(t->text)) {
        const char *kw = t->text;
        SrcLoc loc = t->loc;
        eat(p);
        return parse_condition_call(p, kw, loc);
    }
    /* Ident call: somecheck x y */
    if (t->kind == TOK_IDENT) {
        const char *name = t->text;
        SrcLoc loc = t->loc;
        eat(p);
        if (!at_eof(p) && !at(p, TOK_BEGIN) && !at(p, TOK_END) && !at(p, TOK_NEWLINE) &&
            !(cur(p)->kind == TOK_KEYWORD && is_structural_keyword(cur(p)->text))) {
            return parse_condition_call(p, name, loc);
        }
        ASTNode *n = ast_new(p->arena, NODE_IDENT, loc);
        n->ident.ident = name;
        return n;
    }
    /* do/end expression group or literal */
    if (t->kind == TOK_DO) return parse_do_end(p);
    return parse_expression(p);
}

static ASTNode *parse_if(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "if");

    ASTNode *n = ast_new(p->arena, NODE_IF, loc);
    n->if_stmt.condition = parse_condition(p);
    n->if_stmt.then_body = parse_begin_end(p);
    da_init(&n->if_stmt.elifs);
    n->if_stmt.else_body = NULL;

    while (at_text(p, "elif")) {
        eat(p);
        ASTNode *cond = parse_condition(p);
        ASTNode *body = parse_begin_end(p);
        da_push(&n->if_stmt.elifs, cond);
        da_push(&n->if_stmt.elifs, body);
    }

    if (at_text(p, "else")) {
        eat(p);
        n->if_stmt.else_body = parse_begin_end(p);
    }
    return n;
}

static ASTNode *parse_while(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "while");
    ASTNode *n = ast_new(p->arena, NODE_WHILE, loc);
    n->while_stmt.condition = parse_condition(p);
    n->while_stmt.while_body = parse_begin_end(p);
    return n;
}

static ASTNode *parse_for(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "for");
    ASTNode *n = ast_new(p->arena, NODE_FOR, loc);
    n->for_stmt.var_name = cur(p)->text;
    expect(p, TOK_IDENT, "loop variable");
    expect_text(p, "in");
    n->for_stmt.iterable = parse_condition(p);
    n->for_stmt.for_body = parse_begin_end(p);
    return n;
}

static ASTNode *parse_match(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    expect_text(p, "match");
    ASTNode *n = ast_new(p->arena, NODE_MATCH, loc);
    n->match_stmt.match_value = parse_condition(p);
    da_init(&n->match_stmt.cases);

    expect(p, TOK_BEGIN, "begin");
    while (!at_eof(p)) {
        skip_newlines(p);
        if (at(p, TOK_END)) break;
        if (at_text(p, "case")) {
            SrcLoc cloc = cur(p)->loc;
            eat(p);
            ASTNode *c = ast_new(p->arena, NODE_CASE, cloc);
            c->case_branch.case_pattern = parse_expression(p);
            c->case_branch.case_body = parse_begin_end(p);
            da_push(&n->match_stmt.cases, c);
        } else if (at_text(p, "default")) {
            SrcLoc dloc = cur(p)->loc;
            eat(p);
            ASTNode *d = ast_new(p->arena, NODE_DEFAULT, dloc);
            d->default_branch.default_body = parse_begin_end(p);
            da_push(&n->match_stmt.cases, d);
        } else {
            error_add(p->errors, ERR_PARSER, cur(p)->loc, "expected 'case' or 'default' in match");
            eat(p);
        }
    }
    expect(p, TOK_END, "end");
    return n;
}

/* ── Binding/Assignment ──────────────────────────────────────────── */

static ASTNode *parse_let_or_var(Parser *p, bool is_var) {
    SrcLoc loc = cur(p)->loc;
    eat(p); /* let or var */
    skip_newlines(p);
    ASTNode *n = ast_new(p->arena, is_var ? NODE_VAR : NODE_LET, loc);
    n->binding.bind_name = cur(p)->text;
    expect(p, TOK_IDENT, "variable name");

    if (at(p, TOK_DO))
        n->binding.value = parse_do_end(p);
    else if (cur(p)->kind == TOK_KEYWORD && strcmp(cur(p)->text, "lambda") == 0)
        n->binding.value = parse_lambda(p);
    else if (cur(p)->kind == TOK_KEYWORD && strcmp(cur(p)->text, "collect") == 0)
        n->binding.value = parse_collect(p);
    else if (cur(p)->kind == TOK_KEYWORD && is_primitive_keyword(cur(p)->text)) {
        const char *kw = cur(p)->text;
        SrcLoc kloc = cur(p)->loc;
        eat(p);
        n->binding.value = parse_keyword_call(p, kw, kloc);
    } else if (cur(p)->kind == TOK_IDENT && !at_eof(p) &&
               peek(p, 1)->kind != TOK_EOF &&
               peek(p, 1)->kind != TOK_END &&
               peek(p, 1)->kind != TOK_NEWLINE &&
               !(peek(p, 1)->kind == TOK_KEYWORD && is_structural_keyword(peek(p, 1)->text))) {
        /* Identifier followed by args — treat as call (e.g., Point 3 4) */
        const char *nm = cur(p)->text;
        SrcLoc kloc = cur(p)->loc;
        eat(p);
        n->binding.value = parse_keyword_call(p, nm, kloc);
    } else
        n->binding.value = parse_expression(p);
    return n;
}

static ASTNode *parse_assign(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    eat(p); /* assign */
    ASTNode *n = ast_new(p->arena, NODE_ASSIGN, loc);
    n->assign.assign_name = cur(p)->text;
    expect(p, TOK_IDENT, "variable name");

    if (at(p, TOK_DO))
        n->assign.value = parse_do_end(p);
    else if (cur(p)->kind == TOK_KEYWORD && is_primitive_keyword(cur(p)->text)) {
        const char *kw = cur(p)->text;
        SrcLoc kloc = cur(p)->loc;
        eat(p);
        n->assign.value = parse_keyword_call(p, kw, kloc);
    } else
        n->assign.value = parse_expression(p);
    return n;
}

static ASTNode *parse_return(Parser *p) {
    SrcLoc loc = cur(p)->loc;
    eat(p); /* return */
    ASTNode *n = ast_new(p->arena, NODE_RETURN, loc);
    if (at(p, TOK_DO))
        n->ret.value = parse_do_end(p);
    else if (cur(p)->kind == TOK_KEYWORD && is_primitive_keyword(cur(p)->text)) {
        const char *kw = cur(p)->text;
        SrcLoc kloc = cur(p)->loc;
        eat(p);
        n->ret.value = parse_keyword_call(p, kw, kloc);
    } else if (!at_eof(p) && !at(p, TOK_END))
        n->ret.value = parse_expression(p);
    else
        n->ret.value = NULL;
    return n;
}

/* ── Statement Parsing ───────────────────────────────────────────── */

ASTNode *parse_statement(Parser *p) {
    skip_newlines(p);
    if (at_eof(p)) return NULL;

    Token *t = cur(p);

    if (t->kind == TOK_KEYWORD) {
        if (strcmp(t->text, "fn") == 0) return parse_fn_decl(p);
        if (strcmp(t->text, "trait") == 0) return parse_trait_decl(p);
        if (strcmp(t->text, "implement") == 0) return parse_implement(p);
        if (strcmp(t->text, "algebra") == 0) return parse_algebra_or_library(p, false);
        if (strcmp(t->text, "library") == 0) return parse_algebra_or_library(p, true);
        if (strcmp(t->text, "use") == 0) return parse_use(p);
        if (strcmp(t->text, "import") == 0) return parse_import(p);
        if (strcmp(t->text, "precedence") == 0) return parse_precedence(p);
        if (strcmp(t->text, "alias") == 0) return parse_alias(p);
        if (strcmp(t->text, "type") == 0) return parse_type_decl(p);
        if (strcmp(t->text, "if") == 0) return parse_if(p);
        if (strcmp(t->text, "while") == 0) return parse_while(p);
        if (strcmp(t->text, "for") == 0) return parse_for(p);
        if (strcmp(t->text, "match") == 0) return parse_match(p);
        if (strcmp(t->text, "let") == 0) return parse_let_or_var(p, false);
        if (strcmp(t->text, "var") == 0) return parse_let_or_var(p, true);
        if (strcmp(t->text, "assign") == 0) return parse_assign(p);
        if (strcmp(t->text, "return") == 0) return parse_return(p);

        if (strcmp(t->text, "lambda") == 0) return parse_lambda(p);
        if (strcmp(t->text, "collect") == 0) return parse_collect(p);

        if (strcmp(t->text, "break") == 0) {
            SrcLoc loc = t->loc;
            eat(p);
            return ast_new(p->arena, NODE_BREAK, loc);
        }
        if (strcmp(t->text, "continue") == 0) {
            SrcLoc loc = t->loc;
            eat(p);
            return ast_new(p->arena, NODE_CONTINUE, loc);
        }

        /* Primitive keyword call: add, times, get, set, etc. */
        if (is_primitive_keyword(t->text)) {
            const char *kw = t->text;
            SrcLoc loc = t->loc;
            eat(p);
            return parse_keyword_call(p, kw, loc);
        }

        /* Type keywords and other keywords used as calls (e.g., map rows cols) */
        if (is_type_keyword(t->text)) {
            const char *kw = t->text;
            SrcLoc loc = t->loc;
            eat(p);
            if (!at_eof(p) && !at(p, TOK_END) &&
                !(cur(p)->kind == TOK_KEYWORD && (is_structural_keyword(cur(p)->text) ||
                                                   is_primitive_keyword(cur(p)->text)))) {
                return parse_keyword_call(p, kw, loc);
            }
            /* Bare type keyword as expression (e.g., int as a value) */
            ASTNode *n = ast_new(p->arena, NODE_IDENT, loc);
            n->ident.ident = kw;
            return n;
        }

        /* Skip unknown keywords */
        error_add(p->errors, ERR_PARSER, t->loc, "unexpected keyword '%s'", t->text);
        eat(p);
        return NULL;
    }

    /* Identifier: could be a keyword-prefix call (user function) or expression */
    if (t->kind == TOK_IDENT) {
        const char *name = t->text;
        SrcLoc loc = t->loc;
        eat(p);

        /* Check if followed by arguments on the same line */
        if (!at_eof(p) && !at(p, TOK_END) && !at(p, TOK_BEGIN) && !at(p, TOK_NEWLINE) &&
            !(cur(p)->kind == TOK_KEYWORD && (is_structural_keyword(cur(p)->text) ||
                                               is_primitive_keyword(cur(p)->text)))) {
            return parse_keyword_call(p, name, loc);
        }
        /* Bare identifier */
        ASTNode *n = ast_new(p->arena, NODE_IDENT, loc);
        n->ident.ident = name;
        return n;
    }

    /* Standalone string literal: treat as comment, skip */
    if (t->kind == TOK_STRING_LIT) {
        eat(p);
        return NULL;
    }

    /* begin/end block */
    if (t->kind == TOK_BEGIN) {
        return parse_begin_end(p);
    }

    /* do/end expression group */
    if (t->kind == TOK_DO) {
        return parse_do_end(p);
    }

    /* Other: expression */
    return parse_expression(p);
}

/* ── Top-Level ───────────────────────────────────────────────────── */


ASTNode *parse_program(Parser *p) {
    ASTNode *prog = ast_new(p->arena, NODE_PROGRAM, cur(p)->loc);
    da_init(&prog->program.top_level);

    while (!at_eof(p)) {
        ASTNode *decl = parse_statement(p);
        if (decl) da_push(&prog->program.top_level, decl);
    }
    return prog;
}
