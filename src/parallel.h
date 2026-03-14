#ifndef SIGIL_PARALLEL_H
#define SIGIL_PARALLEL_H

#include "ast.h"
#include "algebra.h"
#include "traits.h"
#include "types.h"
#include "errors.h"

/* ── Parallel Strategy ──────────────────────────────────────────── */

typedef enum {
    PAR_OBLIGATE_SEQUENTIAL,     /* true data dependency, no algebraic escape */
    PAR_OPTIMAL_SEQUENTIAL,      /* parallelizable but below cost threshold */
    PAR_PTHREAD,                 /* coarse-grained OS thread parallelism */
    PAR_WORK_STEALING,           /* fine-grained GCD/libdispatch */
    PAR_ATOMIC_REDUCTION,        /* hardware atomic accumulation */
    PAR_PARALLEL_PREFIX,         /* Blelloch-style scan */
    PAR_SIMD,                    /* ARM NEON vectorized */
    PAR_AMX,                     /* Apple matrix coprocessor */
    PAR_GPU,                     /* Metal compute shaders */
    PAR_PIPELINE,                /* streaming producer-consumer */
    PAR_SPECULATIVE,             /* branch speculation */
} ParallelStrategy;

typedef struct {
    ParallelStrategy strategy;
    const char *reduction_fn;    /* if reduction: which fn (e.g., "add") */
    ASTNode *identity_value;     /* identity element for reduction, if known */
    bool is_pure;                /* whether enclosing algebra is pure */
    int estimated_iterations;    /* -1 if unknown */
} ParallelAnnotation;

/* ── Mechanism Selector ─────────────────────────────────────────── */

typedef struct {
    Arena *arena;
    InternTable *intern_tab;
    AlgebraRegistry *algebra_reg;
    TraitRegistry *trait_reg;
    TypeChecker *type_checker;
    ErrorList *errors;
    bool current_pure;           /* purity of the enclosing algebra */
} MechanismSelector;

void mechanism_selector_init(MechanismSelector *ms, Arena *arena,
                             InternTable *intern_tab,
                             AlgebraRegistry *algebra_reg,
                             TraitRegistry *trait_reg,
                             TypeChecker *type_checker,
                             ErrorList *errors);

/* Analyze the AST and annotate loops/comprehensions with parallel strategies. */
void mechanism_select(MechanismSelector *ms, ASTNode *program);

/* Human-readable name for a strategy (for emitter comments). */
const char *parallel_strategy_name(ParallelStrategy s);

/* Look up the identity value for a function implemented for a concrete type. */
ASTNode *parallel_find_identity(MechanismSelector *ms, const char *fn_name,
                                const char *concrete_type);

#endif /* SIGIL_PARALLEL_H */
