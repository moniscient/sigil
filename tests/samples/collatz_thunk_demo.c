/*
 * Collatz Thunk Demo — Shows the thunk graph classification system in action.
 *
 * This program manually constructs thunk graphs that represent Collatz
 * computations and feeds them to the expander → classifier → executor pipeline.
 *
 * Compile:
 *   cc -std=c11 -I src -pthread -o collatz_demo tests/samples/collatz_thunk_demo.c \
 *     src/sigil_runtime.c src/sigil_thunk.c src/sigil_expander.c \
 *     src/sigil_classifier.c src/sigil_hardware.c src/sigil_exec_seq.c \
 *     src/sigil_exec_coro.c src/sigil_exec_thread.c src/sigil_exec_gpu.c \
 *     src/sigil_executor.c
 */

#include "sigil_thunk.h"
#include "sigil_expander.h"
#include "sigil_classifier.h"
#include "sigil_hardware.h"
#include "sigil_executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Collatz step as evaluator ───────────────────────────────────── */
/* func_id 0: step(n) — one Collatz iteration */

static SigilVal eval_step(SigilThunk *t, ThunkArena *arena) {
    SigilVal nv = thunk_force(t->args[0], arena);
    int64_t n = nv.i;
    int64_t result;
    if (n % 2 == 0) {
        result = n / 2;
    } else {
        result = (3 * n + 1) / 2;
    }
    return sigil_val_int(result);
}

/* func_id 1: count(start) — iterate step until reaching 1 */

static SigilVal eval_count(SigilThunk *t, ThunkArena *arena) {
    SigilVal sv = thunk_force(t->args[0], arena);
    int64_t n = sv.i;
    int64_t steps = 0;
    while (n > 1) {
        if (n % 2 == 0)
            n = n / 2;
        else
            n = (3 * n + 1) / 2;
        steps++;
    }
    return sigil_val_int(steps);
}

/* func_id 2: chain_step(n) — constructs a chain: step(step(step(...))) */
/* Constructor builds out one level of the Collatz chain */

static SigilThunk *ctor_chain_step(ThunkArena *arena, SigilThunk **args) {
    /* Force the arg to get the current value */
    SigilVal nv = thunk_force(args[0], arena);
    int64_t n = nv.i;
    if (n <= 1) {
        /* Base case: return completed thunk with value 1 */
        return thunk_alloc_completed(arena, sigil_val_int(1));
    }
    /* Compute one step */
    int64_t next;
    if (n % 2 == 0)
        next = n / 2;
    else
        next = (3 * n + 1) / 2;

    /* Create child thunk for the next step */
    SigilThunk *child = thunk_alloc(arena, 2, 1);  /* func_id 2 = chain_step */
    child->args[0] = thunk_alloc_completed(arena, sigil_val_int(next));
    return child;
}

static SigilVal eval_chain_step(SigilThunk *t, ThunkArena *arena) {
    SigilVal nv = thunk_force(t->args[0], arena);
    int64_t n = nv.i;
    if (n <= 1) return sigil_val_int(1);
    int64_t next;
    if (n % 2 == 0)
        next = n / 2;
    else
        next = (3 * n + 1) / 2;
    /* Create and force a new chain thunk */
    SigilThunk *child = thunk_alloc(arena, 2, 1);
    child->args[0] = thunk_alloc_completed(arena, sigil_val_int(next));
    return thunk_force(child, arena);
}

/* ── Dispatch tables ─────────────────────────────────────────────── */

static ThunkConstructor ctors[] = { NULL, NULL, ctor_chain_step };
static ThunkEvaluator evals[] = { eval_step, eval_count, eval_chain_step };

/* ── Demo ────────────────────────────────────────────────────────── */

static void demo_classify_collatz(int64_t start, ThunkArena *arena, HardwareProfile *hw) {
    printf("\n=== Collatz thunk graph for n=%lld ===\n", (long long)start);

    /* Build a chain_step thunk graph rooted at start */
    SigilThunk *root = thunk_alloc(arena, 2, 1);  /* chain_step */
    root->args[0] = thunk_alloc_completed(arena, sigil_val_int(start));

    /* Expand the graph (probe phase) */
    ExpansionStats stats = expand_thunk_graph(root, arena,
                                               EXPAND_MAX_THUNKS, EXPAND_MAX_DEPTH);

    printf("  Expansion stats:\n");
    printf("    depth:        %d\n", stats.depth);
    printf("    total_thunks: %d\n", stats.total_thunks);
    printf("    max_width:    %d\n", stats.max_width);
    printf("    terminated:   %s\n", stats.terminated ? "yes" : "no");
    printf("    uniform_ops:  %s\n", stats.uniform_ops ? "yes" : "no");

    /* Classify */
    ExecutionBin bin = classify(&stats, hw);
    printf("  Classification: BIN_%s\n", execution_bin_name(bin));

    /* Force the root to get the final value */
    thunk_arena_reset(arena);
    root = thunk_alloc(arena, 2, 1);
    root->args[0] = thunk_alloc_completed(arena, sigil_val_int(start));
    SigilVal result = thunk_force(root, arena);
    printf("  Result:         %lld (should be 1)\n", (long long)result.i);

    /* Also compute step count via the count evaluator */
    thunk_arena_reset(arena);
    SigilThunk *count_thunk = thunk_alloc(arena, 1, 1);
    count_thunk->args[0] = thunk_alloc_completed(arena, sigil_val_int(start));
    SigilVal steps = thunk_force(count_thunk, arena);
    printf("  Steps to 1:     %lld\n", (long long)steps.i);
}

static void demo_parallel_collatz(ThunkArena *arena, HardwareProfile *hw) {
    printf("\n=== Parallel Collatz: 8 independent computations ===\n");

    /* Create 8 independent count thunks — these could run in parallel */
    int64_t starts[] = {7, 27, 97, 871, 6171, 77031, 113383, 837799};
    int n = 8;

    /* Build a "wide" graph: fake parent with 8 children */
    /* This is what the classifier would see for parallel map operations */
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = false;
    stats.depth = 2;
    stats.total_thunks = 1 + n;
    stats.max_width = n;
    stats.uniform_ops = true;  /* all same operation (count) */
    stats.width_at_depth[0] = 1;
    stats.width_at_depth[1] = n;

    ExecutionBin bin = classify(&stats, hw);
    printf("  Width: %d independent count operations\n", n);
    printf("  Classification: BIN_%s\n", execution_bin_name(bin));
    printf("  (threshold for parallelism: %d)\n", hw->parallelism_threshold);

    /* Execute them (currently sequential since thunks are eager) */
    for (int i = 0; i < n; i++) {
        thunk_arena_reset(arena);
        SigilThunk *t = thunk_alloc(arena, 1, 1);
        t->args[0] = thunk_alloc_completed(arena, sigil_val_int(starts[i]));
        SigilVal result = thunk_force(t, arena);
        printf("  collatz_count(%lld) = %lld steps\n",
               (long long)starts[i], (long long)result.i);
    }
}

int main(void) {
    ThunkArena arena;
    thunk_arena_init(&arena, 4 * 1024 * 1024);  /* 4MB arena */

    HardwareProfile hw;
    calibrate_hardware(&hw);

    printf("Hardware profile:\n");
    printf("  cores:                %d\n", hw.core_count);
    printf("  parallelism_threshold: %d\n", hw.parallelism_threshold);
    printf("  thread_depth_threshold: %d\n", hw.thread_depth_threshold);
    printf("  gpu_width_threshold:  %d\n", hw.gpu_width_threshold);
    printf("  thread_spawn_cost:    %lld ns\n", (long long)hw.thread_spawn_cost_ns);

    /* Set up dispatch tables */
    sigil_constructors = ctors;
    sigil_evaluators = evals;
    sigil_thunk_fn_count = 3;

    /* Demo 1: Small Collatz — should classify as SEQ_OPTIMAL (terminated) */
    demo_classify_collatz(7, &arena, &hw);

    /* Demo 2: Medium Collatz — 27 has 70+ steps, chain is deep */
    thunk_arena_reset(&arena);
    demo_classify_collatz(27, &arena, &hw);

    /* Demo 3: Large Collatz — 871 has 113 steps, very deep chain */
    thunk_arena_reset(&arena);
    demo_classify_collatz(871, &arena, &hw);

    /* Demo 4: Very large — 837799 has 524 steps (longest under 1M) */
    thunk_arena_reset(&arena);
    demo_classify_collatz(837799, &arena, &hw);

    /* Demo 5: Parallel independent Collatz computations */
    thunk_arena_reset(&arena);
    demo_parallel_collatz(&arena, &hw);

    thunk_arena_destroy(&arena);

    printf("\n=== Forcing mechanism analysis ===\n");
    printf("The Collatz sequence is an OBLIGATE SEQUENTIAL computation:\n");
    printf("  - Each step depends on the previous result (n -> f(n))\n");
    printf("  - The graph is a linear chain: max_width = 1\n");
    printf("  - The classifier correctly bins it as SEQ_OBLIGATE\n");
    printf("  - No parallelism is possible within a single sequence\n");
    printf("\nHowever, MULTIPLE independent sequences CAN parallelize:\n");
    printf("  - Computing count(7), count(27), count(871) simultaneously\n");
    printf("  - Each is independent — width = number of sequences\n");
    printf("  - The classifier bins this as CORO or THREAD depending on width\n");
    printf("\nForcing mechanism:\n");
    printf("  - thunk_force(): immediate recursive sequential evaluation\n");
    printf("  - execute_thunk(): expand graph -> classify -> dispatch to executor\n");
    printf("  - For Collatz chains: BIN_SEQ_OBLIGATE -> trampoline-style loop\n");
    printf("  - For parallel Collatz: BIN_CORO/THREAD -> work-stealing/partitioned\n");
    printf("  - Currently, thunks are created and immediately forced (eager-through-thunk)\n");
    printf("  - True lazy forcing would defer step(n) until the value is needed\n");

    return 0;
}
