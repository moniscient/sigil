/*
 * test_validation.c — Thunk system validation harness
 *
 * Tests: factorial(20), fib(15), sum(1..100), trivial add(3,4)
 * Verifies classification bins, execution results, cross-executor consistency.
 */

#include "sigil_thunk.h"
#include "sigil_expander.h"
#include "sigil_classifier.h"
#include "sigil_executor.h"
#include "sigil_hardware.h"
#include "sigil_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

/* External executor declarations */
extern SigilVal execute_seq_obligate(SigilThunk *root, ThunkArena *arena);
extern SigilVal execute_seq_optimal(SigilThunk *root, ThunkArena *arena);
extern SigilVal execute_coro(SigilThunk *root, ThunkArena *arena, int core_count);
extern SigilVal execute_thread(SigilThunk *root, ThunkArena *arena, int core_count);
extern SigilVal execute_gpu(SigilThunk *root, ThunkArena *arena);

/* ── Function IDs ────────────────────────────────────────────────── */

enum {
    FUNC_FACTORIAL = 0,
    FUNC_FIB       = 1,
    FUNC_SUM       = 2,
    FUNC_ADD       = 3,
    FUNC_COUNT     = 4
};

/* ── Plain C implementations (no thunks) ─────────────────────────── */

static int64_t factorial_plain(int64_t n) {
    if (n <= 1) return 1;
    return n * factorial_plain(n - 1);
}

static int64_t fib_plain(int64_t n) {
    if (n < 2) return n;
    return fib_plain(n - 1) + fib_plain(n - 2);
}

static int64_t sum_plain(int64_t n) {
    int64_t s = 0;
    for (int64_t i = 1; i <= n; i++) s += i;
    return s;
}

/* ── Constructors (build thunk graph for classification) ─────────── */

static SigilThunk* factorial_ctor(ThunkArena *arena, SigilThunk **args) {
    if (!args || !args[0]) return NULL;
    int64_t n = thunk_force(args[0], arena).i;
    if (n <= 1) return thunk_alloc_completed(arena, sigil_val_int(1));
    SigilThunk *result = thunk_alloc(arena, FUNC_FACTORIAL, 1);
    result->args[0] = thunk_alloc_completed(arena, sigil_val_int(n - 1));
    return result;
}

static SigilThunk* fib_ctor(ThunkArena *arena, SigilThunk **args) {
    if (!args || !args[0]) return NULL;
    int64_t n = thunk_force(args[0], arena).i;
    if (n < 2) return thunk_alloc_completed(arena, sigil_val_int(n));
    /* Two PENDING child thunks representing fib(n-1) and fib(n-2) */
    SigilThunk *child0 = thunk_alloc(arena, FUNC_FIB, 1);
    child0->args[0] = thunk_alloc_completed(arena, sigil_val_int(n - 1));
    SigilThunk *child1 = thunk_alloc(arena, FUNC_FIB, 1);
    child1->args[0] = thunk_alloc_completed(arena, sigil_val_int(n - 2));
    SigilThunk *result = thunk_alloc(arena, FUNC_FIB, 2);
    result->args[0] = child0;
    result->args[1] = child1;
    return result;
}

static SigilThunk* sum_ctor(ThunkArena *arena, SigilThunk **args) {
    if (!args || !args[0]) return NULL;
    int64_t n = thunk_force(args[0], arena).i;
    if (n <= 0) return thunk_alloc_completed(arena, sigil_val_int(0));
    /* Linear chain: sum(n) = n + sum(n-1) */
    SigilThunk *result = thunk_alloc(arena, FUNC_SUM, 1);
    result->args[0] = thunk_alloc_completed(arena, sigil_val_int(n - 1));
    return result;
}

static SigilThunk* add_ctor(ThunkArena *arena, SigilThunk **args) {
    if (!args || !args[0] || !args[1]) return NULL;
    int64_t a = thunk_force(args[0], arena).i;
    int64_t b = thunk_force(args[1], arena).i;
    return thunk_alloc_completed(arena, sigil_val_int(a + b));
}

/* ── Evaluators ──────────────────────────────────────────────────── */

static SigilVal factorial_eval(SigilThunk *t, ThunkArena *arena) {
    int64_t n = thunk_force(t->args[0], arena).i;
    if (n <= 1) return sigil_val_int(1);
    /* Force the child (factorial(n-1)) from the expanded graph */
    int64_t child0 = sigil_force_val(thunk_force(t->children[0], arena), arena).i;
    return sigil_val_int(n * child0);
}

static SigilVal fib_eval(SigilThunk *t, ThunkArena *arena) {
    int64_t n = thunk_force(t->args[0], arena).i;
    if (n < 2) return sigil_val_int(n);
    int64_t child0 = sigil_force_val(thunk_force(t->children[0], arena), arena).i;
    int64_t child1 = sigil_force_val(thunk_force(t->children[1], arena), arena).i;
    return sigil_val_int(child0 + child1);
}

static SigilVal sum_eval(SigilThunk *t, ThunkArena *arena) {
    int64_t n = thunk_force(t->args[0], arena).i;
    if (n <= 0) return sigil_val_int(0);
    /* Force the child (sum(n-1)) from the expanded graph */
    int64_t child0 = sigil_force_val(thunk_force(t->children[0], arena), arena).i;
    return sigil_val_int(n + child0);
}

static SigilVal add_eval(SigilThunk *t, ThunkArena *arena) {
    int64_t a = thunk_force(t->args[0], arena).i;
    int64_t b = thunk_force(t->args[1], arena).i;
    return sigil_val_int(a + b);
}

/* ── Dispatch tables ─────────────────────────────────────────────── */

static ThunkConstructor constructors[FUNC_COUNT] = {
    factorial_ctor, fib_ctor, sum_ctor, add_ctor
};
static ThunkEvaluator evaluators[FUNC_COUNT] = {
    factorial_eval, fib_eval, sum_eval, add_eval
};

/* ── Helpers ─────────────────────────────────────────────────────── */

static double timespec_ms(struct timespec *start, struct timespec *end) {
    double s = (double)(end->tv_sec - start->tv_sec) * 1000.0;
    double ns = (double)(end->tv_nsec - start->tv_nsec) / 1e6;
    return s + ns;
}

static const char *bin_name(ExecutionBin b) {
    return execution_bin_name(b);
}

/* Build root thunk for a given algorithm */
static SigilThunk* build_root(ThunkArena *arena, int func_id, int64_t arg) {
    SigilThunk *root = thunk_alloc(arena, (uint16_t)func_id, 1);
    root->args[0] = thunk_alloc_completed(arena, sigil_val_int(arg));
    return root;
}

static SigilThunk* build_add_root(ThunkArena *arena, int64_t a, int64_t b) {
    SigilThunk *root = thunk_alloc(arena, FUNC_ADD, 2);
    root->args[0] = thunk_alloc_completed(arena, sigil_val_int(a));
    root->args[1] = thunk_alloc_completed(arena, sigil_val_int(b));
    return root;
}

/* ── Test infrastructure ─────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { tests_passed++; } \
    else { tests_failed++; printf("  FAIL: " fmt "\n", ##__VA_ARGS__); } \
} while(0)

/* ── Algorithm test ──────────────────────────────────────────────── */

typedef struct {
    const char *name;
    int func_id;
    int64_t arg;
    int64_t arg2;       /* only for add */
    int64_t expected;
    ExecutionBin acceptable_bins[3];
    int acceptable_count;
} AlgoTest;

static void run_algo_test(AlgoTest *test, ThunkArena *arena, HardwareProfile *hw) {
    printf("\n── %s ──\n", test->name);

    /* Build graph */
    thunk_arena_reset(arena);
    SigilThunk *root;
    if (test->func_id == FUNC_ADD) {
        root = build_add_root(arena, test->arg, test->arg2);
    } else {
        root = build_root(arena, test->func_id, test->arg);
    }

    /* Classify */
    struct timespec t0, t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ExpansionStats stats = expand_thunk_graph(root, arena, EXPAND_MAX_THUNKS, EXPAND_MAX_DEPTH);
    ExecutionBin bin = classify(&stats, hw);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    printf("  Stats: depth=%d thunks=%d max_width=%d terminated=%d uniform=%d\n",
           stats.depth, stats.total_thunks, stats.max_width,
           stats.terminated, stats.uniform_ops);
    printf("  Bin: %s (%d)\n", bin_name(bin), bin);
    printf("  Classification time: %.3f ms\n", timespec_ms(&t0, &t1));

    /* Check bin is in acceptable set */
    int bin_ok = 0;
    for (int i = 0; i < test->acceptable_count; i++) {
        if (bin == test->acceptable_bins[i]) { bin_ok = 1; break; }
    }
    CHECK(bin_ok, "%s: bin %s not in expected set", test->name, bin_name(bin));

    /* Execute via pipeline */
    thunk_arena_reset(arena);
    if (test->func_id == FUNC_ADD) {
        root = build_add_root(arena, test->arg, test->arg2);
    } else {
        root = build_root(arena, test->func_id, test->arg);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    SigilVal result = execute_thunk(root, arena, hw);
    clock_gettime(CLOCK_MONOTONIC, &t2);

    printf("  Result: %" PRId64 " (expected %" PRId64 ")\n", result.i, test->expected);
    printf("  Execution time: %.3f ms\n", timespec_ms(&t1, &t2));
    CHECK(result.i == test->expected, "%s: got %" PRId64 " expected %" PRId64,
          test->name, result.i, test->expected);
}

/* ── Cross-executor verification ─────────────────────────────────── */

static void run_cross_executor(AlgoTest *test, ThunkArena *arena, HardwareProfile *hw) {
    printf("\n  Cross-executor for %s:\n", test->name);

    const char *exec_names[] = {
        "seq_obligate", "seq_optimal", "coro", "thread", "gpu"
    };

    int64_t results[5];

    for (int e = 0; e < 5; e++) {
        thunk_arena_reset(arena);
        SigilThunk *root;
        if (test->func_id == FUNC_ADD) {
            root = build_add_root(arena, test->arg, test->arg2);
        } else {
            root = build_root(arena, test->func_id, test->arg);
        }

        SigilVal val;
        switch (e) {
            case 0: val = execute_seq_obligate(root, arena); break;
            case 1: val = execute_seq_optimal(root, arena); break;
            case 2: val = execute_coro(root, arena, hw->core_count); break;
            case 3: val = execute_thread(root, arena, hw->core_count); break;
            case 4: val = execute_gpu(root, arena); break;
            default: val = sigil_val_int(0); break;
        }
        results[e] = val.i;
        printf("    %-14s = %" PRId64 "\n", exec_names[e], val.i);
    }

    /* All results must match expected */
    for (int e = 0; e < 5; e++) {
        CHECK(results[e] == test->expected,
              "%s via %s: got %" PRId64 " expected %" PRId64,
              test->name, exec_names[e], results[e], test->expected);
    }
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Sigil Thunk Validation Harness ===\n");

    /* Install dispatch tables */
    sigil_constructors = constructors;
    sigil_evaluators = evaluators;
    sigil_thunk_fn_count = FUNC_COUNT;

    /* Init arena */
    ThunkArena arena;
    thunk_arena_init(&arena, 1024 * 1024); /* 1 MB */

    /* Hardware profile */
    HardwareProfile hw;
    calibrate_hardware(&hw);
    printf("Hardware: %d cores, gpu=%d\n", hw.core_count, hw.gpu_available);

    /* Define test cases */
    AlgoTest tests[] = {
        {
            .name = "factorial(20)",
            .func_id = FUNC_FACTORIAL,
            .arg = 20, .arg2 = 0,
            .expected = factorial_plain(20),
            .acceptable_bins = {BIN_SEQ_OBLIGATE, BIN_SEQ_OPTIMAL, -1},
            .acceptable_count = 2
        },
        {
            .name = "fib(15)",
            .func_id = FUNC_FIB,
            .arg = 15, .arg2 = 0,
            .expected = fib_plain(15),
            .acceptable_bins = {BIN_THREAD, BIN_CORO, BIN_SEQ_OPTIMAL},
            .acceptable_count = 3
        },
        {
            .name = "sum(1..100)",
            .func_id = FUNC_SUM,
            .arg = 100, .arg2 = 0,
            .expected = sum_plain(100),
            .acceptable_bins = {BIN_SEQ_OBLIGATE, BIN_SEQ_OPTIMAL, -1},
            .acceptable_count = 2
        },
        {
            .name = "add(3,4)",
            .func_id = FUNC_ADD,
            .arg = 3, .arg2 = 4,
            .expected = 7,
            .acceptable_bins = {BIN_SEQ_OBLIGATE, BIN_SEQ_OPTIMAL, -1},
            .acceptable_count = 2
        }
    };
    int num_tests = sizeof(tests) / sizeof(tests[0]);

    /* Part 1: Individual algorithm tests */
    printf("\n=== Part 1: Classification & Execution ===\n");
    for (int i = 0; i < num_tests; i++) {
        run_algo_test(&tests[i], &arena, &hw);
    }

    /* Part 2: Cross-executor verification */
    printf("\n=== Part 2: Cross-Executor Verification ===\n");
    for (int i = 0; i < num_tests; i++) {
        run_cross_executor(&tests[i], &arena, &hw);
    }

    /* Summary */
    printf("\n=== Summary ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    printf("Total:  %d\n", tests_passed + tests_failed);
    printf("%s\n", tests_failed == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    thunk_arena_destroy(&arena);
    return tests_failed == 0 ? 0 : 1;
}
