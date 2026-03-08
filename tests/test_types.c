#include "common.h"
#include "tokenizer.h"
#include "parser.h"
#include "types.h"
#include "traits.h"
#include "algebra.h"
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

static void test_type_equality(void) {
    printf("--- Type equality ---\n");
    Arena arena;
    arena_init(&arena);

    TypeRef *int1 = make_type(&arena, TYPE_INT);
    TypeRef *int2 = make_type(&arena, TYPE_INT);
    TypeRef *float1 = make_type(&arena, TYPE_FLOAT);

    ASSERT(types_equal(int1, int2), "int == int");
    ASSERT(!types_equal(int1, float1), "int != float");
    ASSERT(types_equal(NULL, NULL), "NULL == NULL");
    ASSERT(!types_equal(int1, NULL), "int != NULL");

    arena_free(&arena);
}

static void test_type_env(void) {
    printf("--- Type environment ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);

    TypeEnv *env = type_env_push(NULL);
    env->arena = &arena;

    TypeRef *int_type = make_type(&arena, TYPE_INT);
    const char *x = intern_cstr(&it, "x");
    type_env_bind(env, x, int_type, false);

    TypeRef *found = type_env_lookup(env, x);
    ASSERT(found != NULL, "x found in env");
    ASSERT(found->kind == TYPE_INT, "x is int");

    /* Child scope */
    TypeEnv *child = type_env_push(env);
    TypeRef *float_type = make_type(&arena, TYPE_FLOAT);
    const char *y = intern_cstr(&it, "y");
    type_env_bind(child, y, float_type, true);

    ASSERT(type_env_lookup(child, x) != NULL, "x visible in child");
    ASSERT(type_env_lookup(child, y) != NULL, "y visible in child");
    ASSERT(type_env_lookup(env, y) == NULL, "y not visible in parent");

    intern_free(&it);
    arena_free(&arena);
}

static void test_basic_type_checking(void) {
    printf("--- Basic type checking ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    const char *source = "let x 42\nlet y 3\nadd x y";
    Tokenizer t;
    tokenizer_init(&t, source, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);

    ASSERT(ok, "type check passes for valid program");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_trait_prerequisites(void) {
    printf("--- Trait prerequisites ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    TraitRegistry tr;
    trait_registry_init(&tr, &arena, &it, &errors);

    /* Create trait Arithmetic T */
    const char *src = "trait Arithmetic T\n  fn add T a returns T\n"
                      "trait Linear T\n  fn scale T a returns T";
    Tokenizer t;
    tokenizer_init(&t, src, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    for (int i = 0; i < ast->program.top_level.count; i++) {
        if (ast->program.top_level.items[i]->kind == NODE_TRAIT_DECL)
            trait_register_def(&tr, ast->program.top_level.items[i]);
    }

    TraitDef *arith = trait_find_def(&tr, "Arithmetic");
    TraitDef *linear = trait_find_def(&tr, "Linear");
    ASSERT(arith != NULL, "Arithmetic trait found");
    ASSERT(linear != NULL, "Linear trait found");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_missing_return(void) {
    printf("--- Missing return in non-void fn ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* fn with returns int but no return statement — should error */
    const char *bad = "fn double int a returns int\n  begin add a a end";
    Tokenizer t;
    tokenizer_init(&t, bad, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);
    ASSERT(!ok, "type check fails for missing return");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_explicit_return(void) {
    printf("--- Explicit return in non-void fn ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* fn with returns int and explicit return — should pass */
    const char *good = "fn double int a returns int\n  begin return begin add a a end end";
    Tokenizer t;
    tokenizer_init(&t, good, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);
    ASSERT(ok, "type check passes with explicit return");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_void_no_return(void) {
    printf("--- Void fn without return ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* fn with returns void and no return — should pass */
    const char *src = "fn greet int a returns void\n  begin add a a end";
    Tokenizer t;
    tokenizer_init(&t, src, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);
    ASSERT(ok, "type check passes for void fn without return");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_mutability_check(void) {
    printf("--- Mutability checking ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* assign to let (immutable) should error */
    const char *bad = "let x 42\nassign x 10";
    Tokenizer t;
    tokenizer_init(&t, bad, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);
    ASSERT(!ok, "assign to immutable let fails");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_mutability_var_ok(void) {
    printf("--- Mutability var ok ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* assign to var (mutable) should pass */
    const char *good = "var x 42\nassign x 10";
    Tokenizer t;
    tokenizer_init(&t, good, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);
    ASSERT(ok, "assign to mutable var passes");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_fn_return_type_inference(void) {
    printf("--- Fn return type inference ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* Define a fn, call it, check that the return type propagates */
    const char *src =
        "fn double int a returns int\n"
        "  begin return begin add a a end end\n"
        "let x begin double 5 end\n";
    Tokenizer t;
    tokenizer_init(&t, src, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    type_check(&tc, ast);

    /* x should have type int from double's return type */
    TypeRef *x_type = type_env_lookup(tc.global_env, intern_cstr(&it, "x"));
    ASSERT(x_type != NULL, "x is bound");
    ASSERT(x_type->kind == TYPE_INT, "x has type int from fn return");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_generic_unification(void) {
    printf("--- Generic type unification ---\n");
    Arena arena;
    arena_init(&arena);

    /* Test unify_generic_return directly */
    TypeRef *param_t = make_generic_type(&arena, "T");
    TypeRef *arg_int = make_type(&arena, TYPE_INT);
    TypeRef *ret_t = make_generic_type(&arena, "T");

    TypeRef *param_types[] = { param_t, param_t };
    TypeRef *arg_types[] = { arg_int, arg_int };

    TypeRef *result = unify_generic_return(&arena, 2, param_types, arg_types, ret_t);
    ASSERT(result->kind == TYPE_INT, "T unifies to int");

    /* Different type var should not unify */
    TypeRef *ret_u = make_generic_type(&arena, "U");
    TypeRef *result2 = unify_generic_return(&arena, 2, param_types, arg_types, ret_u);
    ASSERT(result2->kind == TYPE_GENERIC, "U stays generic (no binding)");

    arena_free(&arena);
}

static void test_collision_detection(void) {
    printf("--- Collision detection ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    const char *src =
        "algebra TestAlg\n"
        "fn foo int a @ int b returns int\n"
        "fn bar int a @ int b returns float\n"
        "precedence @\n";
    Tokenizer t;
    tokenizer_init(&t, src, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    AlgebraRegistry ar;
    algebra_registry_init(&ar, &arena, &it);
    ASTNode *alg_node = ast->program.top_level.items[0];
    AlgebraEntry *alg = algebra_registry_add(&ar, alg_node->algebra.algebra_name);
    algebra_register_declarations(&ar, alg, alg_node);

    bool ok = algebra_check_collisions(alg, &errors);
    ASSERT(!ok, "collision detected: same sigil, fixity, param types");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_trait_completeness(void) {
    printf("--- Trait completeness ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* Incomplete implementation: trait has add and times, impl only has add */
    const char *src =
        "trait Arithmetic T\n"
        "  fn add T a + T b returns T\n"
        "  fn times T a * T b returns T\n"
        "implement Arithmetic int\n"
        "  fn add int a + int b returns int\n";
    Tokenizer t;
    tokenizer_init(&t, src, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TraitRegistry tr;
    trait_registry_init(&tr, &arena, &it, &errors);
    for (int i = 0; i < ast->program.top_level.count; i++) {
        ASTNode *n = ast->program.top_level.items[i];
        if (n->kind == NODE_TRAIT_DECL) trait_register_def(&tr, n);
        if (n->kind == NODE_IMPLEMENT) trait_register_impl(&tr, n);
    }

    bool ok = trait_check_all(&tr);
    ASSERT(!ok, "incomplete implementation detected");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_return_type_mismatch(void) {
    printf("--- Return type mismatch ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* fn returns int but return expression is bool */
    const char *src =
        "fn bad int a returns int\n"
        "  begin return true end\n";
    Tokenizer t;
    tokenizer_init(&t, src, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);
    ASSERT(!ok, "return type mismatch detected");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

static void test_return_type_ok(void) {
    printf("--- Return type match ---\n");
    Arena arena;
    arena_init(&arena);
    InternTable it;
    intern_init(&it);
    ErrorList errors;
    error_list_init(&errors, &arena);

    /* fn returns int and return expression is int */
    const char *src =
        "fn good int a returns int\n"
        "  begin return 42 end\n";
    Tokenizer t;
    tokenizer_init(&t, src, "<test>", &it, &errors, NULL);
    TokenList tokens = tokenize_all(&t);
    Parser p;
    parser_init(&p, tokens, &arena, &it, &errors);
    ASTNode *ast = parse_program(&p);

    TypeChecker tc;
    type_checker_init(&tc, &arena, &it, &errors);
    bool ok = type_check(&tc, ast);
    ASSERT(ok, "return type match passes");

    da_free(&tokens);
    intern_free(&it);
    arena_free(&arena);
}

int main(void) {
    printf("=== Type System Tests ===\n\n");

    test_type_equality();
    test_type_env();
    test_basic_type_checking();
    test_trait_prerequisites();
    test_missing_return();
    test_explicit_return();
    test_void_no_return();
    test_mutability_check();
    test_mutability_var_ok();
    test_fn_return_type_inference();
    test_generic_unification();
    test_collision_detection();
    test_trait_completeness();
    test_return_type_mismatch();
    test_return_type_ok();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
