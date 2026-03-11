#include "sigil_thunk.h"
#include <stdio.h>

/* ── Bin 0: Obligate Sequential (linear chain) ──────────────────── */

static SigilVal exec_seq_obligate(SigilThunk *root, ThunkArena *arena) {
    /* Walk the chain, forcing bottom-up via recursive thunk_force */
    return thunk_force(root, arena);
}

/* ── Bin 1: Optimal Sequential (small DAG) ──────────────────────── */

static SigilVal exec_seq_optimal(SigilThunk *root, ThunkArena *arena) {
    /* For small terminated graphs, simple recursive force is optimal */
    return thunk_force(root, arena);
}

/* ── Public Entry Points ─────────────────────────────────────────── */

SigilVal execute_seq_obligate(SigilThunk *root, ThunkArena *arena) {
    return exec_seq_obligate(root, arena);
}

SigilVal execute_seq_optimal(SigilThunk *root, ThunkArena *arena) {
    return exec_seq_optimal(root, arena);
}
