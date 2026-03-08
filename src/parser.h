#ifndef SIGIL_PARSER_H
#define SIGIL_PARSER_H

#include "ast.h"
#include "tokenizer.h"
#include "errors.h"

typedef struct {
    TokenList tokens;
    int pos;
    Arena *arena;
    InternTable *intern_tab;
    ErrorList *errors;
    bool in_sigil_mode;  /* true inside use blocks for sigil expression parsing */
} Parser;

void parser_init(Parser *p, TokenList tokens, Arena *arena,
                 InternTable *intern_tab, ErrorList *errors);

ASTNode *parse_program(Parser *p);
ASTNode *parse_expression(Parser *p);
ASTNode *parse_statement(Parser *p);

/* fn pattern analysis */
Fixity analyze_fn_pattern(PatList *pattern, StrList *sigils_out);

#endif /* SIGIL_PARSER_H */
