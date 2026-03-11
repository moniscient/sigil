#include "sigil_executor.h"
#include "sigil_expander.h"
#include "sigil_classifier.h"
#include <stdatomic.h>

/* External executor entry points */
extern SigilVal execute_seq_obligate(SigilThunk *root, ThunkArena *arena);
extern SigilVal execute_seq_optimal(SigilThunk *root, ThunkArena *arena);
extern SigilVal execute_coro(SigilThunk *root, ThunkArena *arena, int core_count);
extern SigilVal execute_thread(SigilThunk *root, ThunkArena *arena, int core_count);
extern SigilVal execute_gpu(SigilThunk *root, ThunkArena *arena);

SigilVal execute_thunk(SigilThunk *root, ThunkArena *arena, HardwareProfile *hw) {
    if (!root) return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
    if (atomic_load_explicit(&root->state, memory_order_acquire) == THUNK_COMPLETED)
        return root->value;

    /* Expand the thunk graph */
    ExpansionStats stats = expand_thunk_graph(root, arena,
                                               EXPAND_MAX_THUNKS, EXPAND_MAX_DEPTH);

    /* Classify */
    ExecutionBin bin = classify(&stats, hw);

    /* Dispatch */
    switch (bin) {
        case BIN_SEQ_OBLIGATE:
            return execute_seq_obligate(root, arena);
        case BIN_SEQ_OPTIMAL:
            return execute_seq_optimal(root, arena);
        case BIN_CORO:
            return execute_coro(root, arena, hw->core_count);
        case BIN_THREAD:
            return execute_thread(root, arena, hw->core_count);
        case BIN_GPU:
            return execute_gpu(root, arena);
    }
    return thunk_force(root, arena);
}

/* Plan 5: sigil_execute_val — force SigilVal at evaluation boundaries.
   Each call independently expands → classifies → dispatches its sub-graph. */
SigilVal sigil_execute_val(SigilVal v, ThunkArena *arena, HardwareProfile *hw) {
    if (v.kind == SIGIL_VAL_THUNK && v.t)
        return execute_thunk(v.t, arena, hw);
    return v;
}
