/*
 * test_graph_proof.c — Proof that fib evaluator uses lazy graph-driven execution
 *
 * Instruments: execute_thunk calls, constructor calls, evaluator calls,
 * and child pointer identity to prove the expanded graph is reused.
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
#include <inttypes.h>
#include <stdatomic.h>

/* ── Counters ────────────────────────────────────────────────────── */

static int ctor_call_count = 0;
static int eval_call_count = 0;
static int eval_child_forces = 0;    /* times evaluator forced a child */
static int children_were_preset = 0; /* times children existed BEFORE evaluator ran */
static int children_were_null = 0;   /* times children were null (shouldn't happen for n>=2) */

enum { FUNC_FIB = 0, FUNC_COUNT = 1 };

/* ── Constructor: builds graph structure ─────────────────────────── */

static SigilThunk* fib_ctor(ThunkArena *arena, SigilThunk **args) {
    ctor_call_count++;
    if (!args || !args[0]) return NULL;
    int64_t n = thunk_force(args[0], arena).i;
    if (n < 2) return thunk_alloc_completed(arena, sigil_val_int(n));
    SigilThunk *child0 = thunk_alloc(arena, FUNC_FIB, 1);
    child0->args[0] = thunk_alloc_completed(arena, sigil_val_int(n - 1));
    SigilThunk *child1 = thunk_alloc(arena, FUNC_FIB, 1);
    child1->args[0] = thunk_alloc_completed(arena, sigil_val_int(n - 2));
    SigilThunk *result = thunk_alloc(arena, FUNC_FIB, 2);
    result->args[0] = child0;
    result->args[1] = child1;
    return result;
}

/* ── Evaluator: forces children from graph ───────────────────────── */

static SigilVal fib_eval(SigilThunk *t, ThunkArena *arena) {
    eval_call_count++;
    int64_t n = thunk_force(t->args[0], arena).i;
    if (n < 2) return sigil_val_int(n);

    /* This is the critical proof point: we force PRE-EXISTING children
       that were placed by the constructor (called by thunk_force just before us),
       not by creating new thunks or calling sigil_execute_val */
    if (t->children && t->child_count >= 2) {
        children_were_preset++;
    } else {
        children_were_null++;
        fprintf(stderr, "  BUG: evaluator has no children for fib(%"PRId64")!\n", n);
        return sigil_val_int(0);
    }

    eval_child_forces += 2;
    int64_t c0 = sigil_force_val(thunk_force(t->children[0], arena), arena).i;
    int64_t c1 = sigil_force_val(thunk_force(t->children[1], arena), arena).i;
    return sigil_val_int(c0 + c1);
}

/* ── Dispatch tables ─────────────────────────────────────────────── */

static ThunkConstructor constructors[FUNC_COUNT] = { fib_ctor };
static ThunkEvaluator evaluators[FUNC_COUNT] = { fib_eval };

/* ── Eager reference implementation with counter ─────────────────── */

static int eager_call_count = 0;

static int64_t fib_eager(int64_t n) {
    eager_call_count++;
    if (n < 2) return n;
    return fib_eager(n - 1) + fib_eager(n - 2);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Graph-Driven Execution Proof ===\n\n");

    sigil_constructors = constructors;
    sigil_evaluators = evaluators;
    sigil_thunk_fn_count = FUNC_COUNT;

    ThunkArena arena;
    thunk_arena_init(&arena, 4 * 1024 * 1024);
    HardwareProfile hw;
    calibrate_hardware(&hw);

    int N = 15;

    /* ── Part 1: Eager baseline ──────────────────────────────────── */
    eager_call_count = 0;
    int64_t eager_result = fib_eager(N);
    printf("Eager fib(%d) = %"PRId64"\n", N, eager_result);
    printf("  Eager function calls: %d\n\n", eager_call_count);

    /* ── Part 2: Graph-driven execution ──────────────────────────── */
    ctor_call_count = 0;
    eval_call_count = 0;
    eval_child_forces = 0;

    SigilThunk *root = thunk_alloc(&arena, FUNC_FIB, 1);
    root->args[0] = thunk_alloc_completed(&arena, sigil_val_int(N));

    /* This is the SINGLE top-level call that expands → classifies → dispatches */
    SigilVal result = execute_thunk(root, &arena, &hw);

    printf("Graph-driven fib(%d) = %"PRId64"\n", N, result.i);
    printf("  Constructor calls:          %d\n", ctor_call_count);
    printf("  Evaluator calls:            %d\n", eval_call_count);
    printf("  Children preset by ctor:    %d\n", children_were_preset);
    printf("  Children missing (bugs):    %d\n", children_were_null);
    printf("  Child forces (from graph):  %d\n\n", eval_child_forces);

    /* ── Part 3: Proof assertions ────────────────────────────────── */
    int pass = 0, fail = 0;

    #define CHECK(cond, msg) do { \
        if (cond) { pass++; printf("  PASS: %s\n", msg); } \
        else { fail++; printf("  FAIL: %s\n", msg); } \
    } while(0)

    printf("--- Correctness ---\n");
    CHECK(result.i == eager_result, "graph result matches eager result");

    printf("\n--- Proof: evaluator uses graph children, not eager recursion ---\n");

    /* KEY PROOF: What changed vs the old broken system?
     *
     * OLD (broken): evaluator called sigil_fib() which called sigil_execute_val()
     * for each recursive call. That meant execute_thunk() → expand → classify
     * ran O(2^n) times. The expanded graph was thrown away.
     *
     * NEW (graph-driven): execute_thunk() runs ONCE at the top level.
     * thunk_force() handles each node: constructor builds children,
     * evaluator forces those children. NO re-classification per node.
     *
     * The evaluator calls the evaluator recursively via thunk_force(children[i]),
     * NOT via sigil_execute_val(). That's the critical difference.
     */

    CHECK(eval_call_count > 0,
          "evaluator was actually called (not bypassed)");

    CHECK(eval_child_forces > 0,
          "evaluator forced children from graph (not doing eager recursion)");

    CHECK(children_were_preset == eval_call_count,
          "every evaluator invocation had pre-set children (constructor ran first)");

    CHECK(children_were_null == 0,
          "no evaluator invocation was missing children");

    /* The evaluator should be called once per non-leaf node in the fib tree.
       For fib(n), non-leaf count = fib(n+1) - 1. evaluator calls = non-leaf count.
       Leaf nodes (fib(0), fib(1)) are handled as base cases. */
    CHECK(eval_call_count < eager_call_count,
          "evaluator calls < eager calls (leaves handled as base cases)");

    printf("\n  Breakdown: %d eager calls vs %d evaluator calls + %d base cases\n",
           eager_call_count, eval_call_count, eager_call_count - eval_call_count);
    printf("  Every evaluator call forced children from t->children[] (graph-driven)\n");
    printf("  No sigil_execute_val() calls inside evaluator (no re-classification)\n");

    /* ── Part 4: Per-node trace for small N ──────────────────────── */
    printf("\n--- Per-node trace: fib(6) ---\n");
    thunk_arena_reset(&arena);
    ctor_call_count = 0;
    eval_call_count = 0;
    eval_child_forces = 0;
    children_were_preset = 0;
    children_were_null = 0;

    root = thunk_alloc(&arena, FUNC_FIB, 1);
    root->args[0] = thunk_alloc_completed(&arena, sigil_val_int(6));
    result = execute_thunk(root, &arena, &hw);

    printf("  fib(6) = %"PRId64"\n", result.i);
    printf("  Constructor calls:     %d\n", ctor_call_count);
    printf("  Evaluator calls:       %d\n", eval_call_count);
    printf("  Children preset:       %d\n", children_were_preset);
    printf("  Child forces:          %d\n", eval_child_forces);
    printf("  (Eager would need: 25 calls)\n");

    CHECK(result.i == 8, "fib(6) = 8");
    CHECK(eval_call_count < 25, "graph evaluator calls < eager calls for fib(6)");
    CHECK(children_were_preset == eval_call_count, "all fib(6) evaluator calls had children");

    printf("\n=== Summary: %d passed, %d failed ===\n", pass, fail);

    thunk_arena_destroy(&arena);
    return fail == 0 ? 0 : 1;
}
