#include "sigil_thunk.h"
#include <stdio.h>

/* ── Bin 4: GPU Execution (stub — falls back to sequential) ──────── */

SigilVal execute_gpu(SigilThunk *root, ThunkArena *arena) {
    /* GPU execution not yet implemented. Fall back to sequential force. */
    return thunk_force(root, arena);
}
