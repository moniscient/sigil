#ifndef SIGIL_THUNK_H
#define SIGIL_THUNK_H

#include "sigil_runtime.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

/* ── Thunk State ─────────────────────────────────────────────────── */

typedef enum {
    THUNK_PENDING,       /* Never touched — needs constructor call */
    THUNK_EXPANDED,      /* Constructor called, children known */
    THUNK_IN_PROGRESS,   /* Currently being evaluated */
    THUNK_COMPLETED      /* Value computed and memoized */
} ThunkState;

/* ── Thunk Node ──────────────────────────────────────────────────── */

typedef struct SigilThunk {
    _Atomic int state;             /* ThunkState, atomic for CAS */
    uint16_t func_id;              /* index into dispatch tables */
    uint8_t arity;
    struct SigilThunk **args;      /* evaluator args (count = arity) */
    SigilVal value;                /* result when COMPLETED */
    int depth;
    _Atomic uint32_t refcount;     /* reference count (for tracking) */
    struct SigilThunk **children;  /* expanded children for graph walking */
    uint8_t child_count;           /* number of expanded children */
} SigilThunk;

/* ── Thunk Arena ─────────────────────────────────────────────────── */

typedef struct {
    char *base;
    size_t used, capacity;
} ThunkArena;

void thunk_arena_init(ThunkArena *a, size_t capacity);
void thunk_arena_reset(ThunkArena *a);
void thunk_arena_destroy(ThunkArena *a);

/* ── Dispatch Table Function Types ───────────────────────────────── */

typedef SigilThunk* (*ThunkConstructor)(ThunkArena *arena, SigilThunk **args);
typedef SigilVal (*ThunkEvaluator)(SigilThunk *t, ThunkArena *arena);

/* These are defined in the generated C code */
extern ThunkConstructor *sigil_constructors;
extern ThunkEvaluator *sigil_evaluators;
extern int sigil_thunk_fn_count;

/* ── Thunk Allocation ────────────────────────────────────────────── */

SigilThunk *thunk_alloc(ThunkArena *a, uint16_t func_id, uint8_t arity);
SigilThunk *thunk_alloc_completed(ThunkArena *a, SigilVal value);

/* ── Reference Counting ──────────────────────────────────────────── */

void thunk_retain(SigilThunk *t);
void thunk_release(SigilThunk *t);

/* ── Raw Arena Allocation (for expander/constructors) ────────────── */

void *thunk_arena_raw_alloc(ThunkArena *a, size_t size);

/* ── Forcing ─────────────────────────────────────────────────────── */

SigilVal thunk_force(SigilThunk *t, ThunkArena *arena);

/* ── Force helper for SigilVal (works on boxed thunks) ───────────── */

SigilVal sigil_force_val(SigilVal v, ThunkArena *arena);

#endif /* SIGIL_THUNK_H */
