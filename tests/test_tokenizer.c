#include "common.h"
#include "unicode.h"
#include "tokenizer.h"
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

static TokenList tokenize(const char *source) {
    Arena arena;
    arena_init(&arena);
    InternTable intern_tab;
    intern_init(&intern_tab);
    ErrorList errors;
    error_list_init(&errors, &arena);
    Tokenizer t;
    tokenizer_init(&t, source, "<test>", &intern_tab, &errors, NULL);
    return tokenize_all(&t);
}

static void test_charclass_boundaries(void) {
    printf("--- Character class boundaries ---\n");

    /* a+b -> a + b */
    {
        Arena arena; arena_init(&arena);
        InternTable it; intern_init(&it);
        ErrorList err; error_list_init(&err, &arena);
        Tokenizer t;
        tokenizer_init(&t, "a+b", "<test>", &it, &err, NULL);
        Token t1 = tokenizer_next(&t);
        Token t2 = tokenizer_next(&t);
        Token t3 = tokenizer_next(&t);
        ASSERT(t1.kind == TOK_IDENT && strcmp(t1.text, "a") == 0, "a+b: first token is 'a'");
        ASSERT(t2.kind == TOK_SIGIL && strcmp(t2.text, "+") == 0, "a+b: second token is '+'");
        ASSERT(t3.kind == TOK_IDENT && strcmp(t3.text, "b") == 0, "a+b: third token is 'b'");
        intern_free(&it); arena_free(&arena);
    }

    /* Unicode: 𝑑x -> 𝑑 x (Unicode to ASCII boundary) */
    {
        Arena arena; arena_init(&arena);
        InternTable it; intern_init(&it);
        ErrorList err; error_list_init(&err, &arena);
        Tokenizer t;
        tokenizer_init(&t, "\xF0\x9D\x91\x91x", "<test>", &it, &err, NULL);
        Token t1 = tokenizer_next(&t);
        Token t2 = tokenizer_next(&t);
        ASSERT(t1.kind == TOK_SIGIL, "𝑑x: first token is sigil (𝑑)");
        ASSERT(t2.kind == TOK_IDENT && strcmp(t2.text, "x") == 0, "𝑑x: second token is 'x'");
        intern_free(&it); arena_free(&arena);
    }

    /* 𝑑y/𝑑x -> 𝑑 y / 𝑑 x */
    {
        Arena arena; arena_init(&arena);
        InternTable it; intern_init(&it);
        ErrorList err; error_list_init(&err, &arena);
        Tokenizer t;
        tokenizer_init(&t, "\xF0\x9D\x91\x91y/\xF0\x9D\x91\x91x", "<test>", &it, &err, NULL);
        Token t1 = tokenizer_next(&t);
        Token t2 = tokenizer_next(&t);
        Token t3 = tokenizer_next(&t);
        Token t4 = tokenizer_next(&t);
        Token t5 = tokenizer_next(&t);
        ASSERT(t1.kind == TOK_SIGIL, "𝑑y/𝑑x: t1 is sigil (𝑑)");
        ASSERT(t2.kind == TOK_IDENT && strcmp(t2.text, "y") == 0, "𝑑y/𝑑x: t2 is 'y'");
        ASSERT(t3.kind == TOK_SIGIL && strcmp(t3.text, "/") == 0, "𝑑y/𝑑x: t3 is '/'");
        ASSERT(t4.kind == TOK_SIGIL, "𝑑y/𝑑x: t4 is sigil (𝑑)");
        ASSERT(t5.kind == TOK_IDENT && strcmp(t5.text, "x") == 0, "𝑑y/𝑑x: t5 is 'x'");
        intern_free(&it); arena_free(&arena);
    }
}

static void test_keywords_and_idents(void) {
    printf("--- Keywords and identifiers ---\n");
    Arena arena; arena_init(&arena);
    InternTable it; intern_init(&it);
    ErrorList err; error_list_init(&err, &arena);
    Tokenizer t;
    tokenizer_init(&t, "fn add int a returns int", "<test>", &it, &err, NULL);

    Token t1 = tokenizer_next(&t);
    Token t2 = tokenizer_next(&t);
    Token t3 = tokenizer_next(&t);
    Token t4 = tokenizer_next(&t);
    Token t5 = tokenizer_next(&t);
    Token t6 = tokenizer_next(&t);

    ASSERT(t1.kind == TOK_KEYWORD && strcmp(t1.text, "fn") == 0, "fn is keyword");
    ASSERT(t2.kind == TOK_KEYWORD && strcmp(t2.text, "add") == 0, "add is keyword");
    ASSERT(t3.kind == TOK_KEYWORD && strcmp(t3.text, "int") == 0, "int is keyword");
    ASSERT(t4.kind == TOK_IDENT && strcmp(t4.text, "a") == 0, "a is ident");
    ASSERT(t5.kind == TOK_KEYWORD && strcmp(t5.text, "returns") == 0, "returns is keyword");
    ASSERT(t6.kind == TOK_KEYWORD && strcmp(t6.text, "int") == 0, "int is keyword");
    intern_free(&it); arena_free(&arena);
}

static void test_numbers(void) {
    printf("--- Number literals ---\n");
    Arena arena; arena_init(&arena);
    InternTable it; intern_init(&it);
    ErrorList err; error_list_init(&err, &arena);
    Tokenizer t;
    tokenizer_init(&t, "42 3.14 0", "<test>", &it, &err, NULL);

    Token t1 = tokenizer_next(&t);
    Token t2 = tokenizer_next(&t);
    Token t3 = tokenizer_next(&t);

    ASSERT(t1.kind == TOK_INT_LIT && t1.int_val == 42, "42 is int 42");
    ASSERT(t2.kind == TOK_FLOAT_LIT && t2.float_val == 3.14, "3.14 is float 3.14");
    ASSERT(t3.kind == TOK_INT_LIT && t3.int_val == 0, "0 is int 0");
    intern_free(&it); arena_free(&arena);
}

static void test_begin_end(void) {
    printf("--- Begin/end tokens ---\n");
    Arena arena; arena_init(&arena);
    InternTable it; intern_init(&it);
    ErrorList err; error_list_init(&err, &arena);
    Tokenizer t;
    tokenizer_init(&t, "begin end", "<test>", &it, &err, NULL);

    Token t1 = tokenizer_next(&t);
    Token t2 = tokenizer_next(&t);

    ASSERT(t1.kind == TOK_BEGIN, "begin is TOK_BEGIN");
    ASSERT(t2.kind == TOK_END, "end is TOK_END");
    intern_free(&it); arena_free(&arena);
}

static void test_compound_sigils(void) {
    printf("--- Compound sigils ---\n");
    Arena arena; arena_init(&arena);
    InternTable it; intern_init(&it);
    ErrorList err; error_list_init(&err, &arena);
    CompoundSigilSet cs;
    compound_sigil_set_init(&cs);
    compound_sigil_set_add(&cs, &it, "++");
    compound_sigil_set_add(&cs, &it, "->");

    Tokenizer t;
    tokenizer_init(&t, "++a->b", "<test>", &it, &err, &cs);

    Token t1 = tokenizer_next(&t);
    Token t2 = tokenizer_next(&t);
    Token t3 = tokenizer_next(&t);
    Token t4 = tokenizer_next(&t);

    ASSERT(t1.kind == TOK_SIGIL && strcmp(t1.text, "++") == 0, "++ is compound sigil");
    ASSERT(t2.kind == TOK_IDENT && strcmp(t2.text, "a") == 0, "a is ident");
    ASSERT(t3.kind == TOK_SIGIL && strcmp(t3.text, "->") == 0, "-> is compound sigil");
    ASSERT(t4.kind == TOK_IDENT && strcmp(t4.text, "b") == 0, "b is ident");

    compound_sigil_set_free(&cs);
    intern_free(&it); arena_free(&arena);
}

static void test_comments(void) {
    printf("--- Comments ---\n");
    Arena arena; arena_init(&arena);
    InternTable it; intern_init(&it);
    ErrorList err; error_list_init(&err, &arena);
    Tokenizer t;
    tokenizer_init(&t, "a rem this is a comment\nb", "<test>", &it, &err, NULL);

    Token t1 = tokenizer_next(&t);
    Token t2 = tokenizer_next(&t);
    Token t3 = tokenizer_next(&t);

    ASSERT(t1.kind == TOK_IDENT && strcmp(t1.text, "a") == 0, "token before comment");
    ASSERT(t2.kind == TOK_NEWLINE, "newline after comment");
    ASSERT(t3.kind == TOK_IDENT && strcmp(t3.text, "b") == 0, "token after comment");
    intern_free(&it); arena_free(&arena);
}

int main(void) {
    printf("=== Tokenizer Tests ===\n\n");

    test_charclass_boundaries();
    test_keywords_and_idents();
    test_numbers();
    test_begin_end();
    test_compound_sigils();
    test_comments();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
