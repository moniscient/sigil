#ifndef SIGIL_TOKENIZER_H
#define SIGIL_TOKENIZER_H

#include "common.h"
#include "unicode.h"
#include "errors.h"

typedef enum {
    TOK_KEYWORD,      /* structural/primitive keyword */
    TOK_IDENT,        /* identifier */
    TOK_SIGIL,        /* operator / symbol */
    TOK_INT_LIT,      /* integer literal */
    TOK_FLOAT_LIT,    /* floating point literal */
    TOK_STRING_LIT,   /* string literal (text...end) */
    TOK_BEGIN,         /* begin */
    TOK_END,           /* end */
    TOK_NEWLINE,       /* statement boundary */
    TOK_EOF
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *text;  /* interned string */
    SrcLoc loc;
    union {
        int64_t int_val;
        double float_val;
    };
} Token;

DA_TYPEDEF(Token, TokenList)

/* Set of compound sigils known to the tokenizer (from algebra declarations). */
typedef struct {
    const char **sigils;  /* interned compound sigil strings */
    int count;
    int capacity;
} CompoundSigilSet;

void compound_sigil_set_init(CompoundSigilSet *cs);
void compound_sigil_set_add(CompoundSigilSet *cs, InternTable *intern_tab, const char *sigil);
void compound_sigil_set_free(CompoundSigilSet *cs);

typedef struct {
    const char *source;
    const char *file;
    int pos;
    int line;
    int col;
    InternTable *intern_tab;
    ErrorList *errors;
    CompoundSigilSet *compounds;
} Tokenizer;

void tokenizer_init(Tokenizer *t, const char *source, const char *file,
                    InternTable *intern_tab, ErrorList *errors,
                    CompoundSigilSet *compounds);

Token tokenizer_next(Tokenizer *t);
TokenList tokenize_all(Tokenizer *t);

/* Check if a string is a keyword. */
bool is_keyword(const char *s);
bool is_structural_keyword(const char *s);
bool is_primitive_keyword(const char *s);
bool is_type_keyword(const char *s);

#endif /* SIGIL_TOKENIZER_H */
