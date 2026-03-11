#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/sigil_thunk.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
    } else { \
        tests_passed++; \
    } \
} while (0)

/* ── Test Arena ──────────────────────────────────────────────────── */

static void test_arena_init_reset(void) {
    ThunkArena a;
    thunk_arena_init(&a, 4096);
    ASSERT(a.base != NULL, "arena base allocated");
    ASSERT(a.capacity == 4096, "arena capacity");
    ASSERT(a.used == 0, "arena used starts at 0");

    SigilThunk *t = thunk_alloc(&a, 0, 2);
    ASSERT(t != NULL, "thunk allocated");
    ASSERT(a.used > 0, "arena used increased");

    thunk_arena_reset(&a);
    ASSERT(a.used == 0, "arena reset to 0");

    thunk_arena_destroy(&a);
    ASSERT(a.base == NULL, "arena destroyed");
}

/* ── Test Completed Thunk ────────────────────────────────────────── */

static void test_completed_thunk(void) {
    ThunkArena a;
    thunk_arena_init(&a, 4096);

    SigilThunk *t = thunk_alloc_completed(&a, sigil_val_int(42));
    ASSERT(t->state == THUNK_COMPLETED, "completed thunk state");
    ASSERT(t->value.kind == SIGIL_VAL_INT, "completed thunk value kind");
    ASSERT(t->value.i == 42, "completed thunk value");

    SigilVal v = thunk_force(t, &a);
    ASSERT(v.i == 42, "force on completed returns value");

    thunk_arena_destroy(&a);
}

/* ── Test Pending Thunk (no evaluator) ───────────────────────────── */

static void test_pending_no_evaluator(void) {
    ThunkArena a;
    thunk_arena_init(&a, 4096);

    /* No evaluators registered */
    ThunkEvaluator *saved = sigil_evaluators;
    int saved_count = sigil_thunk_fn_count;
    sigil_evaluators = NULL;
    sigil_thunk_fn_count = 0;

    SigilThunk *t = thunk_alloc(&a, 0, 0);
    SigilVal v = thunk_force(t, &a);
    ASSERT(v.kind == SIGIL_VAL_INT, "no evaluator returns int");
    ASSERT(v.i == 0, "no evaluator returns 0");
    ASSERT(t->state == THUNK_COMPLETED, "state set to completed even without evaluator");

    sigil_evaluators = saved;
    sigil_thunk_fn_count = saved_count;
    thunk_arena_destroy(&a);
}

/* ── Test Cycle Detection ────────────────────────────────────────── */

static SigilVal cycle_evaluator(SigilThunk *t, ThunkArena *arena) {
    /* This tries to force itself — should trigger cycle detection */
    return thunk_force(t, arena);
}

static void test_cycle_detection(void) {
    ThunkArena a;
    thunk_arena_init(&a, 4096);

    ThunkEvaluator evals[1] = { cycle_evaluator };
    ThunkEvaluator *saved = sigil_evaluators;
    int saved_count = sigil_thunk_fn_count;
    sigil_evaluators = evals;
    sigil_thunk_fn_count = 1;

    SigilThunk *t = thunk_alloc(&a, 0, 0);
    /* This should detect cycle (IN_PROGRESS) and return 0 */
    SigilVal v = thunk_force(t, &a);
    ASSERT(v.kind == SIGIL_VAL_INT, "cycle returns int");
    ASSERT(v.i == 0, "cycle returns 0");

    sigil_evaluators = saved;
    sigil_thunk_fn_count = saved_count;
    thunk_arena_destroy(&a);
}

/* ── Test Basic Evaluation ───────────────────────────────────────── */

static SigilVal add_evaluator(SigilThunk *t, ThunkArena *arena) {
    /* Force two child thunks and add them */
    SigilVal a = thunk_force(t->args[0], arena);
    SigilVal b = thunk_force(t->args[1], arena);
    return sigil_val_int(a.i + b.i);
}

static void test_basic_evaluation(void) {
    ThunkArena a;
    thunk_arena_init(&a, 4096);

    ThunkEvaluator evals[1] = { add_evaluator };
    ThunkEvaluator *saved = sigil_evaluators;
    int saved_count = sigil_thunk_fn_count;
    sigil_evaluators = evals;
    sigil_thunk_fn_count = 1;

    /* Create: add(3, 4) */
    SigilThunk *t = thunk_alloc(&a, 0, 2);
    t->args[0] = thunk_alloc_completed(&a, sigil_val_int(3));
    t->args[1] = thunk_alloc_completed(&a, sigil_val_int(4));

    SigilVal v = thunk_force(t, &a);
    ASSERT(v.i == 7, "3 + 4 = 7");

    /* Force again — should return memoized */
    SigilVal v2 = thunk_force(t, &a);
    ASSERT(v2.i == 7, "memoized result");

    sigil_evaluators = saved;
    sigil_thunk_fn_count = saved_count;
    thunk_arena_destroy(&a);
}

/* ── Test Nested Evaluation ──────────────────────────────────────── */

static void test_nested_evaluation(void) {
    ThunkArena a;
    thunk_arena_init(&a, 4096);

    ThunkEvaluator evals[1] = { add_evaluator };
    ThunkEvaluator *saved = sigil_evaluators;
    int saved_count = sigil_thunk_fn_count;
    sigil_evaluators = evals;
    sigil_thunk_fn_count = 1;

    /* Create: add(add(1, 2), add(3, 4)) = 10 */
    SigilThunk *inner1 = thunk_alloc(&a, 0, 2);
    inner1->args[0] = thunk_alloc_completed(&a, sigil_val_int(1));
    inner1->args[1] = thunk_alloc_completed(&a, sigil_val_int(2));

    SigilThunk *inner2 = thunk_alloc(&a, 0, 2);
    inner2->args[0] = thunk_alloc_completed(&a, sigil_val_int(3));
    inner2->args[1] = thunk_alloc_completed(&a, sigil_val_int(4));

    SigilThunk *outer = thunk_alloc(&a, 0, 2);
    outer->args[0] = inner1;
    outer->args[1] = inner2;

    SigilVal v = thunk_force(outer, &a);
    ASSERT(v.i == 10, "add(add(1,2), add(3,4)) = 10");

    sigil_evaluators = saved;
    sigil_thunk_fn_count = saved_count;
    thunk_arena_destroy(&a);
}

/* ── Test sigil_force_val ────────────────────────────────────────── */

static void test_force_val(void) {
    ThunkArena a;
    thunk_arena_init(&a, 4096);

    /* Non-thunk value passes through */
    SigilVal plain = sigil_val_int(99);
    SigilVal r = sigil_force_val(plain, &a);
    ASSERT(r.i == 99, "force_val on non-thunk");

    /* Thunk value gets forced */
    SigilThunk *t = thunk_alloc_completed(&a, sigil_val_int(55));
    SigilVal boxed = sigil_val_thunk(t);
    SigilVal r2 = sigil_force_val(boxed, &a);
    ASSERT(r2.i == 55, "force_val on thunk");

    thunk_arena_destroy(&a);
}

/* ── Test Arena Growth ───────────────────────────────────────────── */

static void test_arena_growth(void) {
    ThunkArena a;
    thunk_arena_init(&a, 64);  /* Tiny initial size */

    /* Allocate more than fits */
    for (int i = 0; i < 100; i++) {
        SigilThunk *t = thunk_alloc_completed(&a, sigil_val_int(i));
        ASSERT(t != NULL, "allocation after growth");
        ASSERT(t->value.i == i, "value preserved after growth");
    }

    thunk_arena_destroy(&a);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    test_arena_init_reset();
    test_completed_thunk();
    test_pending_no_evaluator();
    test_cycle_detection();
    test_basic_evaluation();
    test_nested_evaluation();
    test_force_val();
    test_arena_growth();

    printf("test_thunk: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
