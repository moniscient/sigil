#include "sigil_thunk.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>

/* ── Bin 3: Thread-Parallel Execution ────────────────────────────── */

typedef struct {
    SigilThunk **thunks;
    int count;
    ThunkArena *arena;
    pthread_mutex_t *arena_lock;
} SliceCtx;

static void *slice_worker(void *arg) {
    SliceCtx *ctx = (SliceCtx *)arg;
    for (int i = 0; i < ctx->count; i++) {
        SigilThunk *t = ctx->thunks[i];
        if (!t) continue;
        int st = atomic_load_explicit(&t->state, memory_order_acquire);
        if (st == THUNK_COMPLETED) continue;

        /* Force children first */
        for (int j = 0; j < t->arity; j++) {
            if (t->args[j]) {
                int cst = atomic_load_explicit(&t->args[j]->state, memory_order_acquire);
                if (cst != THUNK_COMPLETED) {
                    pthread_mutex_lock(ctx->arena_lock);
                    thunk_force(t->args[j], ctx->arena);
                    pthread_mutex_unlock(ctx->arena_lock);
                }
            }
        }

        /* Use CAS-based thunk_force */
        pthread_mutex_lock(ctx->arena_lock);
        thunk_force(t, ctx->arena);
        pthread_mutex_unlock(ctx->arena_lock);
    }
    return NULL;
}

SigilVal execute_thread(SigilThunk *root, ThunkArena *arena, int core_count) {
    if (!root) return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
    if (atomic_load_explicit(&root->state, memory_order_acquire) == THUNK_COMPLETED)
        return root->value;

    /* Collect all pending thunks via BFS */
    int cap = 1024;
    SigilThunk **all = (SigilThunk **)malloc(cap * sizeof(SigilThunk *));
    int count = 0;

    /* Simple BFS collection */
    SigilThunk **queue = (SigilThunk **)malloc(cap * sizeof(SigilThunk *));
    int qhead = 0, qtail = 0;
    queue[qtail++] = root;

    while (qhead < qtail) {
        SigilThunk *t = queue[qhead++];
        if (!t) continue;
        if (count >= cap) {
            cap *= 2;
            all = (SigilThunk **)realloc(all, cap * sizeof(SigilThunk *));
            queue = (SigilThunk **)realloc(queue, cap * sizeof(SigilThunk *));
        }
        all[count++] = t;
        for (int i = 0; i < t->arity; i++) {
            if (t->args[i] && qtail < cap) {
                queue[qtail++] = t->args[i];
            }
        }
    }
    free(queue);

    /* Partition into slices */
    int nslices = core_count * 2;
    if (nslices > count) nslices = count;
    if (nslices < 1) nslices = 1;
    int per_slice = count / nslices;

    pthread_mutex_t arena_lock;
    pthread_mutex_init(&arena_lock, NULL);

    pthread_t *threads = (pthread_t *)malloc(nslices * sizeof(pthread_t));
    SliceCtx *slices = (SliceCtx *)malloc(nslices * sizeof(SliceCtx));

    /* Process bottom-up: reverse the BFS order */
    for (int i = 0; i < count / 2; i++) {
        SigilThunk *tmp = all[i];
        all[i] = all[count - 1 - i];
        all[count - 1 - i] = tmp;
    }

    for (int i = 0; i < nslices; i++) {
        int start = i * per_slice;
        int end = (i == nslices - 1) ? count : start + per_slice;
        slices[i].thunks = all + start;
        slices[i].count = end - start;
        slices[i].arena = arena;
        slices[i].arena_lock = &arena_lock;
        pthread_create(&threads[i], NULL, slice_worker, &slices[i]);
    }

    for (int i = 0; i < nslices; i++)
        pthread_join(threads[i], NULL);

    /* Fallback: force root if not yet completed */
    SigilVal result = (atomic_load_explicit(&root->state, memory_order_acquire) == THUNK_COMPLETED)
        ? root->value : thunk_force(root, arena);

    free(all);
    free(threads);
    free(slices);
    pthread_mutex_destroy(&arena_lock);

    return result;
}
