#include "sigil_runtime.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    else { tests_passed++; } \
} while(0)

static SigilVal make_int(int64_t v) {
    return (SigilVal){.kind = SIGIL_VAL_INT, .i = v};
}

static SigilVal make_bool(bool v) {
    return (SigilVal){.kind = SIGIL_VAL_BOOL, .b = v};
}

static SigilVal make_float(double v) {
    return (SigilVal){.kind = SIGIL_VAL_FLOAT, .f = v};
}

static SigilVal make_char(uint32_t v) {
    return (SigilVal){.kind = SIGIL_VAL_CHAR, .c = v};
}

/* ── Map basic operations ────────────────────────────────────────── */

static void test_map_new(void) {
    printf("--- Map: new ---\n");
    SigilMap *m = sigil_map_new();
    ASSERT(m != NULL, "map created");
    ASSERT(sigil_map_count(m) == 0, "empty map count is 0");
    ASSERT(m->ref_count == 1, "initial ref_count is 1");
    sigil_map_release(m);
}

static void test_map_set_get(void) {
    printf("--- Map: set/get ---\n");
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, make_int(1), make_int(100));
    sigil_map_set(m, make_int(2), make_int(200));
    ASSERT(sigil_map_count(m) == 2, "count is 2 after two inserts");

    SigilVal v1 = sigil_map_get(m, make_int(1));
    ASSERT(v1.kind == SIGIL_VAL_INT && v1.i == 100, "get key 1 returns 100");

    SigilVal v2 = sigil_map_get(m, make_int(2));
    ASSERT(v2.kind == SIGIL_VAL_INT && v2.i == 200, "get key 2 returns 200");

    sigil_map_release(m);
}

static void test_map_overwrite(void) {
    printf("--- Map: overwrite ---\n");
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, make_int(1), make_int(10));
    sigil_map_set(m, make_int(1), make_int(20));
    ASSERT(sigil_map_count(m) == 1, "overwrite doesn't increase count");
    SigilVal v = sigil_map_get(m, make_int(1));
    ASSERT(v.i == 20, "overwrite updates value");
    sigil_map_release(m);
}

static void test_map_has(void) {
    printf("--- Map: has ---\n");
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, make_int(42), make_int(1));
    ASSERT(sigil_map_has(m, make_int(42)) == true, "has existing key");
    ASSERT(sigil_map_has(m, make_int(99)) == false, "doesn't have missing key");
    sigil_map_release(m);
}

static void test_map_remove(void) {
    printf("--- Map: remove ---\n");
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, make_int(1), make_int(10));
    sigil_map_set(m, make_int(2), make_int(20));
    sigil_map_set(m, make_int(3), make_int(30));
    ASSERT(sigil_map_count(m) == 3, "count is 3");

    sigil_map_remove(m, make_int(2));
    ASSERT(sigil_map_count(m) == 2, "count is 2 after remove");
    ASSERT(sigil_map_has(m, make_int(2)) == false, "removed key gone");
    ASSERT(sigil_map_has(m, make_int(1)) == true, "other keys still present (1)");
    ASSERT(sigil_map_has(m, make_int(3)) == true, "other keys still present (3)");

    sigil_map_release(m);
}

static void test_map_many_entries(void) {
    printf("--- Map: many entries (resize) ---\n");
    SigilMap *m = sigil_map_new();
    for (int i = 0; i < 100; i++) {
        sigil_map_set(m, make_int(i), make_int(i * i));
    }
    ASSERT(sigil_map_count(m) == 100, "100 entries inserted");

    /* Verify all entries */
    bool all_correct = true;
    for (int i = 0; i < 100; i++) {
        SigilVal v = sigil_map_get(m, make_int(i));
        if (v.i != i * i) { all_correct = false; break; }
    }
    ASSERT(all_correct, "all 100 entries have correct values after resizing");

    sigil_map_release(m);
}

static void test_map_mixed_key_types(void) {
    printf("--- Map: mixed key types ---\n");
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, make_int(1), make_int(10));
    sigil_map_set(m, make_bool(true), make_int(20));
    sigil_map_set(m, make_float(3.14), make_int(30));
    sigil_map_set(m, make_char('A'), make_int(40));
    ASSERT(sigil_map_count(m) == 4, "4 entries with different key types");

    ASSERT(sigil_map_get(m, make_int(1)).i == 10, "int key");
    ASSERT(sigil_map_get(m, make_bool(true)).i == 20, "bool key");
    ASSERT(sigil_map_get(m, make_float(3.14)).i == 30, "float key");
    ASSERT(sigil_map_get(m, make_char('A')).i == 40, "char key");

    sigil_map_release(m);
}

/* ── Reference counting ──────────────────────────────────────────── */

static void test_map_refcount(void) {
    printf("--- Map: ref counting ---\n");
    SigilMap *m = sigil_map_new();
    ASSERT(m->ref_count == 1, "starts at 1");
    sigil_map_retain(m);
    ASSERT(m->ref_count == 2, "retain increments");
    sigil_map_release(m);
    ASSERT(m->ref_count == 1, "release decrements");
    sigil_map_release(m);
    /* m is freed — no crash means success */
    ASSERT(true, "double release doesn't crash");
}

/* ── Iterator ────────────────────────────────────────────────────── */

static void test_iter_empty_map(void) {
    printf("--- Iter: empty map ---\n");
    SigilMap *m = sigil_map_new();
    SigilIter it = sigil_map_iter(m);
    ASSERT(sigil_iter_has_next(&it) == false, "empty map has no next");
    sigil_map_release(m);
}

static void test_iter_all_keys(void) {
    printf("--- Iter: visits all keys ---\n");
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, make_int(10), make_int(1));
    sigil_map_set(m, make_int(20), make_int(2));
    sigil_map_set(m, make_int(30), make_int(3));

    SigilIter it = sigil_map_iter(m);
    int count = 0;
    int64_t sum = 0;
    while (sigil_iter_has_next(&it)) {
        SigilVal key = sigil_iter_next(&it);
        sum += key.i;
        count++;
    }
    ASSERT(count == 3, "iterated 3 keys");
    ASSERT(sum == 60, "sum of keys is 10+20+30=60");

    sigil_map_release(m);
}

static void test_map_null_safe(void) {
    printf("--- Map: null safety ---\n");
    ASSERT(sigil_map_count(NULL) == 0, "count of NULL map is 0");
    sigil_map_release(NULL); /* should not crash */
    ASSERT(true, "release NULL doesn't crash");
}

int main(void) {
    printf("=== Runtime Tests ===\n\n");

    test_map_new();
    test_map_set_get();
    test_map_overwrite();
    test_map_has();
    test_map_remove();
    test_map_many_entries();
    test_map_mixed_key_types();
    test_map_refcount();
    test_iter_empty_map();
    test_iter_all_keys();
    test_map_null_safe();

    /* Extended map operations */
    printf("--- Map: copy ---\n");
    {
        SigilMap *m = sigil_map_new();
        sigil_map_set(m, make_int(1), make_int(42));
        sigil_map_set(m, make_int(2), make_int(99));
        SigilMap *m2 = sigil_map_copy(m);
        ASSERT(sigil_map_count(m2) == 2, "copy has 2 entries");
        ASSERT(sigil_map_get(m2, make_int(1)).i == 42, "copy preserves values");
        /* Modify copy, original unchanged */
        sigil_map_set(m2, make_int(1), make_int(0));
        ASSERT(sigil_map_get(m, make_int(1)).i == 42, "original unchanged after copy modified");
        sigil_map_release(m);
        sigil_map_release(m2);
    }

    printf("--- Map: append ---\n");
    {
        SigilMap *m = sigil_map_new();
        sigil_map_append(m, make_int(10));
        sigil_map_append(m, make_int(20));
        sigil_map_append(m, make_int(30));
        ASSERT(sigil_map_count(m) == 3, "append creates 3 entries");
        ASSERT(sigil_map_get(m, make_int(0)).i == 10, "append[0]=10");
        ASSERT(sigil_map_get(m, make_int(1)).i == 20, "append[1]=20");
        ASSERT(sigil_map_get(m, make_int(2)).i == 30, "append[2]=30");
        sigil_map_release(m);
    }

    printf("--- Map: keys ---\n");
    {
        SigilMap *m = sigil_map_new();
        sigil_map_set(m, make_int(10), make_int(1));
        sigil_map_set(m, make_int(20), make_int(2));
        SigilMap *k = sigil_map_keys(m);
        ASSERT(sigil_map_count(k) == 2, "keys returns 2 entries");
        sigil_map_release(m);
        sigil_map_release(k);
    }

    printf("--- Map: values ---\n");
    {
        SigilMap *m = sigil_map_new();
        sigil_map_set(m, make_int(10), make_int(100));
        sigil_map_set(m, make_int(20), make_int(200));
        SigilMap *v = sigil_map_values(m);
        ASSERT(sigil_map_count(v) == 2, "values returns 2 entries");
        sigil_map_release(m);
        sigil_map_release(v);
    }

    printf("--- String: concat ---\n");
    {
        SigilMap *a = sigil_string_from_utf8("hello ", 6);
        SigilMap *b = sigil_string_from_utf8("world", 5);
        SigilMap *c = sigil_string_concat(a, b);
        ASSERT(sigil_map_count(c) == 11, "concat length is 11");
        ASSERT(sigil_map_get(c, make_int(0)).c == 'h', "concat[0]='h'");
        ASSERT(sigil_map_get(c, make_int(6)).c == 'w', "concat[6]='w'");
        sigil_map_release(a);
        sigil_map_release(b);
        sigil_map_release(c);
    }

    printf("--- Type: to_int ---\n");
    {
        ASSERT(sigil_to_int(3.7) == 3, "to_int(3.7) == 3");
        ASSERT(sigil_to_int(-2.9) == -2, "to_int(-2.9) == -2");
    }

    printf("--- Type: to_float ---\n");
    {
        ASSERT(sigil_to_float(42) == 42.0, "to_float(42) == 42.0");
    }

    printf("--- Type: int_to_string ---\n");
    {
        SigilMap *s = sigil_int_to_string(42);
        ASSERT(sigil_map_count(s) == 2, "int_to_string(42) has 2 chars");
        ASSERT(sigil_map_get(s, make_int(0)).c == '4', "first char is '4'");
        ASSERT(sigil_map_get(s, make_int(1)).c == '2', "second char is '2'");
        sigil_map_release(s);
    }

    printf("--- Type: bool_to_string ---\n");
    {
        SigilMap *s = sigil_bool_to_string(true);
        ASSERT(sigil_map_count(s) == 4, "bool_to_string(true) has 4 chars");
        sigil_map_release(s);
    }

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
