#include "common.h"
#include "tokenizer.h"
#include "parser.h"
#include "algebra.h"
#include "desugarer.h"
#include "skw_emitter.h"
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

static void test_standard_arithmetic_desugar(void) {
    printf("--- StandardArithmetic desugaring ---\n");

    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* Build algebra manually */
    AlgebraRegistry reg;
    algebra_registry_init(&reg, &arena, &it);
    AlgebraEntry *alg = algebra_registry_add(&reg, "StandardArithmetic");

    /* Parse fn declarations to register sigil bindings */
    const char *decls =
        "algebra StandardArithmetic\n"
        "fn add int a + int b returns int\n"
        "fn times int a * int b returns int\n"
        "precedence + *\n";

    Tokenizer t;
    tokenizer_init(&t, decls, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    /* Register */
    if (ast->program.top_level.count > 0) {
        ASTNode *alg_node = ast->program.top_level.items[0];
        if (alg_node->kind == NODE_ALGEBRA) {
            algebra_register_declarations(&reg, alg, alg_node);
        }
    }

    ASSERT(alg->bindings.count >= 2, "registered at least 2 bindings");
    ASSERT(alg->precedence.count == 2, "precedence has 2 entries");

    /* Test precedence-climbing desugaring with raw tokens */
    Desugarer d;
    desugarer_init(&d, &arena, &it, &reg, NULL, &errors);
    d.current_algebra = alg;

    /* Tokenize "a + b * c" */
    Tokenizer t2;
    tokenizer_init(&t2, "a + b * c", "<test>", &it, &errors, NULL);
    TokenList expr_tokens = tokenize_all(&t2);

    /* Remove EOF token for expression parsing */
    int count = expr_tokens.count - 1; /* exclude EOF */
    ASTNode *result = desugar_expression(&d, expr_tokens.items, count);

    ASSERT(result != NULL, "desugared result is not NULL");
    if (result) {
        ASSERT(result->kind == NODE_CALL, "top level is a call");
        if (result->kind == NODE_CALL) {
            ASSERT(strcmp(result->call.call_name, "add") == 0, "top call is 'add' (lower precedence)");
            /* The second arg should be a times call */
            if (result->call.args.count == 2) {
                ASTNode *rhs = result->call.args.items[1];
                ASSERT(rhs->kind == NODE_CALL && strcmp(rhs->call.call_name, "times") == 0,
                       "rhs is 'times' (higher precedence)");
            }
        }
    }

    da_free(&tokens);
    da_free(&expr_tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_prefix_desugar(void) {
    printf("--- Prefix operator desugaring ---\n");

    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    AlgebraRegistry reg;
    algebra_registry_init(&reg, &arena, &it);
    AlgebraEntry *alg = algebra_registry_add(&reg, "Test");

    const char *decls =
        "algebra Test\n"
        "fn negate - int a returns int\n"
        "precedence -\n";

    Tokenizer t;
    tokenizer_init(&t, decls, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    if (ast->program.top_level.count > 0 && ast->program.top_level.items[0]->kind == NODE_ALGEBRA)
        algebra_register_declarations(&reg, alg, ast->program.top_level.items[0]);

    Desugarer d;
    desugarer_init(&d, &arena, &it, &reg, NULL, &errors);
    d.current_algebra = alg;

    Tokenizer t2;
    tokenizer_init(&t2, "- x", "<test>", &it, &errors, NULL);
    TokenList expr_tokens = tokenize_all(&t2);
    ASTNode *result = desugar_expression(&d, expr_tokens.items, expr_tokens.count - 1);

    ASSERT(result != NULL, "prefix result not NULL");
    if (result && result->kind == NODE_CALL) {
        ASSERT(strcmp(result->call.call_name, "negate") == 0, "desugared to negate");
        ASSERT(result->call.args.count == 1, "one argument");
    }

    da_free(&tokens);
    da_free(&expr_tokens);
    intern_free(&it);
    arena_free(&arena);
}

int main(void) {
    printf("=== Desugarer Tests ===\n\n");

    test_standard_arithmetic_desugar();
    test_prefix_desugar();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
