#include "sigil_thunk.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* ── Bin 2: Coroutine / Work-Stealing Pool ──────────────────────── */

/* Work-stealing deque entry */
typedef struct {
    SigilThunk **items;
    int head, tail, capacity;
    pthread_mutex_t lock;
} WorkDeque;

static void deque_init(WorkDeque *d, int capacity) {
    d->items = (SigilThunk **)malloc(capacity * sizeof(SigilThunk *));
    d->head = 0;
    d->tail = 0;
    d->capacity = capacity;
    pthread_mutex_init(&d->lock, NULL);
}

static void deque_destroy(WorkDeque *d) {
    free(d->items);
    pthread_mutex_destroy(&d->lock);
}

static void deque_push(WorkDeque *d, SigilThunk *t) {
    pthread_mutex_lock(&d->lock);
    if (d->tail >= d->capacity) {
        d->capacity *= 2;
        d->items = (SigilThunk **)realloc(d->items, d->capacity * sizeof(SigilThunk *));
    }
    d->items[d->tail++] = t;
    pthread_mutex_unlock(&d->lock);
}

static SigilThunk *deque_pop(WorkDeque *d) {
    pthread_mutex_lock(&d->lock);
    SigilThunk *t = NULL;
    if (d->tail > d->head)
        t = d->items[--d->tail];
    pthread_mutex_unlock(&d->lock);
    return t;
}

static SigilThunk *deque_steal(WorkDeque *d) {
    pthread_mutex_lock(&d->lock);
    SigilThunk *t = NULL;
    if (d->tail > d->head)
        t = d->items[d->head++];
    pthread_mutex_unlock(&d->lock);
    return t;
}

typedef struct {
    WorkDeque *deques;
    int worker_count;
    int worker_id;
    ThunkArena *arena;
    volatile int *done;
    pthread_mutex_t *arena_lock;
} WorkerCtx;

static void process_thunk(WorkerCtx *ctx, SigilThunk *t) {
    if (!t) return;
    int st = atomic_load_explicit(&t->state, memory_order_acquire);
    if (st == THUNK_COMPLETED) return;

    /* Force all children first */
    for (int i = 0; i < t->arity; i++) {
        if (t->args[i]) {
            int cst = atomic_load_explicit(&t->args[i]->state, memory_order_acquire);
            if (cst != THUNK_COMPLETED)
                process_thunk(ctx, t->args[i]);
        }
    }

    /* Use CAS-based thunk_force instead of manual state manipulation */
    pthread_mutex_lock(ctx->arena_lock);
    thunk_force(t, ctx->arena);
    pthread_mutex_unlock(ctx->arena_lock);
}

static void *worker_func(void *arg) {
    WorkerCtx *ctx = (WorkerCtx *)arg;
    WorkDeque *my_deque = &ctx->deques[ctx->worker_id];

    while (!*ctx->done) {
        SigilThunk *t = deque_pop(my_deque);
        if (!t) {
            /* Try stealing from other workers */
            for (int i = 0; i < ctx->worker_count; i++) {
                if (i == ctx->worker_id) continue;
                t = deque_steal(&ctx->deques[i]);
                if (t) break;
            }
        }
        if (t) {
            process_thunk(ctx, t);
        } else {
            /* Nothing to do */
            break;
        }
    }
    return NULL;
}

SigilVal execute_coro(SigilThunk *root, ThunkArena *arena, int core_count) {
    if (!root) return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
    if (atomic_load_explicit(&root->state, memory_order_acquire) == THUNK_COMPLETED)
        return root->value;

    int nworkers = core_count > 1 ? core_count : 2;

    WorkDeque *deques = (WorkDeque *)malloc(nworkers * sizeof(WorkDeque));
    for (int i = 0; i < nworkers; i++)
        deque_init(&deques[i], 256);

    /* Seed leaf thunks into deques */
    /* For now, just push root and let workers process recursively */
    deque_push(&deques[0], root);

    volatile int done = 0;
    pthread_mutex_t arena_lock;
    pthread_mutex_init(&arena_lock, NULL);

    pthread_t *threads = (pthread_t *)malloc(nworkers * sizeof(pthread_t));
    WorkerCtx *ctxs = (WorkerCtx *)malloc(nworkers * sizeof(WorkerCtx));
    for (int i = 0; i < nworkers; i++) {
        ctxs[i].deques = deques;
        ctxs[i].worker_count = nworkers;
        ctxs[i].worker_id = i;
        ctxs[i].arena = arena;
        ctxs[i].done = &done;
        ctxs[i].arena_lock = &arena_lock;
        pthread_create(&threads[i], NULL, worker_func, &ctxs[i]);
    }

    for (int i = 0; i < nworkers; i++)
        pthread_join(threads[i], NULL);

    /* If root still not completed, force sequentially as fallback */
    SigilVal result;
    if (atomic_load_explicit(&root->state, memory_order_acquire) == THUNK_COMPLETED) {
        result = root->value;
    } else {
        result = thunk_force(root, arena);
    }

    for (int i = 0; i < nworkers; i++)
        deque_destroy(&deques[i]);
    free(deques);
    free(threads);
    free(ctxs);
    pthread_mutex_destroy(&arena_lock);

    return result;
}
