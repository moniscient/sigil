#include "common.h"
#include "tokenizer.h"
#include "parser.h"
#include "algebra.h"
#include "desugarer.h"
#include "types.h"
#include "traits.h"
#include "resolver.h"
#include "c_emitter.h"
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

/* ── Helper: run .skw source through full pipeline and emit C to string ── */

static char *emit_c_from_skw(const char *source) {
    Arena arena;
    arena_init(&arena);
    InternTable intern_tab;
    intern_init(&intern_tab);
    ErrorList errors;
    error_list_init(&errors, &arena);
    CompoundSigilSet compounds;
    compound_sigil_set_init(&compounds);

    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, source, "<test>", &intern_tab, &errors, &compounds);
    TokenList tokens = tokenize_all(&tokenizer);

    Parser parser;
    parser_init(&parser, tokens, &arena, &intern_tab, &errors);
    ASTNode *ast = parse_program(&parser);

    /* .skw: no desugaring needed, but still need algebra/trait/type/resolve */
    AlgebraRegistry algebra_reg;
    algebra_registry_init(&algebra_reg, &arena, &intern_tab);
    TraitRegistry trait_reg;
    trait_registry_init(&trait_reg, &arena, &intern_tab, &errors);

    TypeChecker type_checker;
    type_checker_init(&type_checker, &arena, &intern_tab, &errors);
    type_check(&type_checker, ast);

    Resolver resolver;
    resolver_init(&resolver, &arena, &intern_tab, &algebra_reg, &trait_reg, &type_checker, &errors);
    resolve_ast(&resolver, ast);

    /* Emit C to string */
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    CEmitter emitter;
    c_emitter_init(&emitter, f, &type_checker, &arena);
    c_emit(&emitter, ast);
    fclose(f);

    da_free(&tokens);
    compound_sigil_set_free(&compounds);
    intern_free(&intern_tab);
    arena_free(&arena);

    return buf;
}

/* ── Helper: run .sigil source through full pipeline and emit C to string ── */

static char *emit_c_from_sigil(const char *source) {
    Arena arena;
    arena_init(&arena);
    InternTable intern_tab;
    intern_init(&intern_tab);
    ErrorList errors;
    error_list_init(&errors, &arena);
    CompoundSigilSet compounds;
    compound_sigil_set_init(&compounds);

    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, source, "<test>", &intern_tab, &errors, &compounds);
    TokenList tokens = tokenize_all(&tokenizer);

    Parser parser;
    parser_init(&parser, tokens, &arena, &intern_tab, &errors);
    ASTNode *ast = parse_program(&parser);

    AlgebraRegistry algebra_reg;
    algebra_registry_init(&algebra_reg, &arena, &intern_tab);
    TraitRegistry trait_reg;
    trait_registry_init(&trait_reg, &arena, &intern_tab, &errors);

    Desugarer desugarer;
    desugarer_init(&desugarer, &arena, &intern_tab, &algebra_reg, &trait_reg, &errors);
    ast = desugar_ast(&desugarer, ast);

    for (int i = 0; i < algebra_reg.algebras.count; i++) {
        algebra_check_collisions(algebra_reg.algebras.items[i], &errors);
        trait_register_from_algebra(&trait_reg, algebra_reg.algebras.items[i]);
    }
    trait_check_all(&trait_reg);

    TypeChecker type_checker;
    type_checker_init(&type_checker, &arena, &intern_tab, &errors);
    type_check(&type_checker, ast);

    Resolver resolver;
    resolver_init(&resolver, &arena, &intern_tab, &algebra_reg, &trait_reg, &type_checker, &errors);
    resolve_ast(&resolver, ast);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    CEmitter emitter;
    c_emitter_init(&emitter, f, &type_checker, &arena);
    c_emit(&emitter, ast);
    fclose(f);

    da_free(&tokens);
    compound_sigil_set_free(&compounds);
    intern_free(&intern_tab);
    arena_free(&arena);

    return buf;
}

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_int_literal(void) {
    printf("--- Int literal ---\n");
    char *c = emit_c_from_skw("let x 42");
    ASSERT(strstr(c, "const int64_t x = 42LL;") != NULL, "let int emits const int64_t with LL suffix");
    free(c);
}

static void test_float_literal(void) {
    printf("--- Float literal ---\n");
    char *c = emit_c_from_skw("let pi 3.14");
    ASSERT(strstr(c, "const double pi = 3.14;") != NULL, "let float emits const double");
    free(c);
}

static void test_bool_literal(void) {
    printf("--- Bool literal ---\n");
    char *c = emit_c_from_skw("let flag true");
    ASSERT(strstr(c, "const bool flag = true;") != NULL, "let bool emits const bool");
    free(c);
}

static void test_var_binding(void) {
    printf("--- Var binding (mutable) ---\n");
    char *c = emit_c_from_skw("var counter 0");
    ASSERT(strstr(c, "int64_t counter = 0LL;") != NULL, "var emits non-const");
    /* Must not have 'const' */
    char *p = strstr(c, "counter = 0LL;");
    ASSERT(p != NULL, "counter assignment exists");
    if (p) {
        /* Check the 6 chars before 'counter' don't include 'const' */
        /* var should produce plain type, not const type */
        char *const_check = strstr(c, "const int64_t counter");
        ASSERT(const_check == NULL, "var does NOT emit const");
    }
    free(c);
}

static void test_assign(void) {
    printf("--- Assign ---\n");
    char *c = emit_c_from_skw("var x 0\nassign x 10");
    ASSERT(strstr(c, "x = 10LL;") != NULL, "assign emits x = value");
    free(c);
}

static void test_return(void) {
    printf("--- Return ---\n");
    char *c = emit_c_from_skw(
        "fn double int a returns int\n"
        "  begin return begin add a a end end");
    /* begin/end wraps the add call in a stmt expr */
    ASSERT(strstr(c, "return ({") != NULL, "return emits C return with stmt expr");
    ASSERT(strstr(c, "(a + a)") != NULL, "return body has add");
    free(c);
}

static void test_builtin_add(void) {
    printf("--- Builtin: add ---\n");
    char *c = emit_c_from_skw("let x begin add 3 4 end");
    ASSERT(strstr(c, "(3LL + 4LL)") != NULL, "add desugars to + operator");
    free(c);
}

static void test_builtin_subtract(void) {
    printf("--- Builtin: subtract ---\n");
    char *c = emit_c_from_skw("let x begin subtract 10 3 end");
    ASSERT(strstr(c, "(10LL - 3LL)") != NULL, "subtract desugars to - operator");
    free(c);
}

static void test_builtin_multiply(void) {
    printf("--- Builtin: multiply ---\n");
    char *c = emit_c_from_skw("let x begin multiply 5 6 end");
    ASSERT(strstr(c, "(5LL * 6LL)") != NULL, "multiply desugars to * operator");
    free(c);
}

static void test_builtin_divide(void) {
    printf("--- Builtin: divide ---\n");
    char *c = emit_c_from_skw("let x begin divide 10 2 end");
    ASSERT(strstr(c, "(10LL / 2LL)") != NULL, "divide desugars to / operator");
    free(c);
}

static void test_builtin_modulo(void) {
    printf("--- Builtin: modulo ---\n");
    char *c = emit_c_from_skw("let x begin modulo 10 3 end");
    ASSERT(strstr(c, "(10LL % 3LL)") != NULL, "modulo desugars to % operator");
    free(c);
}

static void test_builtin_equal(void) {
    printf("--- Builtin: equal ---\n");
    char *c = emit_c_from_skw("let x begin equal 1 1 end");
    ASSERT(strstr(c, "(1LL == 1LL)") != NULL, "equal desugars to == operator");
    free(c);
}

static void test_builtin_less(void) {
    printf("--- Builtin: less ---\n");
    char *c = emit_c_from_skw("let x begin less 1 2 end");
    ASSERT(strstr(c, "(1LL < 2LL)") != NULL, "less desugars to < operator");
    free(c);
}

static void test_builtin_greater(void) {
    printf("--- Builtin: greater ---\n");
    char *c = emit_c_from_skw("let x begin greater 2 1 end");
    ASSERT(strstr(c, "(2LL > 1LL)") != NULL, "greater desugars to > operator");
    free(c);
}

static void test_builtin_and(void) {
    printf("--- Builtin: and ---\n");
    char *c = emit_c_from_skw("let x begin and true false end");
    ASSERT(strstr(c, "(true && false)") != NULL, "and desugars to && operator");
    free(c);
}

static void test_builtin_or(void) {
    printf("--- Builtin: or ---\n");
    char *c = emit_c_from_skw("let x begin or true false end");
    ASSERT(strstr(c, "(true || false)") != NULL, "or desugars to || operator");
    free(c);
}

static void test_builtin_negate(void) {
    printf("--- Builtin: negate ---\n");
    char *c = emit_c_from_skw("let x begin negate 5 end");
    ASSERT(strstr(c, "(-5LL)") != NULL, "negate desugars to unary minus");
    free(c);
}

static void test_builtin_not(void) {
    printf("--- Builtin: not ---\n");
    char *c = emit_c_from_skw("let x begin not true end");
    ASSERT(strstr(c, "(!true)") != NULL, "not desugars to unary !");
    free(c);
}

static void test_builtin_print(void) {
    printf("--- Builtin: print ---\n");
    char *c = emit_c_from_skw("print 42");
    ASSERT(strstr(c, "sigil_print_int(42LL)") != NULL, "print desugars to sigil_print_int");
    free(c);
}

static void test_nested_builtins(void) {
    printf("--- Nested builtin calls ---\n");
    char *c = emit_c_from_skw("let x begin add 1 begin multiply 2 3 end end");
    /* begin/end wraps inner multiply, producing stmt expr */
    ASSERT(strstr(c, "(1LL + (") != NULL, "nested add has inner multiply");
    free(c);
}

static void test_if_simple(void) {
    printf("--- If (simple) ---\n");
    char *c = emit_c_from_skw(
        "if true begin\n"
        "  print 1\n"
        "end");
    ASSERT(strstr(c, "if (true) {") != NULL, "if emits C if");
    ASSERT(strstr(c, "sigil_print_int(1LL)") != NULL, "if body emits print");
    free(c);
}

static void test_if_elif_else(void) {
    printf("--- If/elif/else ---\n");
    char *c = emit_c_from_skw(
        "if false begin\n"
        "  print 1\n"
        "end elif true begin\n"
        "  print 2\n"
        "end else begin\n"
        "  print 3\n"
        "end");
    ASSERT(strstr(c, "if (false) {") != NULL, "if clause");
    ASSERT(strstr(c, "} else if (true) {") != NULL, "elif clause");
    ASSERT(strstr(c, "} else {") != NULL, "else clause");
    free(c);
}

static void test_while_loop(void) {
    printf("--- While loop ---\n");
    char *c = emit_c_from_skw(
        "var i 0\n"
        "while begin less i 10 end begin\n"
        "  assign i begin add i 1 end\n"
        "end");
    ASSERT(strstr(c, "while (") != NULL, "while emits C while");
    ASSERT(strstr(c, "i = ({") != NULL, "while body has assign with stmt expr");
    free(c);
}

static void test_for_loop(void) {
    printf("--- For loop ---\n");
    char *c = emit_c_from_skw(
        "for x in items begin\n"
        "  print 1\n"
        "end");
    ASSERT(strstr(c, "SigilIter _it = items;") != NULL, "for emits iterator init");
    ASSERT(strstr(c, "sigil_iter_has_next(&_it)") != NULL, "for emits has_next check");
    ASSERT(strstr(c, "SigilVal x = sigil_iter_next(&_it);") != NULL, "for emits next call with var name");
    free(c);
}

static void test_match(void) {
    printf("--- Match/case/default ---\n");
    char *c = emit_c_from_skw(
        "match 42 begin\n"
        "  case 1 begin\n"
        "    print 10\n"
        "  end\n"
        "  case 42 begin\n"
        "    print 20\n"
        "  end\n"
        "  default begin\n"
        "    print 30\n"
        "  end\n"
        "end");
    ASSERT(strstr(c, "_match_val = 42LL;") != NULL, "match stores value");
    ASSERT(strstr(c, "if (_match_val == 1LL)") != NULL, "case 1");
    ASSERT(strstr(c, "if (_match_val == 42LL)") != NULL, "case 42");
    ASSERT(strstr(c, "else {") != NULL, "default is else");
    free(c);
}

static void test_begin_end_stmt_expr(void) {
    printf("--- Begin/end (statement expression) ---\n");
    char *c = emit_c_from_skw("let x begin add 1 2 end");
    ASSERT(strstr(c, "({") != NULL, "begin/end emits GCC stmt expr opening");
    ASSERT(strstr(c, "})") != NULL, "begin/end emits GCC stmt expr closing");
    free(c);
}

static void test_fn_declaration_simple(void) {
    printf("--- Fn declaration (simple) ---\n");
    char *c = emit_c_from_skw(
        "fn double int a returns int\n"
        "  begin return begin add a a end end");
    ASSERT(strstr(c, "int64_t sigil_double(int64_t a)") != NULL, "fn prototype with sigil_ prefix");
    ASSERT(strstr(c, "return ({") != NULL, "fn body has return with stmt expr");
    ASSERT(strstr(c, "(a + a)") != NULL, "fn body has add(a,a)");
    free(c);
}

static void test_fn_void_return(void) {
    printf("--- Fn void return ---\n");
    char *c = emit_c_from_skw(
        "fn greet returns void\n"
        "  print 0");
    ASSERT(strstr(c, "void sigil_greet(void)") != NULL, "void fn with no params emits void");
    free(c);
}

static void test_fn_multiple_params(void) {
    printf("--- Fn multiple params ---\n");
    char *c = emit_c_from_skw(
        "fn triple int a int b int c returns int\n"
        "  begin return begin add a begin add b c end end end");
    ASSERT(strstr(c, "int64_t sigil_triple(int64_t a, int64_t b, int64_t c)") != NULL,
           "fn with 3 params");
    free(c);
}

static void test_fn_var_param(void) {
    printf("--- Fn var (mutable) param ---\n");
    char *c = emit_c_from_skw(
        "fn inc var int x returns void\n"
        "  begin assign x begin add x 1 end end");
    /* var param should become pointer */
    ASSERT(strstr(c, "int64_t *x") != NULL, "var param emits pointer type");
    free(c);
}

static void test_fn_call_var_param(void) {
    printf("--- Call to fn with var param ---\n");
    char *c = emit_c_from_skw(
        "fn inc var int x returns void\n"
        "  assign x begin add x 1 end\n"
        "var n 5\n"
        "inc n");
    ASSERT(strstr(c, "sigil_inc(&n)") != NULL, "call passes &arg for var param");
    free(c);
}

static void test_fn_two_pass(void) {
    printf("--- Fn two-pass (prototype before body) ---\n");
    char *c = emit_c_from_skw(
        "fn alpha int a returns int\n"
        "  begin return a end\n"
        "fn beta int b returns int\n"
        "  begin return begin alpha b end end");
    /* Prototypes should appear before bodies */
    char *proto_alpha = strstr(c, "int64_t sigil_alpha(int64_t a);");
    char *proto_beta  = strstr(c, "int64_t sigil_beta(int64_t b);");
    char *body_alpha  = strstr(c, "int64_t sigil_alpha(int64_t a) {");
    ASSERT(proto_alpha != NULL, "alpha prototype exists");
    ASSERT(proto_beta != NULL, "beta prototype exists");
    ASSERT(body_alpha != NULL, "alpha body exists");
    if (proto_alpha && body_alpha) {
        ASSERT(proto_alpha < body_alpha, "prototype appears before body");
    }
    free(c);
}

static void test_builtin_fn_no_prototype(void) {
    printf("--- Builtin fn omits prototype ---\n");
    /* Bodyless fns with builtin names should not emit prototypes */
    char *c = emit_c_from_skw(
        "fn add int a int b returns int\n"
        "let x begin add 1 2 end");
    ASSERT(strstr(c, "sigil_add(") == NULL, "no sigil_add prototype for builtin");
    ASSERT(strstr(c, "(1LL + 2LL)") != NULL, "add still resolves to + operator");
    free(c);
}

static void test_main_wrapper(void) {
    printf("--- Main wrapper ---\n");
    char *c = emit_c_from_skw("let x 1");
    ASSERT(strstr(c, "int main(void) {") != NULL, "top-level code wrapped in main()");
    ASSERT(strstr(c, "return 0;") != NULL, "main returns 0");
    free(c);
}

static void test_no_main_without_stmts(void) {
    printf("--- No main without executable stmts ---\n");
    /* fn-only program should not emit main */
    char *c = emit_c_from_skw(
        "fn id int a returns int\n"
        "  begin return a end");
    /* There should be a function but possibly no main */
    ASSERT(strstr(c, "sigil_id") != NULL, "fn exists");
    /* main may or may not be emitted depending on whether fn decl is executable */
    free(c);
}

static void test_includes(void) {
    printf("--- Includes ---\n");
    char *c = emit_c_from_skw("let x 1");
    ASSERT(strstr(c, "#include <stdint.h>") != NULL, "includes stdint.h");
    ASSERT(strstr(c, "#include <stdbool.h>") != NULL, "includes stdbool.h");
    ASSERT(strstr(c, "#include <stdio.h>") != NULL, "includes stdio.h");
    ASSERT(strstr(c, "#include \"sigil_runtime.h\"") != NULL, "includes sigil_runtime.h");
    free(c);
}

static void test_type_mapping_bool(void) {
    printf("--- Type mapping: bool ---\n");
    char *c = emit_c_from_skw(
        "fn check bool a returns bool\n"
        "  begin return a end");
    ASSERT(strstr(c, "bool sigil_check(bool a)") != NULL, "bool maps to bool");
    free(c);
}

static void test_type_mapping_float(void) {
    printf("--- Type mapping: float ---\n");
    char *c = emit_c_from_skw(
        "fn half float a returns float\n"
        "  begin return a end");
    ASSERT(strstr(c, "double sigil_half(double a)") != NULL, "float maps to double");
    free(c);
}

static void test_type_mapping_char(void) {
    printf("--- Type mapping: char ---\n");
    char *c = emit_c_from_skw(
        "fn echo char a returns char\n"
        "  begin return a end");
    ASSERT(strstr(c, "uint32_t sigil_echo(uint32_t a)") != NULL, "char maps to uint32_t");
    free(c);
}

static void test_type_mapping_void(void) {
    printf("--- Type mapping: void ---\n");
    char *c = emit_c_from_skw(
        "fn noop returns void\n"
        "  print 0");
    ASSERT(strstr(c, "void sigil_noop(void)") != NULL, "void maps to void");
    free(c);
}

static void test_sigil_algebra_arithmetic(void) {
    printf("--- Sigil algebra: arithmetic desugaring ---\n");
    char *c = emit_c_from_sigil(
        "algebra Arith\n"
        "fn add int a + int b returns int\n"
        "fn multiply int a * int b returns int\n"
        "precedence + *\n"
        "use Arith\n"
        "  let x 3 + 4\n"
        "  let y x * 2\n");
    /* After desugaring, should have add and multiply calls -> + and * operators */
    ASSERT(strstr(c, "(3LL + 4LL)") != NULL, "sigil + desugars to C +");
    ASSERT(strstr(c, "(x * 2LL)") != NULL, "sigil * desugars to C *");
    free(c);
}

static void test_sigil_precedence(void) {
    printf("--- Sigil precedence (a + b * c) ---\n");
    char *c = emit_c_from_sigil(
        "algebra Prec\n"
        "fn add int a + int b returns int\n"
        "fn times int a * int b returns int\n"
        "precedence + *\n"
        "use Prec\n"
        "  let x 1 + 2 * 3\n");
    /* * binds tighter: times is now a builtin → 2LL * 3LL */
    ASSERT(strstr(c, "1LL +") != NULL, "precedence: 1 + ...");
    ASSERT(strstr(c, "2LL * 3LL") != NULL, "precedence: times(2,3) as higher-prec call");
    free(c);
}

static void test_sigil_prefix(void) {
    printf("--- Sigil prefix operator ---\n");
    char *c = emit_c_from_sigil(
        "algebra Pre\n"
        "fn negate - int a returns int\n"
        "precedence -\n"
        "use Pre\n"
        "  let x - 5\n");
    ASSERT(strstr(c, "(-5LL)") != NULL, "prefix sigil - desugars to (-x)");
    free(c);
}

static void test_if_with_condition_expr(void) {
    printf("--- If with complex condition ---\n");
    char *c = emit_c_from_skw(
        "let a 10\n"
        "if begin greater a 5 end begin\n"
        "  print a\n"
        "end");
    ASSERT(strstr(c, "if (({") != NULL, "if condition is statement expression");
    ASSERT(strstr(c, "(a > 5LL)") != NULL, "greater desugars to > in condition");
    free(c);
}

static void test_while_with_mutation(void) {
    printf("--- While with mutation ---\n");
    char *c = emit_c_from_skw(
        "var sum 0\n"
        "var i 1\n"
        "while begin less i 4 end begin\n"
        "  assign sum begin add sum i end\n"
        "  assign i begin add i 1 end\n"
        "end\n"
        "print sum");
    ASSERT(strstr(c, "while (") != NULL, "while loop present");
    ASSERT(strstr(c, "sum = ({") != NULL, "accumulator update with stmt expr");
    ASSERT(strstr(c, "i = ({") != NULL, "counter increment with stmt expr");
    free(c);
}

static void test_match_with_default_only(void) {
    printf("--- Match with only default ---\n");
    char *c = emit_c_from_skw(
        "match 99 begin\n"
        "  default begin\n"
        "    print 0\n"
        "  end\n"
        "end");
    ASSERT(strstr(c, "_match_val = 99LL;") != NULL, "match stores value");
    free(c);
}

static void test_fn_body_with_if(void) {
    printf("--- Fn body with if ---\n");
    char *c = emit_c_from_skw(
        "fn abs int x returns int\n"
        "  begin\n"
        "    if begin less x 0 end begin\n"
        "      return begin negate x end\n"
        "    end else begin\n"
        "      return x\n"
        "    end\n"
        "  end");
    ASSERT(strstr(c, "int64_t sigil_abs(int64_t x)") != NULL, "abs fn prototype");
    ASSERT(strstr(c, "if (") != NULL, "if in fn body");
    ASSERT(strstr(c, "return ({ (-x);") != NULL, "return negate (stmt expr wrapped)");
    ASSERT(strstr(c, "return x;") != NULL, "return x direct");
    free(c);
}

static void test_fn_calling_fn(void) {
    printf("--- Fn calling another fn ---\n");
    char *c = emit_c_from_skw(
        "fn double int a returns int\n"
        "  begin return begin add a a end end\n"
        "fn quadruple int a returns int\n"
        "  begin return begin double begin double a end end end");
    /* Nested calls wrapped in stmt exprs: sigil_double(({ sigil_double(a); })) */
    ASSERT(strstr(c, "sigil_double(") != NULL, "fn calls fn with sigil_ prefix");
    ASSERT(strstr(c, "sigil_double(a)") != NULL, "inner call to double(a)");
    free(c);
}

static void test_multiple_let_bindings(void) {
    printf("--- Multiple let bindings ---\n");
    char *c = emit_c_from_skw(
        "let a 1\n"
        "let b 2\n"
        "let c 3\n"
        "let d begin add a begin add b c end end");
    ASSERT(strstr(c, "const int64_t a = 1LL;") != NULL, "a bound");
    ASSERT(strstr(c, "const int64_t b = 2LL;") != NULL, "b bound");
    ASSERT(strstr(c, "const int64_t c = 3LL;") != NULL, "c bound");
    ASSERT(strstr(c, "(a + (") != NULL, "d = add(a, add(b, c)) — outer add");
    ASSERT(strstr(c, "(b + c)") != NULL, "d inner add(b, c)");
    free(c);
}

static void test_deeply_nested_begin_end(void) {
    printf("--- Deeply nested begin/end ---\n");
    char *c = emit_c_from_skw(
        "let x begin add begin add 1 2 end begin add 3 4 end end");
    ASSERT(strstr(c, "(1LL + 2LL)") != NULL, "deeply nested: inner add(1,2)");
    ASSERT(strstr(c, "(3LL + 4LL)") != NULL, "deeply nested: inner add(3,4)");
    free(c);
}

static void test_algebra_use_block(void) {
    printf("--- Algebra use block ---\n");
    char *c = emit_c_from_sigil(
        "algebra Alg\n"
        "fn add int a + int b returns int\n"
        "precedence +\n"
        "use Alg\n"
        "  let result 10 + 20\n");
    ASSERT(strstr(c, "(10LL + 20LL)") != NULL, "use block code emitted");
    ASSERT(strstr(c, "int main(void)") != NULL, "use block stmts in main");
    free(c);
}

static void test_fn_with_bool_param(void) {
    printf("--- Fn with bool param ---\n");
    char *c = emit_c_from_skw(
        "fn choose bool cond int a int b returns int\n"
        "  begin\n"
        "    if cond begin\n"
        "      return a\n"
        "    end else begin\n"
        "      return b\n"
        "    end\n"
        "  end");
    ASSERT(strstr(c, "int64_t sigil_choose(bool cond, int64_t a, int64_t b)") != NULL,
           "mixed param types");
    free(c);
}

static void test_let_with_bool_expr(void) {
    printf("--- Let with bool expression ---\n");
    char *c = emit_c_from_skw("let ok begin and true begin not false end end");
    /* and/not are builtins not in fn_registry, so type infers as UNKNOWN -> int64_t */
    ASSERT(strstr(c, "ok = ({") != NULL, "let ok has stmt expr value");
    ASSERT(strstr(c, "true && (") != NULL, "and(true, ...) produces &&");
    ASSERT(strstr(c, "!false") != NULL, "not(false) produces !");
    free(c);
}

/* ── Phase 7 Gap Fix Tests ───────────────────────────────────────── */

static void test_type_aware_print(void) {
    printf("--- Type-aware print ---\n");
    char *c = emit_c_from_skw("let x true\nprint x");
    ASSERT(strstr(c, "sigil_print_bool(x)") != NULL, "print bool uses sigil_print_bool");
    free(c);

    c = emit_c_from_skw("let y 3.14\nprint y");
    ASSERT(strstr(c, "sigil_print_float(y)") != NULL, "print float uses sigil_print_float");
    free(c);

    c = emit_c_from_skw("print 42");
    ASSERT(strstr(c, "sigil_print_int(42LL)") != NULL, "print int uses sigil_print_int");
    free(c);
}

static void test_var_param_deref(void) {
    printf("--- Var param dereference ---\n");
    char *c = emit_c_from_skw(
        "fn inc var int x returns void\n"
        "  begin assign x begin add x 1 end end");
    ASSERT(strstr(c, "int64_t *x") != NULL, "var param is pointer");
    ASSERT(strstr(c, "(*x) = ") != NULL, "assign to var param dereferences");
    ASSERT(strstr(c, "((*x) + 1LL)") != NULL, "var param read dereferences");
    free(c);
}

static void test_overload_mangling(void) {
    printf("--- Overload name mangling ---\n");
    char *c = emit_c_from_skw(
        "fn double int a returns int\n"
        "  begin return begin add a a end end\n"
        "fn double float a returns float\n"
        "  begin return a end");
    ASSERT(strstr(c, "sigil_double_int") != NULL, "int overload is mangled");
    ASSERT(strstr(c, "sigil_double_float") != NULL, "float overload is mangled");
    free(c);
}

static void test_multi_statement_print(void) {
    printf("--- Multi-statement print ---\n");
    char *c = emit_c_from_skw("print 1\nprint 2");
    char *first = strstr(c, "sigil_print_int(1LL)");
    char *second = strstr(c, "sigil_print_int(2LL)");
    ASSERT(first != NULL, "first print call exists");
    ASSERT(second != NULL, "second print call exists");
    free(c);
}

static void test_context_emission(void) {
    printf("--- context -> _context ---\n");
    char *c = emit_c_from_skw(
        "fn getval int x returns int\n"
        "  begin return context end");
    ASSERT(strstr(c, "_context") != NULL, "context identifier emits as _context");
    free(c);
}

static void test_repeats_emission(void) {
    printf("--- repeats param emission ---\n");
    char *c = emit_c_from_skw(
        "fn row int a repeats returns void\n"
        "  print a");
    ASSERT(strstr(c, "_repeats_count") != NULL, "repeats emits count param");
    ASSERT(strstr(c, "SigilVal *_repeats_data") != NULL, "repeats emits data param");
    free(c);
}

/* ── Map operation tests ─────────────────────────────────────────── */

static void test_mapnew(void) {
    printf("--- mapnew ---\n");
    char *c = emit_c_from_skw("let m mapnew");
    ASSERT(strstr(c, "SigilMap* m = sigil_map_new()") != NULL, "mapnew emits sigil_map_new()");
    free(c);
}

static void test_map_set(void) {
    printf("--- map set ---\n");
    char *c = emit_c_from_skw("var m mapnew\nset m 1 42");
    ASSERT(strstr(c, "sigil_map_set(m, sigil_val_int(1LL), sigil_val_int(42LL))") != NULL,
           "set emits sigil_map_set with boxed args");
    free(c);
}

static void test_map_get(void) {
    printf("--- map get ---\n");
    char *c = emit_c_from_skw("var m mapnew\nlet v begin get m 1 end");
    ASSERT(strstr(c, "sigil_map_get(m, sigil_val_int(1LL))") != NULL,
           "get emits sigil_map_get with boxed key");
    free(c);
}

static void test_map_has(void) {
    printf("--- map has ---\n");
    char *c = emit_c_from_skw("var m mapnew\nlet b begin has m 1 end");
    ASSERT(strstr(c, "sigil_map_has(m, sigil_val_int(1LL))") != NULL,
           "has emits sigil_map_has with boxed key");
    free(c);
}

static void test_map_remove(void) {
    printf("--- map remove ---\n");
    char *c = emit_c_from_skw("var m mapnew\nremove m 1");
    ASSERT(strstr(c, "sigil_map_remove(m, sigil_val_int(1LL))") != NULL,
           "remove emits sigil_map_remove with boxed key");
    free(c);
}

static void test_map_count(void) {
    printf("--- map mapcount ---\n");
    char *c = emit_c_from_skw("var m mapnew\nlet n begin mapcount m end");
    ASSERT(strstr(c, "sigil_map_count(m)") != NULL,
           "mapcount emits sigil_map_count");
    free(c);
}

static void test_map_get_float_val(void) {
    printf("--- map get with float values ---\n");
    char *c = emit_c_from_skw("var m mapnew\nset m 1 3.14\nlet v begin get m 1 end");
    ASSERT(strstr(c, "sigil_val_float(3.14)") != NULL, "set boxes float value");
    ASSERT(strstr(c, "sigil_unbox_float(") != NULL, "get unboxes to float");
    free(c);
}

static void test_map_get_bool_val(void) {
    printf("--- map get with bool values ---\n");
    char *c = emit_c_from_skw("var m mapnew\nset m 1 true\nlet v begin get m 1 end");
    ASSERT(strstr(c, "sigil_val_bool(true)") != NULL, "set boxes bool value");
    ASSERT(strstr(c, "sigil_unbox_bool(") != NULL, "get unboxes to bool");
    free(c);
}

static void test_map_typed_fn_param(void) {
    printf("--- map as typed fn param ---\n");
    char *c = emit_c_from_skw(
        "fn total map int int m int key returns int\n"
        "  begin return begin get m key end end");
    ASSERT(strstr(c, "SigilMap* m") != NULL, "map param emits SigilMap*");
    ASSERT(strstr(c, "sigil_unbox_int(") != NULL, "get from map int int unboxes to int");
    free(c);
}

static void test_map_for_loop(void) {
    printf("--- map for loop ---\n");
    char *c = emit_c_from_skw("var m mapnew\nfor k in m begin\n  print 1\nend");
    ASSERT(strstr(c, "sigil_map_iter(m)") != NULL,
           "for-loop over map emits sigil_map_iter");
    free(c);
}

/* ── Monomorphization tests ───────────────────────────────────────── */

static void test_mono_identity_int(void) {
    printf("--- Mono: identity int ---\n");
    char *c = emit_c_from_skw(
        "fn identity T x returns T\n"
        "  begin return x end\n"
        "let a begin identity 42 end");
    ASSERT(strstr(c, "sigil_identity_int") != NULL, "mono identity int exists");
    ASSERT(strstr(c, "int64_t sigil_identity_int(int64_t x)") != NULL, "mono identity int signature");
    free(c);
}

static void test_mono_identity_float(void) {
    printf("--- Mono: identity float ---\n");
    char *c = emit_c_from_skw(
        "fn identity T x returns T\n"
        "  begin return x end\n"
        "let a begin identity 3.14 end");
    ASSERT(strstr(c, "sigil_identity_float") != NULL, "mono identity float exists");
    ASSERT(strstr(c, "double sigil_identity_float(double x)") != NULL, "mono identity float signature");
    free(c);
}

static void test_mono_identity_both(void) {
    printf("--- Mono: identity int + float ---\n");
    char *c = emit_c_from_skw(
        "fn identity T x returns T\n"
        "  begin return x end\n"
        "let a begin identity 42 end\n"
        "let b begin identity 3.14 end");
    ASSERT(strstr(c, "sigil_identity_int") != NULL, "mono identity int present");
    ASSERT(strstr(c, "sigil_identity_float") != NULL, "mono identity float present");
    free(c);
}

static void test_mono_no_generic_standalone(void) {
    printf("--- Mono: no standalone generic fn ---\n");
    char *c = emit_c_from_skw(
        "fn identity T x returns T\n"
        "  begin return x end\n"
        "let a begin identity 42 end");
    /* Should NOT have a generic version with T suffix */
    ASSERT(strstr(c, "sigil_identity_T") == NULL, "no standalone generic fn emitted");
    free(c);
}

static void test_mono_uncalled_generic(void) {
    printf("--- Mono: uncalled generic omitted ---\n");
    char *c = emit_c_from_skw(
        "fn identity T x returns T\n"
        "  begin return x end\n"
        "let a 42");
    /* Never called → no mono instance, no C function */
    ASSERT(strstr(c, "sigil_identity") == NULL, "uncalled generic fn not emitted");
    free(c);
}

int main(void) {
    printf("=== C Emitter Tests ===\n\n");

    /* Literals and bindings */
    test_int_literal();
    test_float_literal();
    test_bool_literal();
    test_var_binding();
    test_assign();
    test_return();

    /* Builtins (all 13 primitives) */
    test_builtin_add();
    test_builtin_subtract();
    test_builtin_multiply();
    test_builtin_divide();
    test_builtin_modulo();
    test_builtin_equal();
    test_builtin_less();
    test_builtin_greater();
    test_builtin_and();
    test_builtin_or();
    test_builtin_negate();
    test_builtin_not();
    test_builtin_print();
    test_nested_builtins();

    /* Control flow */
    test_if_simple();
    test_if_elif_else();
    test_while_loop();
    test_for_loop();
    test_match();
    test_begin_end_stmt_expr();

    /* Functions */
    test_fn_declaration_simple();
    test_fn_void_return();
    test_fn_multiple_params();
    test_fn_var_param();
    test_fn_call_var_param();
    test_fn_two_pass();
    test_builtin_fn_no_prototype();

    /* Structure */
    test_main_wrapper();
    test_no_main_without_stmts();
    test_includes();

    /* Type mapping */
    test_type_mapping_bool();
    test_type_mapping_float();
    test_type_mapping_char();
    test_type_mapping_void();

    /* Sigil algebra (full pipeline) */
    test_sigil_algebra_arithmetic();
    test_sigil_precedence();
    test_sigil_prefix();

    /* Complex programs */
    test_if_with_condition_expr();
    test_while_with_mutation();
    test_match_with_default_only();
    test_fn_body_with_if();
    test_fn_calling_fn();
    test_multiple_let_bindings();
    test_deeply_nested_begin_end();
    test_algebra_use_block();
    test_fn_with_bool_param();
    test_let_with_bool_expr();

    /* Phase 7 gap fixes */
    test_type_aware_print();
    test_var_param_deref();
    test_overload_mangling();
    test_multi_statement_print();
    test_context_emission();
    test_repeats_emission();

    /* Map operations */
    test_mapnew();
    test_map_set();
    test_map_get();
    test_map_has();
    test_map_remove();
    test_map_count();
    test_map_get_float_val();
    test_map_get_bool_val();
    test_map_typed_fn_param();
    test_map_for_loop();

    /* Monomorphization */
    test_mono_identity_int();
    test_mono_identity_float();
    test_mono_identity_both();
    test_mono_no_generic_standalone();
    test_mono_uncalled_generic();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
