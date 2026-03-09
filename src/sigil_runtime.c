#include "sigil_runtime.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ── Closure ─────────────────────────────────────────────────────── */

SigilClosure *sigil_closure_new(void *fn_ptr, int capture_count) {
    SigilClosure *cl = (SigilClosure *)calloc(1, sizeof(SigilClosure));
    cl->fn_ptr = fn_ptr;
    cl->capture_count = capture_count;
    if (capture_count > 0)
        cl->captures = (SigilVal *)calloc(capture_count, sizeof(SigilVal));
    else
        cl->captures = NULL;
    return cl;
}

void sigil_closure_set_capture(SigilClosure *cl, int index, SigilVal val) {
    if (index >= 0 && index < cl->capture_count)
        cl->captures[index] = val;
}

/* ── Map Hashing ─────────────────────────────────────────────────── */

static uint32_t hash_val(SigilVal v) {
    switch (v.kind) {
        case SIGIL_VAL_BOOL:  return v.b ? 1 : 0;
        case SIGIL_VAL_INT:   return (uint32_t)(v.i ^ (v.i >> 32));
        case SIGIL_VAL_FLOAT: { uint64_t bits; memcpy(&bits, &v.f, 8); return (uint32_t)(bits ^ (bits >> 32)); }
        case SIGIL_VAL_CHAR:  return v.c;
        case SIGIL_VAL_MAP:     return (uint32_t)(uintptr_t)v.m;
        case SIGIL_VAL_CLOSURE: return (uint32_t)(uintptr_t)v.cl;
    }
    return 0;
}

static bool val_eq(SigilVal a, SigilVal b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case SIGIL_VAL_BOOL:  return a.b == b.b;
        case SIGIL_VAL_INT:   return a.i == b.i;
        case SIGIL_VAL_FLOAT: return a.f == b.f;
        case SIGIL_VAL_CHAR:  return a.c == b.c;
        case SIGIL_VAL_MAP:     return a.m == b.m;
        case SIGIL_VAL_CLOSURE: return a.cl == b.cl;
    }
    return false;
}

/* ── Map Operations ──────────────────────────────────────────────── */

#define MAP_INIT_CAP 16

SigilMap *sigil_map_new(void) {
    SigilMap *m = (SigilMap *)calloc(1, sizeof(SigilMap));
    m->capacity = MAP_INIT_CAP;
    m->buckets = (SigilMapBucket *)calloc(MAP_INIT_CAP, sizeof(SigilMapBucket));
    m->ref_count = 1;
    return m;
}

void sigil_map_retain(SigilMap *m) {
    if (m) m->ref_count++;
}

void sigil_map_release(SigilMap *m) {
    if (!m) return;
    if (--m->ref_count <= 0) {
        free(m->buckets);
        free(m);
    }
}

static int map_find_slot(SigilMapBucket *buckets, int cap, SigilVal key) {
    uint32_t h = hash_val(key);
    int idx = (int)(h % (uint32_t)cap);
    for (int i = 0; i < cap; i++) {
        int slot = (idx + i) % cap;
        if (!buckets[slot].occupied || val_eq(buckets[slot].key, key))
            return slot;
    }
    return -1; /* full — shouldn't happen if we resize */
}

static void map_resize(SigilMap *m) {
    int new_cap = m->capacity * 2;
    SigilMapBucket *new_buckets = (SigilMapBucket *)calloc(new_cap, sizeof(SigilMapBucket));
    for (int i = 0; i < m->capacity; i++) {
        if (m->buckets[i].occupied) {
            int slot = map_find_slot(new_buckets, new_cap, m->buckets[i].key);
            new_buckets[slot] = m->buckets[i];
        }
    }
    free(m->buckets);
    m->buckets = new_buckets;
    m->capacity = new_cap;
}

void sigil_map_set(SigilMap *m, SigilVal key, SigilVal val) {
    if (m->count * 2 >= m->capacity) map_resize(m);
    int slot = map_find_slot(m->buckets, m->capacity, key);
    if (!m->buckets[slot].occupied) m->count++;
    m->buckets[slot].key = key;
    m->buckets[slot].val = val;
    m->buckets[slot].occupied = true;
}

SigilVal sigil_map_get(SigilMap *m, SigilVal key) {
    int slot = map_find_slot(m->buckets, m->capacity, key);
    if (slot >= 0 && m->buckets[slot].occupied)
        return m->buckets[slot].val;
    return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
}

bool sigil_map_has(SigilMap *m, SigilVal key) {
    int slot = map_find_slot(m->buckets, m->capacity, key);
    return slot >= 0 && m->buckets[slot].occupied;
}

int sigil_map_count(SigilMap *m) {
    return m ? m->count : 0;
}

void sigil_map_remove(SigilMap *m, SigilVal key) {
    int slot = map_find_slot(m->buckets, m->capacity, key);
    if (slot >= 0 && m->buckets[slot].occupied) {
        m->buckets[slot].occupied = false;
        m->count--;
        /* Re-insert following entries (linear probing deletion) */
        int idx = (slot + 1) % m->capacity;
        while (m->buckets[idx].occupied) {
            SigilMapBucket tmp = m->buckets[idx];
            m->buckets[idx].occupied = false;
            m->count--;
            sigil_map_set(m, tmp.key, tmp.val);
            idx = (idx + 1) % m->capacity;
        }
    }
}

/* ── Iterator ────────────────────────────────────────────────────── */

SigilIter sigil_map_iter(SigilMap *m) {
    return (SigilIter){.kind = ITER_MAP, .map = m, .index = 0, .range_end = 0};
}

SigilIter sigil_range(int64_t start, int64_t end) {
    return (SigilIter){.kind = ITER_RANGE, .map = NULL, .index = (int)start, .range_end = end};
}

bool sigil_iter_has_next(SigilIter *it) {
    if (it->kind == ITER_RANGE)
        return (int64_t)it->index < it->range_end;
    while (it->index < it->map->capacity) {
        if (it->map->buckets[it->index].occupied) return true;
        it->index++;
    }
    return false;
}

SigilVal sigil_iter_next(SigilIter *it) {
    if (it->kind == ITER_RANGE)
        return sigil_val_int((int64_t)it->index++);
    while (it->index < it->map->capacity) {
        if (it->map->buckets[it->index].occupied)
            return it->map->buckets[it->index++].key;
        it->index++;
    }
    return (SigilVal){.kind = SIGIL_VAL_INT, .i = 0};
}

/* ── I/O ─────────────────────────────────────────────────────────── */

void sigil_print_int(int64_t v)  { printf("%lld\n", (long long)v); }
void sigil_print_float(double v) { printf("%g\n", v); }
void sigil_print_bool(bool v)    { printf("%s\n", v ? "true" : "false"); }
void sigil_print_char(uint32_t v) {
    /* UTF-8 encode and print */
    char buf[5] = {0};
    if (v < 0x80) { buf[0] = (char)v; }
    else if (v < 0x800) { buf[0] = 0xC0 | (v >> 6); buf[1] = 0x80 | (v & 0x3F); }
    else if (v < 0x10000) { buf[0] = 0xE0 | (v >> 12); buf[1] = 0x80 | ((v >> 6) & 0x3F); buf[2] = 0x80 | (v & 0x3F); }
    else { buf[0] = 0xF0 | (v >> 18); buf[1] = 0x80 | ((v >> 12) & 0x3F); buf[2] = 0x80 | ((v >> 6) & 0x3F); buf[3] = 0x80 | (v & 0x3F); }
    printf("%s\n", buf);
}

void sigil_print_string(SigilMap *m) {
    /* Print map<int,char> as a UTF-8 string */
    if (!m) { printf("\n"); return; }
    for (int64_t i = 0; i < (int64_t)m->count; i++) {
        SigilVal v = sigil_map_get(m, sigil_val_int(i));
        uint32_t cp = v.c;
        char buf[5] = {0};
        if (cp < 0x80) { buf[0] = (char)cp; }
        else if (cp < 0x800) { buf[0] = 0xC0 | (cp >> 6); buf[1] = 0x80 | (cp & 0x3F); }
        else if (cp < 0x10000) { buf[0] = 0xE0 | (cp >> 12); buf[1] = 0x80 | ((cp >> 6) & 0x3F); buf[2] = 0x80 | (cp & 0x3F); }
        else { buf[0] = 0xF0 | (cp >> 18); buf[1] = 0x80 | ((cp >> 12) & 0x3F); buf[2] = 0x80 | ((cp >> 6) & 0x3F); buf[3] = 0x80 | (cp & 0x3F); }
        printf("%s", buf);
    }
    printf("\n");
}

/* ── String Concat ───────────────────────────────────────────────── */

SigilMap *sigil_string_concat(SigilMap *a, SigilMap *b) {
    SigilMap *result = sigil_map_new();
    int64_t idx = 0;
    if (a) {
        for (int64_t i = 0; i < (int64_t)a->count; i++) {
            SigilVal v = sigil_map_get(a, sigil_val_int(i));
            sigil_map_set(result, sigil_val_int(idx++), v);
        }
    }
    if (b) {
        for (int64_t i = 0; i < (int64_t)b->count; i++) {
            SigilVal v = sigil_map_get(b, sigil_val_int(i));
            sigil_map_set(result, sigil_val_int(idx++), v);
        }
    }
    return result;
}

/* ── Map Copy ────────────────────────────────────────────────────── */

SigilMap *sigil_map_copy(SigilMap *m) {
    SigilMap *result = sigil_map_new();
    if (!m) return result;
    for (int i = 0; i < m->capacity; i++) {
        if (m->buckets[i].occupied)
            sigil_map_set(result, m->buckets[i].key, m->buckets[i].val);
    }
    return result;
}

/* ── Map Append ──────────────────────────────────────────────────── */

void sigil_map_append(SigilMap *m, SigilVal val) {
    int64_t idx = (int64_t)m->count;
    sigil_map_set(m, sigil_val_int(idx), val);
}

/* ── Map Keys/Values ─────────────────────────────────────────────── */

SigilMap *sigil_map_keys(SigilMap *m) {
    SigilMap *result = sigil_map_new();
    if (!m) return result;
    int64_t idx = 0;
    for (int i = 0; i < m->capacity; i++) {
        if (m->buckets[i].occupied)
            sigil_map_set(result, sigil_val_int(idx++), m->buckets[i].key);
    }
    return result;
}

SigilMap *sigil_map_values(SigilMap *m) {
    SigilMap *result = sigil_map_new();
    if (!m) return result;
    int64_t idx = 0;
    for (int i = 0; i < m->capacity; i++) {
        if (m->buckets[i].occupied)
            sigil_map_set(result, sigil_val_int(idx++), m->buckets[i].val);
    }
    return result;
}

/* ── Map Print ───────────────────────────────────────────────────── */

void sigil_print_map(SigilMap *m) {
    if (!m) { printf("{}\n"); return; }
    printf("{");
    bool first = true;
    for (int i = 0; i < m->capacity; i++) {
        if (!m->buckets[i].occupied) continue;
        if (!first) printf(", ");
        first = false;
        SigilVal k = m->buckets[i].key;
        SigilVal v = m->buckets[i].val;
        /* Print key */
        switch (k.kind) {
            case SIGIL_VAL_INT:   printf("%lld", (long long)k.i); break;
            case SIGIL_VAL_FLOAT: printf("%g", k.f); break;
            case SIGIL_VAL_BOOL:  printf("%s", k.b ? "true" : "false"); break;
            case SIGIL_VAL_CHAR:  printf("'%c'", (char)k.c); break;
            default: printf("?"); break;
        }
        printf(": ");
        /* Print value */
        switch (v.kind) {
            case SIGIL_VAL_INT:   printf("%lld", (long long)v.i); break;
            case SIGIL_VAL_FLOAT: printf("%g", v.f); break;
            case SIGIL_VAL_BOOL:  printf("%s", v.b ? "true" : "false"); break;
            case SIGIL_VAL_CHAR:  printf("'%c'", (char)v.c); break;
            case SIGIL_VAL_MAP:   printf("<map>"); break;
            case SIGIL_VAL_CLOSURE: printf("<closure>"); break;
        }
    }
    printf("}\n");
}

/* ── Type Conversions ────────────────────────────────────────────── */

int64_t sigil_to_int(double v) { return (int64_t)v; }
double sigil_to_float(int64_t v) { return (double)v; }

SigilMap *sigil_int_to_string(int64_t v) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return sigil_string_from_utf8(buf, len);
}

SigilMap *sigil_float_to_string(double v) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%g", v);
    return sigil_string_from_utf8(buf, len);
}

SigilMap *sigil_bool_to_string(bool v) {
    const char *s = v ? "true" : "false";
    return sigil_string_from_utf8(s, (int)strlen(s));
}

SigilMap *sigil_char_to_string(uint32_t v) {
    char buf[5] = {0};
    int len = 0;
    if (v < 0x80) { buf[0] = (char)v; len = 1; }
    else if (v < 0x800) { buf[0] = 0xC0 | (v >> 6); buf[1] = 0x80 | (v & 0x3F); len = 2; }
    else if (v < 0x10000) { buf[0] = 0xE0 | (v >> 12); buf[1] = 0x80 | ((v >> 6) & 0x3F); buf[2] = 0x80 | (v & 0x3F); len = 3; }
    else { buf[0] = 0xF0 | (v >> 18); buf[1] = 0x80 | ((v >> 12) & 0x3F); buf[2] = 0x80 | ((v >> 6) & 0x3F); buf[3] = 0x80 | (v & 0x3F); len = 4; }
    return sigil_string_from_utf8(buf, len);
}

/* ── String from UTF-8 ───────────────────────────────────────────── */

SigilMap *sigil_string_from_utf8(const char *s, int len) {
    SigilMap *m = sigil_map_new();
    int64_t idx = 0;
    int pos = 0;
    while (pos < len) {
        uint32_t cp;
        uint8_t b = (uint8_t)s[pos];
        int cplen;
        if (b < 0x80) { cp = b; cplen = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; cplen = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; cplen = 3; }
        else { cp = b & 0x07; cplen = 4; }
        for (int i = 1; i < cplen && pos + i < len; i++)
            cp = (cp << 6) | ((uint8_t)s[pos + i] & 0x3F);
        sigil_map_set(m, sigil_val_int(idx), sigil_val_char(cp));
        idx++;
        pos += cplen;
    }
    return m;
}

/* ── Row/Matrix Constructors ─────────────────────────────────────── */

SigilMap *sigil_row(SigilVal first, int _repeats_count, SigilVal *_repeats_data) {
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, sigil_val_int(0), first);
    for (int i = 0; i < _repeats_count; i++)
        sigil_map_set(m, sigil_val_int(i + 1), _repeats_data[i]);
    return m;
}

SigilMap *sigil_matrix(SigilVal first, int _repeats_count, SigilVal *_repeats_data) {
    SigilMap *m = sigil_map_new();
    sigil_map_set(m, sigil_val_int(0), first);
    for (int i = 0; i < _repeats_count; i++)
        sigil_map_set(m, sigil_val_int(i + 1), _repeats_data[i]);
    return m;
}
