#include "common.h"
#include "tokenizer.h"
#include "parser.h"
#include "errors.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    else { tests_passed++; } \
} while(0)

static ASTNode *parse_source(const char *source) {
    static Arena arena;
    static InternTable intern_tab;
    static bool initialized = false;
    if (!initialized) {
        arena_init(&arena);
        intern_init(&intern_tab);
        initialized = true;
    }
    ErrorList errors;
    error_list_init(&errors, &arena);
    Tokenizer t;
    tokenizer_init(&t, source, "<test>", &intern_tab, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &intern_tab, &errors);
    return parse_program(&p);
}

static void test_keyword_prefix_call(void) {
    printf("--- Keyword prefix calls ---\n");
    ASTNode *prog = parse_source("add a b");
    ASSERT(prog->kind == NODE_PROGRAM, "program node");
    ASSERT(prog->program.top_level.count == 1, "one top-level statement");
    ASTNode *call = prog->program.top_level.items[0];
    ASSERT(call->kind == NODE_CALL, "is a call");
    ASSERT(strcmp(call->call.call_name, "add") == 0, "call name is 'add'");
    ASSERT(call->call.args.count == 2, "two arguments");
}

static void test_do_end_nesting(void) {
    printf("--- Do/end nesting ---\n");
    ASTNode *prog = parse_source("add a do times b c end");
    ASSERT(prog->kind == NODE_PROGRAM, "program node");
    ASTNode *call = prog->program.top_level.items[0];
    ASSERT(call->kind == NODE_CALL, "is a call");
    ASSERT(call->call.args.count == 2, "two args");
    ASTNode *inner = call->call.args.items[1];
    ASSERT(inner->kind == NODE_BEGIN_END, "second arg is do/end");
}

static void test_fn_declaration(void) {
    printf("--- fn declaration ---\n");
    ASTNode *prog = parse_source("fn multiply int a + int b returns int\n  begin add a b end");
    ASSERT(prog->program.top_level.count == 1, "one declaration");
    ASTNode *fn = prog->program.top_level.items[0];
    ASSERT(fn->kind == NODE_FN_DECL, "is fn decl");
    ASSERT(strcmp(fn->fn_decl.fn_name, "multiply") == 0, "fn name");
    ASSERT(fn->fn_decl.fixity == FIXITY_INFIX, "infix fixity");
    ASSERT(fn->fn_decl.sigils.count == 1, "one sigil");
    ASSERT(strcmp(fn->fn_decl.sigils.items[0], "+") == 0, "sigil is +");
}

static void test_fn_prefix_operator(void) {
    printf("--- fn prefix operator ---\n");
    ASTNode *prog = parse_source("fn negate - int a returns int\n  begin subtract 0 a end");
    ASTNode *fn = prog->program.top_level.items[0];
    ASSERT(fn->kind == NODE_FN_DECL, "is fn decl");
    ASSERT(fn->fn_decl.fixity == FIXITY_PREFIX, "prefix fixity");
}

static void test_fn_postfix_operator(void) {
    printf("--- fn postfix operator ---\n");
    ASTNode *prog = parse_source("fn postinc int a ++ returns int\n  begin add a 1 end");
    ASTNode *fn = prog->program.top_level.items[0];
    ASSERT(fn->kind == NODE_FN_DECL, "is fn decl");
    ASSERT(fn->fn_decl.fixity == FIXITY_POSTFIX, "postfix fixity");
}

static void test_fn_bracketed(void) {
    printf("--- fn bracketed operator ---\n");
    ASTNode *prog = parse_source("fn getelem mat m [ int i , int j ] returns int\n  begin get m i j end");
    ASTNode *fn = prog->program.top_level.items[0];
    ASSERT(fn->kind == NODE_FN_DECL, "is fn decl");
    ASSERT(fn->fn_decl.fixity == FIXITY_BRACKETED, "bracketed fixity");
    ASSERT(fn->fn_decl.sigils.count >= 3, "multiple sigils ([ , ])");
}

static void test_if_elif_else(void) {
    printf("--- if/elif/else ---\n");
    ASTNode *prog = parse_source(
        "if x begin\n  add a 1\nend elif y begin\n  add a 2\nend else begin\n  add a 3\nend");
    ASTNode *stmt = prog->program.top_level.items[0];
    ASSERT(stmt->kind == NODE_IF, "is if");
    ASSERT(stmt->if_stmt.elifs.count == 2, "one elif (cond + body)");
    ASSERT(stmt->if_stmt.else_body != NULL, "has else body");
}

static void test_let_var_assign(void) {
    printf("--- let/var/assign ---\n");
    ASTNode *prog = parse_source("let x 42\nvar y 0\nassign y 10");
    ASSERT(prog->program.top_level.count == 3, "three statements");
    ASSERT(prog->program.top_level.items[0]->kind == NODE_LET, "let");
    ASSERT(prog->program.top_level.items[1]->kind == NODE_VAR, "var");
    ASSERT(prog->program.top_level.items[2]->kind == NODE_ASSIGN, "assign");
}

static void test_trait_declaration(void) {
    printf("--- trait declaration ---\n");
    ASTNode *prog = parse_source(
        "trait Arithmetic T\n  fn add T a + T b returns T\n  fn negate - T a returns T");
    ASSERT(prog->program.top_level.count >= 1, "at least one decl");
    ASTNode *trait = prog->program.top_level.items[0];
    ASSERT(trait->kind == NODE_TRAIT_DECL, "is trait decl");
    ASSERT(strcmp(trait->trait_decl.trait_name, "Arithmetic") == 0, "trait name");
    ASSERT(strcmp(trait->trait_decl.type_var, "T") == 0, "type var");
    ASSERT(trait->trait_decl.methods.count == 2, "two methods");
}

static void test_fn_var_annotation(void) {
    printf("--- fn var (mutable ref) annotation ---\n");
    ASTNode *prog = parse_source("fn assign var int a = int b returns void");
    ASTNode *fn = prog->program.top_level.items[0];
    ASSERT(fn->kind == NODE_FN_DECL, "is fn decl");
    ASSERT(fn->fn_decl.pattern.count >= 3, "has params and sigils");
    /* First param should be 'var int a' with is_mutable = true */
    PatElem *first_param = NULL;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM) {
            first_param = &fn->fn_decl.pattern.items[i];
            break;
        }
    }
    ASSERT(first_param != NULL, "has a param");
    ASSERT(first_param->is_mutable == true, "first param is mutable");
    ASSERT(strcmp(first_param->param_name, "a") == 0, "param name is a");
    /* Second param should not be mutable */
    PatElem *second_param = NULL;
    int param_idx = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM) {
            if (param_idx == 1) { second_param = &fn->fn_decl.pattern.items[i]; break; }
            param_idx++;
        }
    }
    ASSERT(second_param != NULL, "has second param");
    ASSERT(second_param->is_mutable == false, "second param is not mutable");
}

int main(void) {
    printf("=== Parser Tests ===\n\n");

    test_keyword_prefix_call();
    test_do_end_nesting();
    test_fn_declaration();
    test_fn_prefix_operator();
    test_fn_postfix_operator();
    test_fn_bracketed();
    test_if_elif_else();
    test_let_var_assign();
    test_trait_declaration();
    test_fn_var_annotation();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
