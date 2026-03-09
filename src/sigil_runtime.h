#ifndef SIGIL_RUNTIME_H
#define SIGIL_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Closure ─────────────────────────────────────────────────────── */

typedef struct SigilClosure SigilClosure;

/* ── Value Types ─────────────────────────────────────────────────── */

typedef enum {
    SIGIL_VAL_BOOL,
    SIGIL_VAL_INT,
    SIGIL_VAL_FLOAT,
    SIGIL_VAL_CHAR,
    SIGIL_VAL_MAP,
    SIGIL_VAL_CLOSURE
} SigilValKind;

typedef struct SigilMap SigilMap;

typedef struct {
    SigilValKind kind;
    union {
        bool b;
        int64_t i;
        double f;
        uint32_t c;
        SigilMap *m;
        SigilClosure *cl;
    };
} SigilVal;

struct SigilClosure {
    void *fn_ptr;
    SigilVal *captures;
    int capture_count;
};

/* ── Map ─────────────────────────────────────────────────────────── */

typedef struct {
    SigilVal key;
    SigilVal val;
    bool occupied;
} SigilMapBucket;

struct SigilMap {
    int count;
    int capacity;
    SigilMapBucket *buckets;
    int ref_count;
};

SigilMap *sigil_map_new(void);
void sigil_map_retain(SigilMap *m);
void sigil_map_release(SigilMap *m);
SigilVal sigil_map_get(SigilMap *m, SigilVal key);
void sigil_map_set(SigilMap *m, SigilVal key, SigilVal val);
bool sigil_map_has(SigilMap *m, SigilVal key);
int sigil_map_count(SigilMap *m);
void sigil_map_remove(SigilMap *m, SigilVal key);

/* ── Boxing / Unboxing ───────────────────────────────────────────── */

static inline SigilVal sigil_val_int(int64_t v)   { return (SigilVal){.kind=SIGIL_VAL_INT,   .i=v}; }
static inline SigilVal sigil_val_float(double v)   { return (SigilVal){.kind=SIGIL_VAL_FLOAT, .f=v}; }
static inline SigilVal sigil_val_bool(bool v)      { return (SigilVal){.kind=SIGIL_VAL_BOOL,  .b=v}; }
static inline SigilVal sigil_val_char(uint32_t v)  { return (SigilVal){.kind=SIGIL_VAL_CHAR,  .c=v}; }
static inline SigilVal sigil_val_map(SigilMap *v)  { return (SigilVal){.kind=SIGIL_VAL_MAP,   .m=v}; }

static inline int64_t  sigil_unbox_int(SigilVal v)   { return v.i; }
static inline double   sigil_unbox_float(SigilVal v)  { return v.f; }
static inline bool     sigil_unbox_bool(SigilVal v)   { return v.b; }
static inline uint32_t sigil_unbox_char(SigilVal v)   { return v.c; }
static inline SigilMap* sigil_unbox_map(SigilVal v)   { return v.m; }
static inline SigilVal sigil_val_closure(SigilClosure *v) { return (SigilVal){.kind=SIGIL_VAL_CLOSURE, .cl=v}; }
static inline SigilClosure* sigil_unbox_closure(SigilVal v) { return v.cl; }

SigilClosure *sigil_closure_new(void *fn_ptr, int capture_count);
void sigil_closure_set_capture(SigilClosure *cl, int index, SigilVal val);

/* ── Iterator ────────────────────────────────────────────────────── */

typedef enum { ITER_MAP, ITER_RANGE } SigilIterKind;

typedef struct {
    SigilIterKind kind;
    SigilMap *map;
    int index;
    int64_t range_end;  /* for ITER_RANGE: exclusive upper bound */
} SigilIter;

SigilIter sigil_map_iter(SigilMap *m);
SigilIter sigil_range(int64_t start, int64_t end);
bool sigil_iter_has_next(SigilIter *it);
SigilVal sigil_iter_next(SigilIter *it);

/* ── I/O ─────────────────────────────────────────────────────────── */

void sigil_print_int(int64_t v);
void sigil_print_float(double v);
void sigil_print_bool(bool v);
void sigil_print_char(uint32_t v);
void sigil_print_string(SigilMap *m);

/* ── String Construction ─────────────────────────────────────────── */

SigilMap *sigil_string_from_utf8(const char *s, int len);

#endif /* SIGIL_RUNTIME_H */
