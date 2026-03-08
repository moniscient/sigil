#ifndef SIGIL_COMMON_H
#define SIGIL_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* ── Arena Allocator ─────────────────────────────────────────────── */

#define ARENA_BLOCK_SIZE (64 * 1024)

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    size_t capacity;
    char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
    ArenaBlock *current;
} Arena;

static inline ArenaBlock *arena_new_block(size_t min_size) {
    size_t cap = min_size > ARENA_BLOCK_SIZE ? min_size : ARENA_BLOCK_SIZE;
    ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    b->next = NULL;
    b->used = 0;
    b->capacity = cap;
    return b;
}

static inline void arena_init(Arena *a) {
    a->head = arena_new_block(ARENA_BLOCK_SIZE);
    a->current = a->head;
}

static inline void *arena_alloc(Arena *a, size_t size) {
    size = (size + 7) & ~(size_t)7; /* 8-byte alignment */
    if (a->current->used + size > a->current->capacity) {
        ArenaBlock *b = arena_new_block(size);
        a->current->next = b;
        a->current = b;
    }
    void *ptr = a->current->data + a->current->used;
    a->current->used += size;
    return ptr;
}

static inline void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = a->current = NULL;
}

/* ── String Interning ────────────────────────────────────────────── */

#define INTERN_TABLE_SIZE 4096

typedef struct InternEntry {
    struct InternEntry *next;
    uint32_t hash;
    uint32_t length;
    char str[];
} InternEntry;

typedef struct {
    InternEntry *buckets[INTERN_TABLE_SIZE];
    Arena arena;
} InternTable;

static inline uint32_t intern_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static inline void intern_init(InternTable *t) {
    memset(t->buckets, 0, sizeof(t->buckets));
    arena_init(&t->arena);
}

static inline void intern_free(InternTable *t) {
    arena_free(&t->arena);
}

static inline const char *intern(InternTable *t, const char *s, size_t len) {
    uint32_t h = intern_hash(s, len);
    uint32_t idx = h % INTERN_TABLE_SIZE;
    for (InternEntry *e = t->buckets[idx]; e; e = e->next) {
        if (e->hash == h && e->length == len && memcmp(e->str, s, len) == 0)
            return e->str;
    }
    InternEntry *e = (InternEntry *)arena_alloc(&t->arena, sizeof(InternEntry) + len + 1);
    e->hash = h;
    e->length = (uint32_t)len;
    memcpy(e->str, s, len);
    e->str[len] = '\0';
    e->next = t->buckets[idx];
    t->buckets[idx] = e;
    return e->str;
}

static inline const char *intern_cstr(InternTable *t, const char *s) {
    return intern(t, s, strlen(s));
}

/* ── Source Location ─────────────────────────────────────────────── */

typedef struct {
    const char *file;
    int line;
    int col;
    int offset;
} SrcLoc;

static inline SrcLoc srcloc(const char *file, int line, int col, int offset) {
    return (SrcLoc){file, line, col, offset};
}

/* ── Dynamic Array ───────────────────────────────────────────────── */

#define DA_INIT_CAP 16

#define DA_TYPEDEF(T, Name) \
    typedef struct { T *items; int count; int capacity; } Name;

#define da_init(da) do { \
    (da)->items = NULL; (da)->count = 0; (da)->capacity = 0; \
} while(0)

#define da_push(da, item) do { \
    if ((da)->count >= (da)->capacity) { \
        (da)->capacity = (da)->capacity ? (da)->capacity * 2 : DA_INIT_CAP; \
        (da)->items = realloc((da)->items, (da)->capacity * sizeof(*(da)->items)); \
    } \
    (da)->items[(da)->count++] = (item); \
} while(0)

#define da_free(da) do { \
    free((da)->items); (da)->items = NULL; (da)->count = (da)->capacity = 0; \
} while(0)

#endif /* SIGIL_COMMON_H */
