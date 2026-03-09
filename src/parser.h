#ifndef SIGIL_PARSER_H
#define SIGIL_PARSER_H

#include "ast.h"
#include "tokenizer.h"
#include "errors.h"

/* Tracks imported file paths for deduplication and circular import detection. */
typedef struct {
    const char **paths;   /* resolved absolute paths (interned) */
    int count;
    int capacity;
} ImportSet;

void import_set_init(ImportSet *is);
bool import_set_contains(ImportSet *is, const char *path);
void import_set_add(ImportSet *is, const char *path);

typedef struct {
    TokenList tokens;
    int pos;
    Arena *arena;
    InternTable *intern_tab;
    ErrorList *errors;
    bool in_sigil_mode;  /* true inside use blocks for sigil expression parsing */
    const char *file_path;         /* path of file being parsed (for relative import resolution) */
    ImportSet *imports;            /* shared across recursive parses for dedup */
    CompoundSigilSet *compounds;   /* shared compound sigil set for recursive prescan */
} Parser;

void parser_init(Parser *p, TokenList tokens, Arena *arena,
                 InternTable *intern_tab, ErrorList *errors);

ASTNode *parse_program(Parser *p);
ASTNode *parse_expression(Parser *p);
ASTNode *parse_statement(Parser *p);

/* fn pattern analysis */
Fixity analyze_fn_pattern(PatList *pattern, StrList *sigils_out);

#endif /* SIGIL_PARSER_H */
