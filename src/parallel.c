#include "parallel.h"
#include <string.h>

/* ── Initialization ─────────────────────────────────────────────── */

void mechanism_selector_init(MechanismSelector *ms, Arena *arena,
                             InternTable *intern_tab,
                             AlgebraRegistry *algebra_reg,
                             TraitRegistry *trait_reg,
                             TypeChecker *type_checker,
                             ErrorList *errors) {
    ms->arena = arena;
    ms->intern_tab = intern_tab;
    ms->algebra_reg = algebra_reg;
    ms->trait_reg = trait_reg;
    ms->type_checker = type_checker;
    ms->errors = errors;
    ms->current_pure = false;
}

const char *parallel_strategy_name(ParallelStrategy s) {
    switch (s) {
        case PAR_OBLIGATE_SEQUENTIAL: return "OBLIGATE_SEQUENTIAL";
        case PAR_OPTIMAL_SEQUENTIAL:  return "OPTIMAL_SEQUENTIAL";
        case PAR_PTHREAD:             return "PTHREAD";
        case PAR_WORK_STEALING:       return "WORK_STEALING";
        case PAR_ATOMIC_REDUCTION:    return "ATOMIC_REDUCTION";
        case PAR_PARALLEL_PREFIX:     return "PARALLEL_PREFIX";
        case PAR_SIMD:                return "SIMD";
        case PAR_AMX:                 return "AMX";
        case PAR_GPU:                 return "GPU";
        case PAR_PIPELINE:            return "PIPELINE";
        case PAR_SPECULATIVE:         return "SPECULATIVE";
    }
    return "UNKNOWN";
}

/* ── Identity Lookup ────────────────────────────────────────────── */

ASTNode *parallel_find_identity(MechanismSelector *ms, const char *fn_name,
                                const char *concrete_type) {
    if (!ms->trait_reg) return NULL;
    const char *fn_interned = intern_cstr(ms->intern_tab, fn_name);
    const char *ty_interned = intern_cstr(ms->intern_tab, concrete_type);

    for (int i = 0; i < ms->trait_reg->impls.count; i++) {
        TraitImpl *impl = ms->trait_reg->impls.items[i];
        if (impl->concrete_type != ty_interned) continue;

        /* Search identity entries in this impl */
        for (int j = 0; j < impl->identity_count; j++) {
            if (impl->identities[j].fn_name == fn_interned)
                return impl->identities[j].value;
        }
    }
    return NULL;
}

/* ── Annotation Helpers ─────────────────────────────────────────── */

static ParallelAnnotation *make_annotation(MechanismSelector *ms, ParallelStrategy strategy) {
    ParallelAnnotation *ann = (ParallelAnnotation *)arena_alloc(ms->arena, sizeof(ParallelAnnotation));
    ann->strategy = strategy;
    ann->reduction_fn = NULL;
    ann->identity_value = NULL;
    ann->is_pure = ms->current_pure;
    ann->estimated_iterations = -1;
    return ann;
}

/* ── Dependency Analysis ────────────────────────────────────────── */

/* Collect all variable names written (assigned/set) within a subtree. */
static void collect_writes(ASTNode *node, StrList *writes) {
    if (!node) return;
    switch (node->kind) {
        case NODE_ASSIGN:
            da_push(writes, node->assign.assign_name);
            break;
        case NODE_CALL:
            if (strcmp(node->call.call_name, "set") == 0 && node->call.args.count >= 3) {
                /* set target key val — target is being written */
                if (node->call.args.items[0]->kind == NODE_IDENT)
                    da_push(writes, node->call.args.items[0]->ident.ident);
            }
            for (int i = 0; i < node->call.args.count; i++)
                collect_writes(node->call.args.items[i], writes);
            break;
        case NODE_BLOCK:
        case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                collect_writes(node->block.stmts.items[i], writes);
            break;
        case NODE_IF:
            collect_writes(node->if_stmt.then_body, writes);
            collect_writes(node->if_stmt.else_body, writes);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                collect_writes(node->if_stmt.elifs.items[i], writes);
            break;
        case NODE_FOR:
            collect_writes(node->for_stmt.for_body, writes);
            break;
        case NODE_WHILE:
            collect_writes(node->while_stmt.while_body, writes);
            break;
        default:
            break;
    }
}

/* Collect all variable names read within a subtree (excluding the loop var). */
static void collect_reads(ASTNode *node, StrList *reads, const char *exclude) {
    if (!node) return;
    switch (node->kind) {
        case NODE_IDENT:
            if (node->ident.ident != exclude)
                da_push(reads, node->ident.ident);
            break;
        case NODE_CALL:
            for (int i = 0; i < node->call.args.count; i++)
                collect_reads(node->call.args.items[i], reads, exclude);
            break;
        case NODE_BLOCK:
        case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                collect_reads(node->block.stmts.items[i], reads, exclude);
            break;
        case NODE_IF:
            collect_reads(node->if_stmt.condition, reads, exclude);
            collect_reads(node->if_stmt.then_body, reads, exclude);
            collect_reads(node->if_stmt.else_body, reads, exclude);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                collect_reads(node->if_stmt.elifs.items[i], reads, exclude);
            break;
        case NODE_FOR:
            collect_reads(node->for_stmt.iterable, reads, exclude);
            collect_reads(node->for_stmt.for_body, reads, exclude);
            break;
        case NODE_WHILE:
            collect_reads(node->while_stmt.condition, reads, exclude);
            collect_reads(node->while_stmt.while_body, reads, exclude);
            break;
        case NODE_ASSIGN:
            collect_reads(node->assign.value, reads, exclude);
            break;
        case NODE_LET:
        case NODE_VAR:
            collect_reads(node->binding.value, reads, exclude);
            break;
        case NODE_RETURN:
            collect_reads(node->ret.value, reads, exclude);
            break;
        case NODE_CHAIN:
            for (int i = 0; i < node->chain.chain_operands.count; i++)
                collect_reads(node->chain.chain_operands.items[i], reads, exclude);
            break;
        default:
            break;
    }
}

static bool str_in_list(const char *s, StrList *list) {
    for (int i = 0; i < list->count; i++)
        if (list->items[i] == s) return true;
    return false;
}

/* Check if a for-loop body has cross-iteration write→read dependency.
 * Returns true if there IS a dependency (obligate sequential). */
static bool has_cross_iteration_dependency(ASTNode *for_node) {
    StrList writes, reads;
    da_init(&writes);
    da_init(&reads);

    collect_writes(for_node->for_stmt.for_body, &writes);
    collect_reads(for_node->for_stmt.for_body, &reads, for_node->for_stmt.var_name);

    /* If any written variable is also read, and it's not the loop variable,
     * there's a potential cross-iteration dependency. */
    bool has_dep = false;
    for (int i = 0; i < writes.count; i++) {
        if (str_in_list(writes.items[i], &reads)) {
            has_dep = true;
            break;
        }
    }

    da_free(&writes);
    da_free(&reads);
    return has_dep;
}

/* ── Reduction Detection ────────────────────────────────────────── */

/* Detect the pattern:
 *   var accum = init
 *   for x in coll begin
 *     assign accum combine(accum, ...)  OR  if cmp(...) assign accum ...
 *   end
 * Returns the combining function name if detected, NULL otherwise. */
static const char *detect_reduction_fn(ASTNode *for_body) {
    if (!for_body) return NULL;

    NodeList *stmts = NULL;
    if (for_body->kind == NODE_BLOCK || for_body->kind == NODE_BEGIN_END)
        stmts = &for_body->block.stmts;
    else
        return NULL;

    /* Look for assign statements with a call on the RHS */
    for (int i = 0; i < stmts->count; i++) {
        ASTNode *stmt = stmts->items[i];
        if (stmt->kind == NODE_ASSIGN && stmt->assign.value &&
            stmt->assign.value->kind == NODE_CALL) {
            return stmt->assign.value->call.call_name;
        }
        /* Pattern: if greater/less ... begin assign accum ... end
         * This is a max/min reduction pattern */
        if (stmt->kind == NODE_IF && stmt->if_stmt.then_body) {
            ASTNode *then = stmt->if_stmt.then_body;
            NodeList *then_stmts = NULL;
            if (then->kind == NODE_BLOCK || then->kind == NODE_BEGIN_END)
                then_stmts = &then->block.stmts;

            if (then_stmts) {
                for (int j = 0; j < then_stmts->count; j++) {
                    if (then_stmts->items[j]->kind == NODE_ASSIGN) {
                        /* The reduction fn is the condition fn */
                        if (stmt->if_stmt.condition &&
                            stmt->if_stmt.condition->kind == NODE_CALL)
                            return stmt->if_stmt.condition->call.call_name;
                    }
                }
            }
        }
    }
    return NULL;
}

/* ── Trait Queries ──────────────────────────────────────────────── */

static bool fn_has_trait(MechanismSelector *ms, const char *fn_name, const char *trait_name) {
    if (!ms->trait_reg) return false;
    /* Check all trait defs for one with matching name that declares this fn */
    const char *tn = intern_cstr(ms->intern_tab, trait_name);
    const char *fn = intern_cstr(ms->intern_tab, fn_name);

    TraitDef *def = trait_find_def(ms->trait_reg, trait_name);
    if (!def) return false;

    /* Check if the trait has a method with this fn name */
    for (int i = 0; i < def->method_sigs.count; i++) {
        ASTNode *sig = def->method_sigs.items[i];
        if (sig->kind == NODE_FN_DECL && sig->fn_decl.fn_name == fn)
            return true;
    }
    /* Also check: is there an impl of this trait that contains this fn? */
    for (int i = 0; i < ms->trait_reg->impls.count; i++) {
        TraitImpl *impl = ms->trait_reg->impls.items[i];
        if (impl->trait_name != tn) continue;
        for (int j = 0; j < impl->methods.count; j++) {
            ASTNode *m = impl->methods.items[j];
            if (m->kind == NODE_FN_DECL && m->fn_decl.fn_name == fn)
                return true;
        }
    }
    return false;
}

/* ── Distributive Check ─────────────────────────────────────────── */

/* Check if the current algebra has a distributive declaration for two fns.
 * Also checks if both map to standard multiply/add for AMX eligibility. */
static bool has_distributive_amx(MechanismSelector *ms) {
    for (int i = 0; i < ms->algebra_reg->algebras.count; i++) {
        AlgebraEntry *alg = ms->algebra_reg->algebras.items[i];
        for (int j = 0; j < alg->distributive_count; j++) {
            DistributiveEntry *de = &alg->distributives[j];
            /* For AMX: outer must be standard multiply, inner must be standard add.
             * In tropical algebra, multiply = ordinary add, add = max — NOT AMX-compatible.
             * We'd need to check that the actual implementations map to hardware ops.
             * For now, just record that distributive exists; AMX check is a stub. */
            (void)de;
        }
    }
    return false; /* AMX not yet implemented */
}

/* ── Iteration Count Estimation ─────────────────────────────────── */

static int estimate_iterations(ASTNode *iterable) {
    /* Check for range(a, b) with literal args */
    ASTNode *inner = iterable;
    if (inner->kind == NODE_BEGIN_END && inner->block.stmts.count == 1)
        inner = inner->block.stmts.items[0];

    if (inner->kind == NODE_CALL && strcmp(inner->call.call_name, "range") == 0 &&
        inner->call.args.count >= 2) {
        ASTNode *lo = inner->call.args.items[0];
        ASTNode *hi = inner->call.args.items[1];
        if (lo->kind == NODE_INT_LIT && hi->kind == NODE_INT_LIT) {
            int64_t count = hi->int_lit.int_val - lo->int_lit.int_val;
            return (int)(count > 0 ? count : 0);
        }
    }
    return -1; /* unknown */
}

/* ── Work Uniformity Check ──────────────────────────────────────── */

/* Returns true if work-per-iteration might be non-uniform */
static bool has_non_uniform_work(ASTNode *for_body) {
    if (!for_body) return false;
    NodeList *stmts = NULL;
    if (for_body->kind == NODE_BLOCK || for_body->kind == NODE_BEGIN_END)
        stmts = &for_body->block.stmts;
    if (!stmts) return false;

    for (int i = 0; i < stmts->count; i++) {
        ASTNode *s = stmts->items[i];
        /* Inner for loop = variable work */
        if (s->kind == NODE_FOR) return true;
        /* Comprehension with filter = variable work */
        if (s->kind == NODE_COMPREHENSION && s->comprehension.comp_filter)
            return true;
    }
    return false;
}

/* ── Strategy Selection for a Single Loop ───────────────────────── */

#define SMALL_LOOP_THRESHOLD 16

static void select_for_strategy(MechanismSelector *ms, ASTNode *node) {
    int iters = estimate_iterations(node->for_stmt.iterable);
    const char *reduction = detect_reduction_fn(node->for_stmt.for_body);
    bool cross_dep = has_cross_iteration_dependency(node);

    bool is_assoc = reduction && fn_has_trait(ms, reduction, "Associative");
    bool is_commut = reduction && fn_has_trait(ms, reduction, "Commutative");
    bool is_idemp = reduction && fn_has_trait(ms, reduction, "Idempotent");
    bool pure = ms->current_pure;
    bool non_uniform = has_non_uniform_work(node->for_stmt.for_body);

    ParallelAnnotation *ann = make_annotation(ms, PAR_OBLIGATE_SEQUENTIAL);
    ann->estimated_iterations = iters;
    ann->reduction_fn = reduction;

    if (reduction) {
        /* Look up identity for the reduction function */
        ann->identity_value = parallel_find_identity(ms, reduction, "int");
    }

    if (cross_dep && !is_assoc) {
        /* True dependency with no algebraic escape */
        ann->strategy = PAR_OBLIGATE_SEQUENTIAL;
    } else if (iters >= 0 && iters < SMALL_LOOP_THRESHOLD) {
        ann->strategy = PAR_OPTIMAL_SEQUENTIAL;
    } else if (pure && is_assoc && is_commut && is_idemp) {
        /* Best case: pure + all traits → GPU candidate */
        ann->strategy = PAR_GPU;
    } else if (is_assoc && is_commut) {
        /* Associative + Commutative → atomic reduction */
        ann->strategy = PAR_ATOMIC_REDUCTION;
    } else if (is_assoc) {
        /* Associative only → parallel prefix or SIMD */
        ann->strategy = PAR_SIMD;
    } else if (!cross_dep && non_uniform) {
        /* Independent iterations, non-uniform work → work stealing */
        ann->strategy = PAR_WORK_STEALING;
    } else if (!cross_dep) {
        /* Independent iterations, uniform work → pthread */
        ann->strategy = PAR_PTHREAD;
    } else {
        ann->strategy = PAR_OBLIGATE_SEQUENTIAL;
    }

    node->parallel = ann;
}

/* ── Strategy Selection for Comprehensions ──────────────────────── */

static void select_comprehension_strategy(MechanismSelector *ms, ASTNode *node) {
    bool has_filter = (node->comprehension.comp_filter != NULL);
    bool pure = ms->current_pure;

    ParallelAnnotation *ann = make_annotation(ms, PAR_PTHREAD);

    if (has_filter) {
        /* Filter means unpredictable work → work stealing */
        ann->strategy = PAR_WORK_STEALING;
    } else if (pure) {
        /* Pure + no filter → could go SIMD or GPU */
        ann->strategy = PAR_SIMD;
    } else {
        ann->strategy = PAR_PTHREAD;
    }

    node->parallel = ann;
}

/* ── Recursive AST Walk ─────────────────────────────────────────── */

static void select_node(MechanismSelector *ms, ASTNode *node) {
    if (!node) return;

    switch (node->kind) {
        case NODE_ALGEBRA:
        case NODE_LIBRARY: {
            /* Check purity flag */
            bool saved_pure = ms->current_pure;
            ms->current_pure = node->algebra.is_pure;

            for (int i = 0; i < node->algebra.declarations.count; i++)
                select_node(ms, node->algebra.declarations.items[i]);

            ms->current_pure = saved_pure;
            break;
        }

        case NODE_PROGRAM:
            for (int i = 0; i < node->program.top_level.count; i++)
                select_node(ms, node->program.top_level.items[i]);
            break;

        case NODE_FN_DECL:
            select_node(ms, node->fn_decl.body);
            break;

        case NODE_FOR:
            /* Analyze this loop */
            select_for_strategy(ms, node);
            /* Recurse into body for nested loops */
            select_node(ms, node->for_stmt.for_body);
            break;

        case NODE_COMPREHENSION:
            select_comprehension_strategy(ms, node);
            /* Recurse into transform/filter */
            select_node(ms, node->comprehension.comp_transform);
            select_node(ms, node->comprehension.comp_filter);
            break;

        case NODE_BLOCK:
        case NODE_BEGIN_END:
            for (int i = 0; i < node->block.stmts.count; i++)
                select_node(ms, node->block.stmts.items[i]);
            break;

        case NODE_IF:
            select_node(ms, node->if_stmt.then_body);
            select_node(ms, node->if_stmt.else_body);
            for (int i = 0; i < node->if_stmt.elifs.count; i++)
                select_node(ms, node->if_stmt.elifs.items[i]);
            break;

        case NODE_WHILE:
            select_node(ms, node->while_stmt.while_body);
            break;

        case NODE_IMPLEMENT:
            for (int i = 0; i < node->implement.methods.count; i++)
                select_node(ms, node->implement.methods.items[i]);
            break;

        case NODE_IMPORT:
            for (int i = 0; i < node->import_decl.declarations.count; i++)
                select_node(ms, node->import_decl.declarations.items[i]);
            break;

        case NODE_USE:
            select_node(ms, node->use_block.body);
            break;

        case NODE_MATCH:
            for (int i = 0; i < node->match_stmt.cases.count; i++)
                select_node(ms, node->match_stmt.cases.items[i]);
            break;

        case NODE_CASE:
            select_node(ms, node->case_branch.case_body);
            break;

        case NODE_DEFAULT:
            select_node(ms, node->default_branch.default_body);
            break;

        case NODE_LAMBDA:
            select_node(ms, node->lambda.lambda_body);
            break;

        default:
            break;
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

void mechanism_select(MechanismSelector *ms, ASTNode *program) {
    select_node(ms, program);
}
