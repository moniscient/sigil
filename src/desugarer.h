#ifndef SIGIL_DESUGARER_H
#define SIGIL_DESUGARER_H

#include "ast.h"
#include "algebra.h"
#include "tokenizer.h"
#include "traits.h"
#include "errors.h"

typedef struct {
    Arena *arena;
    InternTable *intern_tab;
    AlgebraRegistry *registry;
    AlgebraEntry *current_algebra;
    TraitRegistry *trait_reg;   /* for chain grouping consultation */
    ErrorList *errors;
} Desugarer;

void desugarer_init(Desugarer *d, Arena *arena, InternTable *intern_tab,
                    AlgebraRegistry *registry, TraitRegistry *trait_reg,
                    ErrorList *errors);

/* Desugar a sigil expression token stream into keyword-prefix AST.
 * tokens: array of tokens within a use block
 * count: number of tokens
 * Returns the desugared AST node.
 */
ASTNode *desugar_expression(Desugarer *d, Token *tokens, int count);

/* Desugar an entire AST tree, converting sigil expressions to keyword-prefix form. */
ASTNode *desugar_ast(Desugarer *d, ASTNode *node);

#endif /* SIGIL_DESUGARER_H */
