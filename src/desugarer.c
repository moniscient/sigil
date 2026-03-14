#include "desugarer.h"
#include <string.h>

void desugarer_init(Desugarer *d, Arena *arena, InternTable *intern_tab,
                    AlgebraRegistry *registry, TraitRegistry *trait_reg,
                    ErrorList *errors) {
    d->arena = arena;
    d->intern_tab = intern_tab;
    d->registry = registry;
    d->current_algebra = NULL;
    d->trait_reg = trait_reg;
    d->errors = errors;
}

/* ── Sigil Detection ─────────────────────────────────────────────── */

static bool has_sigils(ASTNode *node) {
    if (!node) return false;
    if (node->kind == NODE_SIGIL_EXPR) return true;
    if (node->kind == NODE_CALL) {
        for (int i = 0; i < node->call.args.count; i++)
            if (has_sigils(node->call.args.items[i])) return true;
    }
    if (node->kind == NODE_CHAIN) {
        for (int i = 0; i < node->chain.chain_operands.count; i++)
            if (has_sigils(node->chain.chain_operands.items[i])) return true;
    }
    if (node->kind == NODE_BLOCK || node->kind == NODE_BEGIN_END) {
        for (int i = 0; i < node->block.stmts.count; i++)
            if (has_sigils(node->block.stmts.items[i])) return true;
    }
    if (node->kind == NODE_LET || node->kind == NODE_VAR)
        return has_sigils(node->binding.value);
    if (node->kind == NODE_ASSIGN)
        return has_sigils(node->assign.value);
    if (node->kind == NODE_RETURN)
        return has_sigils(node->ret.value);
    if (node->kind == NODE_IF) {
        if (has_sigils(node->if_stmt.condition)) return true;
        if (has_sigils(node->if_stmt.then_body)) return true;
    }
    return false;
}

/* ── AST-to-Token Flattening ─────────────────────────────────────── */

static void flatten_node(Desugarer *d, ASTNode *node, TokenList *out) {
    if (!node) return;
    Token t;
    memset(&t, 0, sizeof(t));
    t.loc = node->loc;

    switch (node->kind) {
        case NODE_IDENT:
            t.kind = TOK_IDENT; t.text = node->ident.ident;
            da_push(out, t);
            break;
        case NODE_INT_LIT:
            t.kind = TOK_INT_LIT;
            t.text = intern_cstr(d->intern_tab, "0");
            t.int_val = node->int_lit.int_val;
            da_push(out, t);
            break;
        case NODE_FLOAT_LIT:
            t.kind = TOK_FLOAT_LIT;
            t.text = intern_cstr(d->intern_tab, "0");
            t.float_val = node->float_lit.float_val;
            da_push(out, t);
            break;
        case NODE_BOOL_LIT:
            t.kind = TOK_KEYWORD;
            t.text = intern_cstr(d->intern_tab, node->bool_lit.bool_val ? "true" : "false");
            da_push(out, t);
            break;
        case NODE_STRING_LIT:
            t.kind = TOK_STRING_LIT;
            t.text = node->string_lit.str_val;
            da_push(out, t);
            break;
        case NODE_SIGIL_EXPR:
            t.kind = TOK_SIGIL; t.text = node->sigil_expr.sigil;
            da_push(out, t);
            break;
        case NODE_CALL:
            t.kind = is_keyword(node->call.call_name) ? TOK_KEYWORD : TOK_IDENT;
            t.text = node->call.call_name;
            da_push(out, t);
            for (int i = 0; i < node->call.args.count; i++)
                flatten_node(d, node->call.args.items[i], out);
            break;
        case NODE_BEGIN_END:
            t.kind = TOK_DO; t.text = intern_cstr(d->intern_tab, "do");
            da_push(out, t);
            for (int i = 0; i < node->block.stmts.count; i++)
                flatten_node(d, node->block.stmts.items[i], out);
            t.kind = TOK_END; t.text = intern_cstr(d->intern_tab, "end");
            da_push(out, t);
            break;
        case NODE_BLOCK:
            t.kind = TOK_BEGIN; t.text = intern_cstr(d->intern_tab, "begin");
            da_push(out, t);
            for (int i = 0; i < node->block.stmts.count; i++)
                flatten_node(d, node->block.stmts.items[i], out);
            t.kind = TOK_END; t.text = intern_cstr(d->intern_tab, "end");
            da_push(out, t);
            break;
        case NODE_LET:
        case NODE_VAR:
            t.kind = TOK_KEYWORD;
            t.text = intern_cstr(d->intern_tab, node->kind == NODE_LET ? "let" : "var");
            da_push(out, t);
            t.kind = TOK_IDENT; t.text = node->binding.bind_name;
            da_push(out, t);
            flatten_node(d, node->binding.value, out);
            break;
        case NODE_ASSIGN:
            t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "assign");
            da_push(out, t);
            t.kind = TOK_IDENT; t.text = node->assign.assign_name;
            da_push(out, t);
            flatten_node(d, node->assign.value, out);
            break;
        case NODE_RETURN:
            t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "return");
            da_push(out, t);
            flatten_node(d, node->ret.value, out);
            break;
        case NODE_IF:
            t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "if");
            da_push(out, t);
            flatten_node(d, node->if_stmt.condition, out);
            flatten_node(d, node->if_stmt.then_body, out);
            for (int i = 0; i < node->if_stmt.elifs.count; i += 2) {
                t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "elif");
                da_push(out, t);
                flatten_node(d, node->if_stmt.elifs.items[i], out);
                flatten_node(d, node->if_stmt.elifs.items[i+1], out);
            }
            if (node->if_stmt.else_body) {
                t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "else");
                da_push(out, t);
                flatten_node(d, node->if_stmt.else_body, out);
            }
            break;
        case NODE_WHILE:
            t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "while");
            da_push(out, t);
            flatten_node(d, node->while_stmt.condition, out);
            flatten_node(d, node->while_stmt.while_body, out);
            break;
        case NODE_FOR:
            t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "for");
            da_push(out, t);
            t.kind = TOK_IDENT; t.text = node->for_stmt.var_name;
            da_push(out, t);
            t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "in");
            da_push(out, t);
            flatten_node(d, node->for_stmt.iterable, out);
            flatten_node(d, node->for_stmt.for_body, out);
            break;
        case NODE_AS_EXPR:
            flatten_node(d, node->as_expr.source, out);
            t.kind = TOK_KEYWORD; t.text = intern_cstr(d->intern_tab, "as");
            da_push(out, t);
            t.kind = TOK_IDENT; t.text = node->as_expr.target_algebra;
            da_push(out, t);
            break;
        default:
            break;
    }
}

/* ── Precedence-Climbing Expression Parser ───────────────────────── */

typedef struct {
    Desugarer *d;
    Token *tokens;
    int count;
    int pos;
} ExprParser;

static ASTNode *expr_parse_prec(ExprParser *ep, int min_prec);
static ASTNode *ep_parse_use_stmt(ExprParser *ep);
static ASTNode *ep_parse_kw_call(ExprParser *ep, const char *name, SrcLoc loc);

static Token *ep_cur(ExprParser *ep) {
    if (ep->pos >= ep->count) return NULL;
    return &ep->tokens[ep->pos];
}

static Token *ep_eat(ExprParser *ep) {
    if (ep->pos >= ep->count) return NULL;
    return &ep->tokens[ep->pos++];
}

static bool ep_at_eof(ExprParser *ep) {
    return ep->pos >= ep->count;
}

static bool ep_at_text(ExprParser *ep, const char *text) {
    if (ep_at_eof(ep)) return false;
    return strcmp(ep->tokens[ep->pos].text, text) == 0;
}

/* Check if a bracketed binding has a parameter before the opening sigil */
static bool binding_has_pre_bracket_param(SigilBinding *b) {
    for (int i = 0; i < b->pattern.count; i++) {
        if (b->pattern.items[i].kind == PAT_SIGIL) return false; /* hit sigil first */
        if (b->pattern.items[i].kind == PAT_PARAM) return true;
    }
    return false;
}

/* Check if a bracketed binding's pattern contains REPEATS */
static bool binding_has_repeats(SigilBinding *b) {
    for (int i = 0; i < b->pattern.count; i++) {
        if (b->pattern.items[i].kind == PAT_REPEATS) return true;
    }
    return false;
}

/* Find the closing sigil for a bracketed binding.
 * For patterns with trailing sigils (like set: [ , ] =), the close is the
 * bracket closer, not the last sigil. We find it by looking at the pattern:
 * it's the sigil that comes after all inner params and before any trailing. */
static const char *binding_close_sigil(SigilBinding *b) {
    /* For a pattern like: [param] SIGIL("[") PARAM SIGIL(",") PARAM SIGIL("]") SIGIL("=") PARAM
     * The close bracket is "]". We find it: it's the first sigil that appears
     * after at least one inner param and before a non-separator sigil or end. */
    /* Simplified: for patterns with pre-bracket param, skip to first sigil,
     * then the close is the last sigil before any trailing sigil+param pair. */
    int open_idx = -1;
    for (int i = 0; i < b->pattern.count; i++) {
        if (b->pattern.items[i].kind == PAT_SIGIL) {
            if (open_idx < 0) { open_idx = i; continue; }
            /* Check: is the next non-repeats element a PARAM followed by end or nothing? */
            /* Actually, just look for bracket-like closure pattern */
        }
    }
    /* Heuristic: scan from the end. The close bracket is the last sigil that
     * is NOT followed by a PARAM (since trailing sigils like = are followed by params). */
    for (int i = b->pattern.count - 1; i >= 0; i--) {
        if (b->pattern.items[i].kind == PAT_SIGIL) {
            /* Check if this sigil is followed by a param (trailing) */
            bool has_trailing_param = false;
            for (int j = i + 1; j < b->pattern.count; j++) {
                if (b->pattern.items[j].kind == PAT_PARAM) { has_trailing_param = true; break; }
                if (b->pattern.items[j].kind == PAT_SIGIL) break;
            }
            if (!has_trailing_param) return b->pattern.items[i].sigil;
        }
    }
    /* Fallback: last sigil */
    return b->all_sigils.items[b->all_sigils.count - 1];
}

/* Try to match a bracketed pattern. left may be NULL for standalone brackets. */
static ASTNode *ep_try_bracketed(ExprParser *ep, ASTNode *left) {
    if (ep_at_eof(ep)) return NULL;
    Token *t = ep_cur(ep);
    if (!t || t->kind != TOK_SIGIL) return NULL;
    if (!ep->d->current_algebra) return NULL;

    /* Try each bracketed binding, preferring longer matches (more sigils).
     * Use backtracking: save position, try, restore if trailing doesn't match. */
    ASTNode *best_result = NULL;
    int best_end_pos = -1;
    int best_sigil_count = -1;

    for (int i = 0; i < ep->d->current_algebra->bindings.count; i++) {
        SigilBinding *b = &ep->d->current_algebra->bindings.items[i];
        if (b->fixity != FIXITY_BRACKETED) continue;
        if (b->all_sigils.count < 2) continue;

        /* Check opening sigil match */
        const char *open_sigil = NULL;
        if (binding_has_pre_bracket_param(b)) {
            if (!left) continue; /* needs left operand but none provided */
            /* Opening sigil is the first sigil in pattern */
            for (int pi = 0; pi < b->pattern.count; pi++) {
                if (b->pattern.items[pi].kind == PAT_SIGIL) {
                    open_sigil = b->pattern.items[pi].sigil;
                    break;
                }
            }
        } else {
            if (left) continue; /* standalone but we have left */
            open_sigil = b->all_sigils.items[0];
        }

        if (!open_sigil || open_sigil != t->text) continue;

        /* Save position for backtracking */
        int saved_pos = ep->pos;
        SrcLoc loc = t->loc;
        ep_eat(ep); /* consume opening bracket */

        ASTNode *call = ast_new(ep->d->arena, NODE_CALL, loc);
        call->call.call_name = b->fn_name;
        da_init(&call->call.args);
        if (left) da_push(&call->call.args, left);

        /* Find the actual close bracket sigil */
        const char *close_sigil = binding_close_sigil(b);

        /* Determine separator sigils (everything between open and close that's a sigil in pattern) */
        const char *sep_sigil = NULL;
        bool found_open = false;
        for (int pi = 0; pi < b->pattern.count; pi++) {
            if (b->pattern.items[pi].kind == PAT_SIGIL && b->pattern.items[pi].sigil == open_sigil) {
                found_open = true;
                continue;
            }
            if (!found_open) continue;
            if (b->pattern.items[pi].kind == PAT_SIGIL && b->pattern.items[pi].sigil == close_sigil)
                break;
            if (b->pattern.items[pi].kind == PAT_SIGIL)
                sep_sigil = b->pattern.items[pi].sigil;
        }

        /* Parse arguments inside brackets */
        bool parse_ok = true;
        bool first_arg = true;
        while (!ep_at_eof(ep)) {
            Token *ct = ep_cur(ep);
            if (!ct) { parse_ok = false; break; }
            if (ct->kind == TOK_SIGIL && ct->text == close_sigil) {
                ep_eat(ep); /* consume closing bracket */
                break;
            }
            /* After the first arg, we expect separator then arg */
            if (!first_arg) {
                if (ct->kind == TOK_SIGIL && sep_sigil && ct->text == sep_sigil) {
                    ep_eat(ep);
                } else {
                    /* Wrong separator — this binding doesn't match */
                    parse_ok = false;
                    break;
                }
            }
            first_arg = false;
            ASTNode *arg = expr_parse_prec(ep, 0);
            if (arg) da_push(&call->call.args, arg);
            else { parse_ok = false; break; }
        }

        if (!parse_ok) {
            ep->pos = saved_pos;
            continue;
        }

        /* Check for trailing pattern elements (e.g., = v after ]) */
        bool trailing_ok = true;
        bool past_close = false;
        for (int pi = 0; pi < b->pattern.count && trailing_ok; pi++) {
            if (b->pattern.items[pi].kind == PAT_SIGIL &&
                b->pattern.items[pi].sigil == close_sigil) {
                past_close = true;
                continue;
            }
            if (!past_close) continue;
            if (b->pattern.items[pi].kind == PAT_SIGIL) {
                if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_SIGIL &&
                    ep_cur(ep)->text == b->pattern.items[pi].sigil) {
                    ep_eat(ep);
                } else {
                    trailing_ok = false;
                }
            } else if (b->pattern.items[pi].kind == PAT_PARAM) {
                ASTNode *arg = expr_parse_prec(ep, 0);
                if (arg) da_push(&call->call.args, arg);
                else trailing_ok = false;
            }
        }

        if (!trailing_ok) {
            ep->pos = saved_pos;
            continue;
        }

        /* This candidate matched. Keep it if it's the longest match. */
        if (b->all_sigils.count > best_sigil_count) {
            best_result = call;
            best_end_pos = ep->pos;
            best_sigil_count = b->all_sigils.count;
        }
        /* Restore position to try other candidates */
        ep->pos = saved_pos;
    }

    if (best_result) {
        ep->pos = best_end_pos;
        return best_result;
    }
    return NULL;
}

/* Parse a begin/end block (body delimiter) in expression context */
static ASTNode *ep_parse_begin_end(ExprParser *ep) {
    SrcLoc loc = ep_cur(ep)->loc;
    ep_eat(ep); /* consume begin */

    ASTNode *block = ast_new(ep->d->arena, NODE_BEGIN_END, loc);
    da_init(&block->block.stmts);

    while (!ep_at_eof(ep) && !(ep_cur(ep)->kind == TOK_END)) {
        ASTNode *stmt = ep_parse_use_stmt(ep);
        if (stmt) da_push(&block->block.stmts, stmt);
        else break;
    }

    if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_END)
        ep_eat(ep); /* consume end */

    return block;
}

/* Parse a do/end expression group */
static ASTNode *ep_parse_do_end(ExprParser *ep) {
    SrcLoc loc = ep_cur(ep)->loc;
    ep_eat(ep); /* consume do */

    ASTNode *block = ast_new(ep->d->arena, NODE_BEGIN_END, loc);
    da_init(&block->block.stmts);

    while (!ep_at_eof(ep) && !(ep_cur(ep)->kind == TOK_END)) {
        ASTNode *stmt = ep_parse_use_stmt(ep);
        if (stmt) da_push(&block->block.stmts, stmt);
        else break;
    }

    if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_END)
        ep_eat(ep); /* consume end */

    return block;
}

static ASTNode *ep_parse_atom(ExprParser *ep) {
    Token *t = ep_cur(ep);
    if (!t) return NULL;

    if (t->kind == TOK_INT_LIT) {
        ASTNode *n = ast_new(ep->d->arena, NODE_INT_LIT, t->loc);
        n->int_lit.int_val = t->int_val;
        ep_eat(ep);
        return n;
    }
    if (t->kind == TOK_FLOAT_LIT) {
        ASTNode *n = ast_new(ep->d->arena, NODE_FLOAT_LIT, t->loc);
        n->float_lit.float_val = t->float_val;
        ep_eat(ep);
        return n;
    }
    if (t->kind == TOK_KEYWORD && (strcmp(t->text, "true") == 0 || strcmp(t->text, "false") == 0)) {
        ASTNode *n = ast_new(ep->d->arena, NODE_BOOL_LIT, t->loc);
        n->bool_lit.bool_val = strcmp(t->text, "true") == 0;
        ep_eat(ep);
        return n;
    }
    if (t->kind == TOK_DO) {
        return ep_parse_do_end(ep);
    }
    /* Primitive keyword in atom position: parse as keyword-prefix call
       (e.g., multiply 6 7 as expression value via alias) */
    if (t->kind == TOK_KEYWORD && is_primitive_keyword(t->text)) {
        const char *kw = t->text;
        SrcLoc loc = t->loc;
        ep_eat(ep);
        /* 0-arg primitives: just return as a call with no args */
        if (strcmp(kw, "mapnew") == 0 || strcmp(kw, "true") == 0 || strcmp(kw, "false") == 0) {
            ASTNode *n = ast_new(ep->d->arena, NODE_CALL, loc);
            n->call.call_name = kw;
            da_init(&n->call.args);
            return n;
        }
        return ep_parse_kw_call(ep, kw, loc);
    }
    if (t->kind == TOK_IDENT) {
        ASTNode *n = ast_new(ep->d->arena, NODE_IDENT, t->loc);
        n->ident.ident = t->text;
        ep_eat(ep);
        return n;
    }

    /* Sigil in atom position */
    if (t->kind == TOK_SIGIL && ep->d->current_algebra) {
        /* Try standalone bracketed pattern (no left operand, e.g., [1,2,3]) */
        ASTNode *standalone = ep_try_bracketed(ep, NULL);
        if (standalone) return standalone;

        /* Try prefix sigil */
        SigilBinding *b = algebra_find_sigil(ep->d->current_algebra, t->text, FIXITY_PREFIX);
        if (b) {
            SrcLoc loc = t->loc;
            ep_eat(ep);
            ASTNode *operand = ep_parse_atom(ep);
            ASTNode *call = ast_new(ep->d->arena, NODE_CALL, loc);
            call->call.call_name = b->fn_name;
            da_init(&call->call.args);
            if (operand) da_push(&call->call.args, operand);
            return call;
        }

        /* Try nullary sigil */
        SigilBinding *nb = algebra_find_sigil(ep->d->current_algebra, t->text, FIXITY_NULLARY);
        if (nb) {
            SrcLoc loc = t->loc;
            ep_eat(ep);
            ASTNode *call = ast_new(ep->d->arena, NODE_CALL, loc);
            call->call.call_name = nb->fn_name;
            da_init(&call->call.args);
            return call;
        }
    }

    /* Unresolved sigil */
    if (t->kind == TOK_SIGIL) {
        ASTNode *n = ast_new(ep->d->arena, NODE_SIGIL_EXPR, t->loc);
        n->sigil_expr.sigil = t->text;
        da_init(&n->sigil_expr.operands);
        n->sigil_expr.expr_fixity = FIXITY_NULLARY;
        ep_eat(ep);
        return n;
    }

    return NULL;
}

static ASTNode *expr_parse_prec(ExprParser *ep, int min_prec) {
    ASTNode *left = ep_parse_atom(ep);
    if (!left) return NULL;

    for (;;) {
        if (ep_at_eof(ep)) break;
        Token *t = ep_cur(ep);
        if (!t || t->kind == TOK_END) break;

        /* Try bracketed operator with left operand (e.g., A[i,j]) */
        if (t->kind == TOK_SIGIL && ep->d->current_algebra) {
            ASTNode *bracketed = ep_try_bracketed(ep, left);
            if (bracketed) {
                left = bracketed;
                continue;
            }
        }

        /* Infix operator */
        if (t->kind != TOK_SIGIL) break;
        if (!ep->d->current_algebra) break;

        int prec = algebra_get_precedence(ep->d->current_algebra, t->text);
        if (prec < 0 || prec < min_prec) break;

        SigilBinding *b = algebra_find_sigil(ep->d->current_algebra, t->text, FIXITY_INFIX);
        if (!b) {
            /* Try postfix */
            SigilBinding *pb = algebra_find_sigil(ep->d->current_algebra, t->text, FIXITY_POSTFIX);
            if (pb) {
                SrcLoc loc = t->loc;
                ep_eat(ep);
                ASTNode *call = ast_new(ep->d->arena, NODE_CALL, loc);
                call->call.call_name = pb->fn_name;
                da_init(&call->call.args);
                da_push(&call->call.args, left);
                left = call;
                continue;
            }
            break;
        }

        SrcLoc loc = t->loc;
        const char *op_sigil = t->text;
        ep_eat(ep); /* consume operator */

        /* Determine chain grouping: check if the same operator follows
         * the right operand (i.e., a + b + c). Use trait consultation
         * to decide left vs right associativity. */
        int next_min_prec = prec + 1; /* default: left-associative */

        /* Check if this is a chain (same operator appears again) */
        if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_SIGIL) {
            /* Peek ahead: will this chain? We need to decide grouping now.
             * For right-associative: use prec (not prec+1) so right side
             * grabs the next operator at same precedence. */
            if (ep->d->trait_reg) {
                /* Check for grouping traits on the fn's param types */
                TypeRef *param_type = (b->param_count > 0 && b->param_types[0])
                    ? b->param_types[0] : NULL;
                const char *type_name = NULL;
                if (param_type) {
                    if (param_type->kind == TYPE_NAMED) type_name = param_type->name;
                    else if (param_type->kind == TYPE_INT) type_name = "int";
                    else if (param_type->kind == TYPE_FLOAT) type_name = "float";
                }

                if (type_name) {
                    TraitImpl *assoc = trait_find_impl(ep->d->trait_reg, "Associative", type_name);
                    TraitImpl *left_g = trait_find_impl(ep->d->trait_reg, "LeftGrouped", type_name);
                    TraitImpl *right_g = trait_find_impl(ep->d->trait_reg, "RightGrouped", type_name);

                    if (assoc && !left_g && !right_g) {
                        /* Associative: collect into flat NODE_CHAIN */
                        ASTNode *right = expr_parse_prec(ep, prec + 1);
                        ASTNode *chain = ast_new(ep->d->arena, NODE_CHAIN, loc);
                        chain->chain.chain_fn_name = b->fn_name;
                        da_init(&chain->chain.chain_operands);
                        /* Flatten left if it's already a chain of the same op */
                        if (left->kind == NODE_CHAIN &&
                            left->chain.chain_fn_name == b->fn_name) {
                            for (int ci = 0; ci < left->chain.chain_operands.count; ci++)
                                da_push(&chain->chain.chain_operands,
                                        left->chain.chain_operands.items[ci]);
                        } else {
                            da_push(&chain->chain.chain_operands, left);
                        }
                        /* Flatten right if it's also a chain of the same op */
                        if (right && right->kind == NODE_CHAIN &&
                            right->chain.chain_fn_name == b->fn_name) {
                            for (int ci = 0; ci < right->chain.chain_operands.count; ci++)
                                da_push(&chain->chain.chain_operands,
                                        right->chain.chain_operands.items[ci]);
                        } else if (right) {
                            da_push(&chain->chain.chain_operands, right);
                        }
                        left = chain;
                        continue;
                    } else if (right_g && !left_g) {
                        next_min_prec = prec; /* right-associative */
                    } else if (!left_g && !right_g) {
                        /* No grouping trait: check if actually chaining */
                        Token *peek = ep_cur(ep);
                        if (peek && peek->kind == TOK_SIGIL && peek->text == op_sigil) {
                            /* Would need to peek past the next atom to confirm chain,
                             * but emit warning — disambiguation needed */
                        }
                    }
                    /* LeftGrouped or default: keep left-associative (prec + 1) */
                }
            }
        }

        ASTNode *right = expr_parse_prec(ep, next_min_prec);

        ASTNode *call = ast_new(ep->d->arena, NODE_CALL, loc);
        call->call.call_name = b->fn_name;
        da_init(&call->call.args);
        da_push(&call->call.args, left);
        if (right) da_push(&call->call.args, right);
        left = call;
    }

    return left;
}

/* ── Use-Block Statement Parser (sigil-aware) ────────────────────── */

/* Parse a keyword-prefix call: eat args until next keyword or end */
/* Parse a keyword-prefix call as a condition: stops at unmatched begin (body delimiter).
   Unlike ep_parse_kw_call, this won't consume the body's begin/end. */
static ASTNode *ep_parse_condition(ExprParser *ep) {
    if (ep_at_eof(ep)) return NULL;
    Token *t = ep_cur(ep);
    if (!t) return NULL;

    /* Primitive keyword call: not, equal, less, etc. */
    if (t->kind == TOK_KEYWORD && is_primitive_keyword(t->text)) {
        const char *kw = t->text;
        SrcLoc loc = t->loc;
        ep_eat(ep);

        ASTNode *n = ast_new(ep->d->arena, NODE_CALL, loc);
        n->call.call_name = kw;
        da_init(&n->call.args);

        while (!ep_at_eof(ep) && ep_cur(ep)->kind != TOK_END &&
               ep_cur(ep)->kind != TOK_BEGIN &&
                   !(ep_cur(ep)->kind == TOK_KEYWORD && (is_structural_keyword(ep_cur(ep)->text) ||
                                                      is_primitive_keyword(ep_cur(ep)->text)))) {
            if (ep_cur(ep)->kind == TOK_DO) {
                ASTNode *block = ep_parse_do_end(ep);
                da_push(&n->call.args, block);
            } else {
                ASTNode *arg = ep_parse_atom(ep);
                if (arg) da_push(&n->call.args, arg);
                else break;
            }
        }
        return n;
    }

    /* Ident call */
    if (t->kind == TOK_IDENT) {
        const char *name = t->text;
        SrcLoc loc = t->loc;
        ep_eat(ep);
        /* If next is body begin or eof, just return ident */
        if (ep_at_eof(ep) || ep_cur(ep)->kind == TOK_END ||
            ep_cur(ep)->kind == TOK_BEGIN ||
            (ep_cur(ep)->kind == TOK_KEYWORD && is_structural_keyword(ep_cur(ep)->text))) {
            ASTNode *n = ast_new(ep->d->arena, NODE_IDENT, loc);
            n->ident.ident = name;
            return n;
        }
        ASTNode *n = ast_new(ep->d->arena, NODE_CALL, loc);
        n->call.call_name = name;
        da_init(&n->call.args);
        while (!ep_at_eof(ep) && ep_cur(ep)->kind != TOK_END &&
               ep_cur(ep)->kind != TOK_BEGIN &&
                   !(ep_cur(ep)->kind == TOK_KEYWORD && (is_structural_keyword(ep_cur(ep)->text) ||
                                                      is_primitive_keyword(ep_cur(ep)->text)))) {
            if (ep_cur(ep)->kind == TOK_DO) {
                ASTNode *block = ep_parse_do_end(ep);
                da_push(&n->call.args, block);
            } else {
                ASTNode *arg = ep_parse_atom(ep);
                if (arg) da_push(&n->call.args, arg);
                else break;
            }
        }
        return n;
    }

    /* do/end expression or sigil expression */
    if (t->kind == TOK_DO) return ep_parse_do_end(ep);
    return expr_parse_prec(ep, 0);
}

static ASTNode *ep_parse_kw_call(ExprParser *ep, const char *name, SrcLoc loc) {
    ASTNode *n = ast_new(ep->d->arena, NODE_CALL, loc);
    n->call.call_name = name;
    da_init(&n->call.args);

    while (!ep_at_eof(ep) && ep_cur(ep)->kind != TOK_END &&
           ep_cur(ep)->kind != TOK_BEGIN &&
           !(ep_cur(ep)->kind == TOK_KEYWORD && (is_structural_keyword(ep_cur(ep)->text) ||
                                                   is_primitive_keyword(ep_cur(ep)->text)))) {
        if (ep_cur(ep)->kind == TOK_DO) {
            ASTNode *block = ep_parse_do_end(ep);
            da_push(&n->call.args, block);
        } else {
            ASTNode *arg = ep_parse_atom(ep);
            if (arg) da_push(&n->call.args, arg);
            else break;
        }
    }
    return n;
}

/* If the next token is the keyword 'as', consume it and wrap node in NODE_AS_EXPR. */
static ASTNode *maybe_wrap_as(ExprParser *ep, ASTNode *node) {
    if (!node) return node;
    if (ep_at_eof(ep)) return node;
    Token *t = ep_cur(ep);
    if (!t || t->kind != TOK_KEYWORD || strcmp(t->text, "as") != 0) return node;
    ep_eat(ep); /* consume 'as' */
    if (ep_at_eof(ep)) return node; /* malformed, leave as-is */
    const char *alg_name = ep_cur(ep)->text;
    ep_eat(ep);
    ASTNode *as_node = ast_new(ep->d->arena, NODE_AS_EXPR, node->loc);
    as_node->as_expr.source = node;
    as_node->as_expr.target_algebra = alg_name;
    return as_node;
}

static ASTNode *ep_parse_use_stmt(ExprParser *ep) {
    if (ep_at_eof(ep)) return NULL;
    Token *t = ep_cur(ep);
    if (!t) return NULL;
    if (t->kind == TOK_END) return NULL;

    /* let / var */
    if (t->kind == TOK_KEYWORD &&
        (strcmp(t->text, "let") == 0 || strcmp(t->text, "var") == 0)) {
        bool is_var = strcmp(t->text, "var") == 0;
        SrcLoc loc = t->loc;
        ep_eat(ep);
        const char *name = ep_cur(ep) ? ep_cur(ep)->text : "";
        if (!ep_at_eof(ep)) ep_eat(ep);

        ASTNode *n = ast_new(ep->d->arena, is_var ? NODE_VAR : NODE_LET, loc);
        n->binding.bind_name = name;

        n->binding.value = maybe_wrap_as(ep, expr_parse_prec(ep, 0));
        return n;
    }

    /* assign */
    if (t->kind == TOK_KEYWORD && strcmp(t->text, "assign") == 0) {
        SrcLoc loc = t->loc;
        ep_eat(ep);
        const char *name = ep_cur(ep) ? ep_cur(ep)->text : "";
        if (!ep_at_eof(ep)) ep_eat(ep);

        ASTNode *n = ast_new(ep->d->arena, NODE_ASSIGN, loc);
        n->assign.assign_name = name;
        n->assign.value = maybe_wrap_as(ep, expr_parse_prec(ep, 0));
        return n;
    }

    /* return */
    if (t->kind == TOK_KEYWORD && strcmp(t->text, "return") == 0) {
        SrcLoc loc = t->loc;
        ep_eat(ep);
        ASTNode *n = ast_new(ep->d->arena, NODE_RETURN, loc);
        if (!ep_at_eof(ep) && ep_cur(ep)->kind != TOK_END)
            n->ret.value = maybe_wrap_as(ep, expr_parse_prec(ep, 0));
        else
            n->ret.value = NULL;
        return n;
    }

    /* for name in expr begin body end */
    if (t->kind == TOK_KEYWORD && strcmp(t->text, "for") == 0) {
        SrcLoc loc = t->loc;
        ep_eat(ep);
        const char *var = ep_cur(ep) ? ep_cur(ep)->text : "";
        if (!ep_at_eof(ep)) ep_eat(ep);
        if (!ep_at_eof(ep) && ep_at_text(ep, "in")) ep_eat(ep);

        ASTNode *n = ast_new(ep->d->arena, NODE_FOR, loc);
        n->for_stmt.var_name = var;

        /* Parse iterable expression */
        n->for_stmt.iterable = ep_parse_condition(ep);

        /* Parse body */
        if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_BEGIN)
            n->for_stmt.for_body = ep_parse_begin_end(ep);
        else
            n->for_stmt.for_body = NULL;
        return n;
    }

    /* if condition begin body end [elif ...] [else ...] */
    if (t->kind == TOK_KEYWORD && strcmp(t->text, "if") == 0) {
        SrcLoc loc = t->loc;
        ep_eat(ep);
        ASTNode *n = ast_new(ep->d->arena, NODE_IF, loc);
        da_init(&n->if_stmt.elifs);
        n->if_stmt.else_body = NULL;

        /* Parse condition as keyword-prefix call */
        n->if_stmt.condition = ep_parse_condition(ep);

        if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_BEGIN)
            n->if_stmt.then_body = ep_parse_begin_end(ep);
        else
            n->if_stmt.then_body = NULL;

        while (!ep_at_eof(ep) && ep_at_text(ep, "elif")) {
            ep_eat(ep);
            ASTNode *cond = ep_parse_condition(ep);
            ASTNode *body = NULL;
            if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_BEGIN)
                body = ep_parse_begin_end(ep);
            da_push(&n->if_stmt.elifs, cond);
            da_push(&n->if_stmt.elifs, body);
        }

        if (!ep_at_eof(ep) && ep_at_text(ep, "else")) {
            ep_eat(ep);
            if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_BEGIN)
                n->if_stmt.else_body = ep_parse_begin_end(ep);
        }
        return n;
    }

    /* while condition begin body end */
    if (t->kind == TOK_KEYWORD && strcmp(t->text, "while") == 0) {
        SrcLoc loc = t->loc;
        ep_eat(ep);
        ASTNode *n = ast_new(ep->d->arena, NODE_WHILE, loc);

        n->while_stmt.condition = ep_parse_condition(ep);

        if (!ep_at_eof(ep) && ep_cur(ep)->kind == TOK_BEGIN)
            n->while_stmt.while_body = ep_parse_begin_end(ep);
        else
            n->while_stmt.while_body = NULL;
        return n;
    }

    /* Primitive keyword calls (add, times, get, set, etc.) */
    if (t->kind == TOK_KEYWORD && is_primitive_keyword(t->text)) {
        const char *kw = t->text;
        SrcLoc loc = t->loc;
        ep_eat(ep);
        return ep_parse_kw_call(ep, kw, loc);
    }

    /* Otherwise: sigil expression */
    return maybe_wrap_as(ep, expr_parse_prec(ep, 0));
}

/* ── Use Block Body Re-parsing ───────────────────────────────────── */

static ASTNode *reparse_use_body(Desugarer *d, ASTNode *body) {
    if (!body) return body;
    if (!has_sigils(body)) return body;

    /* Flatten the body to tokens */
    TokenList toks;
    da_init(&toks);

    if (body->kind == NODE_BLOCK || body->kind == NODE_BEGIN_END) {
        for (int i = 0; i < body->block.stmts.count; i++)
            flatten_node(d, body->block.stmts.items[i], &toks);
    } else {
        flatten_node(d, body, &toks);
    }

    if (toks.count == 0) {
        da_free(&toks);
        return body;
    }

    /* Apply aliases from the active algebra */
    if (d->current_algebra) {
        for (int i = 0; i < toks.count; i++) {
            algebra_apply_alias(d->current_algebra, &toks.items[i], d->intern_tab);
        }
    }

    /* Re-parse the token stream with sigil-aware statement parser */
    ExprParser ep;
    ep.d = d;
    ep.tokens = toks.items;
    ep.count = toks.count;
    ep.pos = 0;

    ASTNode *new_body = ast_new(d->arena, NODE_BLOCK, body->loc);
    da_init(&new_body->block.stmts);

    while (!ep_at_eof(&ep)) {
        ASTNode *stmt = ep_parse_use_stmt(&ep);
        if (stmt) da_push(&new_body->block.stmts, stmt);
        else break;
    }

    da_free(&toks);
    return new_body;
}

/* ── Public API ──────────────────────────────────────────────────── */

ASTNode *desugar_expression(Desugarer *d, Token *tokens, int count) {
    ExprParser ep;
    ep.d = d;
    ep.tokens = tokens;
    ep.count = count;
    ep.pos = 0;
    return expr_parse_prec(&ep, 0);
}

/* ── AST Tree Desugaring ─────────────────────────────────────────── */

static ASTNode *desugar_node(Desugarer *d, ASTNode *node);

static void desugar_node_list(Desugarer *d, NodeList *list) {
    for (int i = 0; i < list->count; i++) {
        list->items[i] = desugar_node(d, list->items[i]);
    }
}

static ASTNode *desugar_node(Desugarer *d, ASTNode *node) {
    if (!node) return NULL;

    switch (node->kind) {
        case NODE_PROGRAM:
            desugar_node_list(d, &node->program.top_level);
            break;

        case NODE_ALGEBRA:
        case NODE_LIBRARY: {
            AlgebraEntry *alg = algebra_registry_find(d->registry, node->algebra.algebra_name);
            if (!alg) {
                alg = algebra_registry_add(d->registry, node->algebra.algebra_name);
                algebra_register_declarations(d->registry, alg, node);
            }
            AlgebraEntry *prev = d->current_algebra;
            d->current_algebra = alg;
            desugar_node_list(d, &node->algebra.declarations);
            d->current_algebra = prev;
            break;
        }

        case NODE_USE: {
            AlgebraEntry *alg = algebra_registry_find(d->registry, node->use_block.algebra_name);
            if (!alg) {
                error_add(d->errors, ERR_DESUGAR, node->loc,
                          "unknown algebra '%s'", node->use_block.algebra_name);
            } else {
                AlgebraEntry *prev = d->current_algebra;
                d->current_algebra = alg;
                /* Re-parse the body with sigil awareness */
                node->use_block.body = reparse_use_body(d, node->use_block.body);
                d->current_algebra = prev;
            }
            break;
        }

        case NODE_IMPORT:
            desugar_node_list(d, &node->import_decl.declarations);
            break;

        case NODE_FN_DECL:
            node->fn_decl.body = desugar_node(d, node->fn_decl.body);
            break;

        case NODE_BLOCK:
        case NODE_BEGIN_END:
            desugar_node_list(d, &node->block.stmts);
            break;

        case NODE_IF:
            node->if_stmt.condition = desugar_node(d, node->if_stmt.condition);
            node->if_stmt.then_body = desugar_node(d, node->if_stmt.then_body);
            desugar_node_list(d, &node->if_stmt.elifs);
            node->if_stmt.else_body = desugar_node(d, node->if_stmt.else_body);
            break;

        case NODE_WHILE:
            node->while_stmt.condition = desugar_node(d, node->while_stmt.condition);
            node->while_stmt.while_body = desugar_node(d, node->while_stmt.while_body);
            break;

        case NODE_FOR:
            node->for_stmt.iterable = desugar_node(d, node->for_stmt.iterable);
            node->for_stmt.for_body = desugar_node(d, node->for_stmt.for_body);
            break;

        case NODE_MATCH:
            node->match_stmt.match_value = desugar_node(d, node->match_stmt.match_value);
            desugar_node_list(d, &node->match_stmt.cases);
            break;

        case NODE_CASE:
            node->case_branch.case_pattern = desugar_node(d, node->case_branch.case_pattern);
            node->case_branch.case_body = desugar_node(d, node->case_branch.case_body);
            break;

        case NODE_DEFAULT:
            node->default_branch.default_body = desugar_node(d, node->default_branch.default_body);
            break;

        case NODE_LET:
        case NODE_VAR:
            node->binding.value = desugar_node(d, node->binding.value);
            break;

        case NODE_ASSIGN:
            node->assign.value = desugar_node(d, node->assign.value);
            break;

        case NODE_RETURN:
            node->ret.value = desugar_node(d, node->ret.value);
            break;

        case NODE_CALL:
            desugar_node_list(d, &node->call.args);
            break;

        case NODE_CHAIN:
            desugar_node_list(d, &node->chain.chain_operands);
            break;

        case NODE_SIGIL_EXPR: {
            if (!d->current_algebra) break;
            SigilBinding *b = algebra_find_sigil(d->current_algebra, node->sigil_expr.sigil, FIXITY_NONE);
            if (b) {
                ASTNode *call = ast_new(d->arena, NODE_CALL, node->loc);
                call->call.call_name = b->fn_name;
                call->call.args = node->sigil_expr.operands;
                desugar_node_list(d, &call->call.args);
                return call;
            }
            break;
        }

        case NODE_TYPE_DECL:
        case NODE_TRAIT_DECL:
        case NODE_IMPLEMENT:
        case NODE_PRECEDENCE:
        case NODE_IDENT:
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_BOOL_LIT:
        case NODE_STRING_LIT:
        case NODE_PARAM:
        case NODE_TYPE_REF:
        case NODE_ALIAS:
        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_LAMBDA:
        case NODE_COMPREHENSION:
        case NODE_EXPORT_DECL:
            break;

        case NODE_AS_EXPR:
            node->as_expr.source = desugar_node(d, node->as_expr.source);
            break;
    }

    return node;
}

ASTNode *desugar_ast(Desugarer *d, ASTNode *node) {
    return desugar_node(d, node);
}
