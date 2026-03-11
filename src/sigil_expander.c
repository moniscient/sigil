#include "sigil_expander.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

ExpansionStats expand_thunk_graph(SigilThunk *root, ThunkArena *arena,
                                   int max_thunks, int max_depth) {
    ExpansionStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.terminated = true;
    stats.uniform_ops = true;

    if (!root || atomic_load_explicit(&root->state, memory_order_acquire) == THUNK_COMPLETED) {
        stats.terminated = true;
        return stats;
    }

    if (max_thunks <= 0) max_thunks = EXPAND_MAX_THUNKS;
    if (max_depth <= 0 || max_depth > EXPAND_MAX_DEPTH) max_depth = EXPAND_MAX_DEPTH;

    /* BFS queue */
    SigilThunk **queue = (SigilThunk **)malloc(max_thunks * sizeof(SigilThunk *));
    int *depths = (int *)malloc(max_thunks * sizeof(int));
    int head = 0, tail = 0;

    queue[tail] = root;
    depths[tail] = 0;
    tail++;
    stats.total_thunks = 1;

    uint16_t first_func_id = root->func_id;

    while (head < tail && stats.total_thunks < max_thunks) {
        SigilThunk *t = queue[head];
        int d = depths[head];
        head++;

        if (d >= max_depth) {
            stats.terminated = false;
            continue;
        }

        if (d >= stats.depth) stats.depth = d + 1;
        if (d < EXPAND_MAX_DEPTH) stats.width_at_depth[d]++;

        int tst = atomic_load_explicit(&t->state, memory_order_acquire);
        if (tst == THUNK_COMPLETED) continue;

        /* Check uniformity */
        if (t->func_id != first_func_id)
            stats.uniform_ops = false;

        /* Expand: call constructor if PENDING */
        if (tst == THUNK_PENDING && sigil_constructors &&
            t->func_id < (uint16_t)sigil_thunk_fn_count) {
            SigilThunk *expanded = sigil_constructors[t->func_id](arena, t->args);
            if (expanded) {
                int est = atomic_load_explicit(&expanded->state, memory_order_acquire);
                if (est == THUNK_COMPLETED) {
                    /* Base case: constructor produced a value */
                    atomic_store_explicit(&t->state, THUNK_COMPLETED, memory_order_release);
                    t->value = expanded->value;
                    continue;
                }
                /* Determine children for graph walking.
                   Two patterns:
                   - "Tree recursion" (fib): expanded->args contain PENDING sub-calls
                   - "Linear recursion" (factorial): expanded itself IS the recursive call,
                     its args are completed parameters */
                bool has_pending_args = false;
                for (int i = 0; i < expanded->arity; i++) {
                    if (expanded->args[i] &&
                        atomic_load_explicit(&expanded->args[i]->state, memory_order_acquire) != THUNK_COMPLETED) {
                        has_pending_args = true;
                        break;
                    }
                }
                if (has_pending_args) {
                    /* Args are the recursive sub-calls (fib pattern) */
                    t->children = expanded->args;
                    t->child_count = expanded->arity;
                } else {
                    /* expanded itself is the recursive call (factorial pattern) */
                    SigilThunk **ca = (SigilThunk **)thunk_arena_raw_alloc(arena, sizeof(SigilThunk *));
                    ca[0] = expanded;
                    t->children = ca;
                    t->child_count = 1;
                }
            }
            /* Mark as EXPANDED — constructor called, children now known */
            atomic_store_explicit(&t->state, THUNK_EXPANDED, memory_order_release);
        }

        /* Enqueue children for graph walking */
        int nc = t->child_count > 0 ? t->child_count : t->arity;
        SigilThunk **kids = t->child_count > 0 ? t->children : t->args;
        for (int i = 0; i < nc && stats.total_thunks < max_thunks; i++) {
            if (kids[i] && atomic_load_explicit(&kids[i]->state, memory_order_acquire) != THUNK_COMPLETED) {
                if (tail < max_thunks) {
                    queue[tail] = kids[i];
                    depths[tail] = d + 1;
                    tail++;
                    stats.total_thunks++;
                } else {
                    stats.terminated = false;
                }
            }
        }
    }

    /* If we ran out of budget, graph is not terminated */
    if (head < tail) stats.terminated = false;

    /* Compute max_width */
    for (int i = 0; i < stats.depth && i < EXPAND_MAX_DEPTH; i++) {
        if (stats.width_at_depth[i] > stats.max_width)
            stats.max_width = stats.width_at_depth[i];
    }

    free(queue);
    free(depths);
    return stats;
}
