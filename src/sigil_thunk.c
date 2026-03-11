#include "sigil_thunk.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#ifdef __APPLE__
#include <sched.h>
#define sigil_yield() sched_yield()
#else
#include <sched.h>
#define sigil_yield() sched_yield()
#endif

/* ── Default dispatch tables (overridden by generated code) ──────── */

ThunkConstructor *sigil_constructors = NULL;
ThunkEvaluator *sigil_evaluators = NULL;
int sigil_thunk_fn_count = 0;

/* ── Arena ───────────────────────────────────────────────────────── */

void thunk_arena_init(ThunkArena *a, size_t capacity) {
    a->base = (char *)malloc(capacity);
    a->used = 0;
    a->capacity = capacity;
}

void thunk_arena_reset(ThunkArena *a) {
    a->used = 0;
}

void thunk_arena_destroy(ThunkArena *a) {
    free(a->base);
    a->base = NULL;
    a->used = 0;
    a->capacity = 0;
}

static void *thunk_arena_alloc(ThunkArena *a, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;
    if (a->used + size > a->capacity) {
        /* Grow arena */
        size_t new_cap = a->capacity * 2;
        if (new_cap < a->used + size) new_cap = a->used + size + a->capacity;
        char *new_base = (char *)realloc(a->base, new_cap);
        if (!new_base) {
            fprintf(stderr, "thunk arena: out of memory\n");
            abort();
        }
        a->base = new_base;
        a->capacity = new_cap;
    }
    void *ptr = a->base + a->used;
    a->used += size;
    return ptr;
}

void *thunk_arena_raw_alloc(ThunkArena *a, size_t size) {
    return thunk_arena_alloc(a, size);
}

/* ── Allocation ──────────────────────────────────────────────────── */

SigilThunk *thunk_alloc(ThunkArena *a, uint16_t func_id, uint8_t arity) {
    SigilThunk *t = (SigilThunk *)thunk_arena_alloc(a, sizeof(SigilThunk));
    atomic_store_explicit(&t->state, THUNK_PENDING, memory_order_relaxed);
    t->func_id = func_id;
    t->arity = arity;
    t->depth = 0;
    t->value = (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
    atomic_store_explicit(&t->refcount, 1, memory_order_relaxed);
    t->children = NULL;
    t->child_count = 0;
    if (arity > 0) {
        t->args = (SigilThunk **)thunk_arena_alloc(a, arity * sizeof(SigilThunk *));
        memset(t->args, 0, arity * sizeof(SigilThunk *));
    } else {
        t->args = NULL;
    }
    return t;
}

SigilThunk *thunk_alloc_completed(ThunkArena *a, SigilVal value) {
    SigilThunk *t = (SigilThunk *)thunk_arena_alloc(a, sizeof(SigilThunk));
    atomic_store_explicit(&t->state, THUNK_COMPLETED, memory_order_relaxed);
    t->func_id = 0;
    t->arity = 0;
    t->args = NULL;
    t->value = value;
    t->depth = 0;
    atomic_store_explicit(&t->refcount, 1, memory_order_relaxed);
    t->children = NULL;
    t->child_count = 0;
    return t;
}

/* ── Reference Counting ──────────────────────────────────────────── */

void thunk_retain(SigilThunk *t) {
    if (t) atomic_fetch_add_explicit(&t->refcount, 1, memory_order_relaxed);
}

void thunk_release(SigilThunk *t) {
    if (t) atomic_fetch_sub_explicit(&t->refcount, 1, memory_order_relaxed);
    /* Arena-allocated — no dealloc; refcount used for tracking only */
}

/* ── Atomic CAS-Based Force ───────────────────────────────────────── */

SigilVal thunk_force(SigilThunk *t, ThunkArena *arena) {
    if (!t) {
        fprintf(stderr, "thunk_force: NULL thunk\n");
        return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
    }

retry:;
    int st = atomic_load_explicit(&t->state, memory_order_acquire);

    if (st == THUNK_COMPLETED)
        return t->value;

    if (st == THUNK_IN_PROGRESS) {
        /* Another thread is evaluating — spin-wait */
        int spins = 0;
        while ((st = atomic_load_explicit(&t->state, memory_order_acquire)) == THUNK_IN_PROGRESS) {
            if (++spins > 100000) {
                /* Likely a true cycle in single-threaded context */
                fprintf(stderr, "thunk_force: cycle detected (func_id=%d)\n", t->func_id);
                return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
            }
            sigil_yield();
        }
        goto retry;
    }

    /* PENDING: try to expand (call constructor), then evaluate */
    if (st == THUNK_PENDING) {
        int expected = THUNK_PENDING;
        if (atomic_compare_exchange_strong_explicit(&t->state, &expected, THUNK_IN_PROGRESS,
                memory_order_acq_rel, memory_order_acquire)) {
            /* We won the race — expand via constructor if available */
            if (sigil_constructors && t->func_id < (uint16_t)sigil_thunk_fn_count) {
                SigilThunk *expanded = sigil_constructors[t->func_id](arena, t->args);
                if (expanded) {
                    if (atomic_load_explicit(&expanded->state, memory_order_acquire) == THUNK_COMPLETED) {
                        /* Constructor produced a completed value (base case) */
                        t->value = expanded->value;
                        atomic_store_explicit(&t->state, THUNK_COMPLETED, memory_order_release);
                        return t->value;
                    }
                    /* Detect whether expanded->args are PENDING sub-calls (tree recursion)
                       or completed parameters (linear recursion) */
                    bool has_pending = false;
                    for (int i = 0; i < expanded->arity; i++) {
                        if (expanded->args[i] &&
                            atomic_load_explicit(&expanded->args[i]->state, memory_order_acquire) != THUNK_COMPLETED) {
                            has_pending = true;
                            break;
                        }
                    }
                    if (has_pending) {
                        /* Tree recursion (fib): args are PENDING sub-calls */
                        t->children = expanded->args;
                        t->child_count = expanded->arity;
                    } else {
                        /* Linear recursion (factorial): expanded itself is the recursive call */
                        SigilThunk **ca = (SigilThunk **)thunk_arena_raw_alloc(arena, sizeof(SigilThunk *));
                        ca[0] = expanded;
                        t->children = ca;
                        t->child_count = 1;
                    }
                }
            }
            /* Now evaluate */
            if (sigil_evaluators && t->func_id < (uint16_t)sigil_thunk_fn_count) {
                t->value = sigil_evaluators[t->func_id](t, arena);
            } else {
                fprintf(stderr, "thunk_force: no evaluator for func_id=%d\n", t->func_id);
                t->value = (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
            }
            atomic_store_explicit(&t->state, THUNK_COMPLETED, memory_order_release);
            return t->value;
        }
        /* Lost the CAS race — retry */
        goto retry;
    }

    /* EXPANDED: constructor was called, children known — try to evaluate */
    if (st == THUNK_EXPANDED) {
        int expected = THUNK_EXPANDED;
        if (atomic_compare_exchange_strong_explicit(&t->state, &expected, THUNK_IN_PROGRESS,
                memory_order_acq_rel, memory_order_acquire)) {
            if (sigil_evaluators && t->func_id < (uint16_t)sigil_thunk_fn_count) {
                t->value = sigil_evaluators[t->func_id](t, arena);
            } else {
                fprintf(stderr, "thunk_force: no evaluator for func_id=%d\n", t->func_id);
                t->value = (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
            }
            atomic_store_explicit(&t->state, THUNK_COMPLETED, memory_order_release);
            return t->value;
        }
        goto retry;
    }

    /* Should not reach here */
    fprintf(stderr, "thunk_force: unknown state %d\n", st);
    return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
}

/* ── SigilVal force helper ───────────────────────────────────────── */

SigilVal sigil_force_val(SigilVal v, ThunkArena *arena) {
    /* Recursively force through nested thunks */
    while (v.kind == SIGIL_VAL_THUNK && v.t) {
        v = thunk_force(v.t, arena);
    }
    return v;
}
