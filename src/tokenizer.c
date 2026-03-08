#include "tokenizer.h"
#include <string.h>

/* ── Keyword Tables ──────────────────────────────────────────────── */

static const char *structural_keywords[] = {
    "fn", "returns", "return", "repeats", "begin", "end", "text",
    "if", "elif", "else", "while", "for", "in",
    "match", "case", "default",
    "let", "var", "assign",
    "precedence", "trait", "implement", "requires",
    "algebra", "library", "use", "type",
    NULL
};

static const char *primitive_keywords[] = {
    "add", "subtract", "times", "multiply", "divide", "modulo", "negate",
    "compare", "equal", "less", "greater", "less_equal", "greater_equal",
    "get", "set", "length", "range",
    "not", "and", "or", "print",
    "mapnew", "has", "remove", "mapcount",
    NULL
};

static const char *type_keywords[] = {
    "bool", "int", "float", "char", "map", "void",
    "int8", "int16", "int32", "int64",
    "float32", "float64",
    "iter",
    "true", "false",
    NULL
};

static bool str_in_list(const char *s, const char **list) {
    for (int i = 0; list[i]; i++)
        if (strcmp(s, list[i]) == 0) return true;
    return false;
}

bool is_structural_keyword(const char *s) { return str_in_list(s, structural_keywords); }
bool is_primitive_keyword(const char *s)  { return str_in_list(s, primitive_keywords); }
bool is_type_keyword(const char *s)       { return str_in_list(s, type_keywords); }
bool is_keyword(const char *s) {
    return is_structural_keyword(s) || is_primitive_keyword(s) || is_type_keyword(s);
}

/* ── Compound Sigils ─────────────────────────────────────────────── */

void compound_sigil_set_init(CompoundSigilSet *cs) {
    cs->sigils = NULL;
    cs->count = cs->capacity = 0;
}

void compound_sigil_set_add(CompoundSigilSet *cs, InternTable *intern_tab, const char *sigil) {
    const char *interned = intern_cstr(intern_tab, sigil);
    for (int i = 0; i < cs->count; i++)
        if (cs->sigils[i] == interned) return;
    if (cs->count >= cs->capacity) {
        cs->capacity = cs->capacity ? cs->capacity * 2 : 16;
        cs->sigils = realloc(cs->sigils, cs->capacity * sizeof(const char *));
    }
    cs->sigils[cs->count++] = interned;
}

void compound_sigil_set_free(CompoundSigilSet *cs) {
    free(cs->sigils);
    cs->sigils = NULL;
    cs->count = cs->capacity = 0;
}

/* ── Tokenizer ───────────────────────────────────────────────────── */

void tokenizer_init(Tokenizer *t, const char *source, const char *file,
                    InternTable *intern_tab, ErrorList *errors,
                    CompoundSigilSet *compounds) {
    t->source = source;
    t->file = file;
    t->pos = 0;
    t->line = 1;
    t->col = 1;
    t->intern_tab = intern_tab;
    t->errors = errors;
    t->compounds = compounds;
}

static uint32_t peek_cp(Tokenizer *t, int *len) {
    uint32_t cp;
    *len = utf8_decode(t->source + t->pos, &cp);
    return cp;
}

static void advance(Tokenizer *t, int bytes) {
    for (int i = 0; i < bytes; i++) {
        if (t->source[t->pos + i] == '\n') {
            t->line++;
            t->col = 1;
        } else {
            t->col++;
        }
    }
    t->pos += bytes;
}

/* Skip spaces and tabs only — newlines are preserved as tokens */
static void skip_spaces(Tokenizer *t) {
    while (t->source[t->pos]) {
        char c = t->source[t->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(t, 1);
            continue;
        }
        /* rem line comment — skip to end of line (newline itself preserved) */
        if (strncmp(t->source + t->pos, "rem", 3) == 0 &&
            (t->source[t->pos + 3] == ' ' || t->source[t->pos + 3] == '\t' ||
             t->source[t->pos + 3] == '\n' || t->source[t->pos + 3] == '\0')) {
            while (t->source[t->pos] && t->source[t->pos] != '\n')
                advance(t, 1);
            continue;
        }
        break;
    }
}

static Token make_token(TokenKind kind, const char *text, SrcLoc loc) {
    Token tok;
    memset(&tok, 0, sizeof(tok));
    tok.kind = kind;
    tok.text = text;
    tok.loc = loc;
    return tok;
}

/* Try to match a compound sigil starting at current position. */
static int try_compound_sigil(Tokenizer *t) {
    if (!t->compounds) return 0;
    int best_len = 0;
    int best_idx = -1;
    for (int i = 0; i < t->compounds->count; i++) {
        const char *cs = t->compounds->sigils[i];
        int clen = (int)strlen(cs);
        if (clen > best_len && strncmp(t->source + t->pos, cs, clen) == 0) {
            best_len = clen;
            best_idx = i;
        }
    }
    (void)best_idx;
    return best_len;
}

Token tokenizer_next(Tokenizer *t) {
    skip_spaces(t);

    SrcLoc loc = srcloc(t->file, t->line, t->col, t->pos);

    if (!t->source[t->pos]) {
        return make_token(TOK_EOF, intern_cstr(t->intern_tab, ""), loc);
    }

    /* Newline → TOK_NEWLINE (collapse multiple) */
    if (t->source[t->pos] == '\n') {
        while (t->source[t->pos] == '\n') {
            advance(t, 1);
            skip_spaces(t);
        }
        return make_token(TOK_NEWLINE, intern_cstr(t->intern_tab, "\n"), loc);
    }

    int cplen;
    uint32_t cp = peek_cp(t, &cplen);
    CharClass cc = charclass_of(cp);

    /* Number literal */
    if (is_digit(cp) || (cp == '.' && is_digit((uint8_t)t->source[t->pos + 1]))) {
        int start = t->pos;
        bool is_float = false;
        while (is_digit((uint8_t)t->source[t->pos]))
            advance(t, 1);
        if (t->source[t->pos] == '.' && is_digit((uint8_t)t->source[t->pos + 1])) {
            is_float = true;
            advance(t, 1); /* skip . */
            while (is_digit((uint8_t)t->source[t->pos]))
                advance(t, 1);
        }
        int len = t->pos - start;
        const char *text = intern(t->intern_tab, t->source + start, len);
        Token tok = make_token(is_float ? TOK_FLOAT_LIT : TOK_INT_LIT, text, loc);
        if (is_float) tok.float_val = strtod(text, NULL);
        else tok.int_val = strtoll(text, NULL, 10);
        return tok;
    }

    /* ASCII alphanumeric: keyword or identifier */
    if (cc == CHARCLASS_ASCII_ALNUM) {
        int start = t->pos;
        while (t->source[t->pos] && charclass_of((uint8_t)t->source[t->pos]) == CHARCLASS_ASCII_ALNUM)
            advance(t, 1);
        int len = t->pos - start;
        const char *text = intern(t->intern_tab, t->source + start, len);

        if (strcmp(text, "text") == 0 && t->source[t->pos] == '\n') {
            /* text\n...\nend — raw string capture */
            advance(t, 1); /* skip the \n */
            int start = t->pos;
            const char *end_marker = "\nend";
            int end_len = 4;
            while (t->source[t->pos]) {
                if (strncmp(t->source + t->pos, end_marker, end_len) == 0) {
                    /* Check that 'end' is followed by whitespace/newline/EOF */
                    char after = t->source[t->pos + end_len];
                    if (after == '\0' || after == '\n' || after == ' ' || after == '\t' || after == '\r') {
                        int content_len = t->pos - start;
                        const char *content = intern(t->intern_tab, t->source + start, content_len);
                        /* advance past \nend */
                        for (int i = 0; i < end_len; i++) advance(t, 1);
                        return make_token(TOK_STRING_LIT, content, loc);
                    }
                }
                advance(t, 1);
            }
            /* Unterminated string */
            error_add(t->errors, ERR_LEXER, loc, "unterminated text literal (missing \\nend)");
            const char *content = intern(t->intern_tab, t->source + start, t->pos - start);
            return make_token(TOK_STRING_LIT, content, loc);
        }
        if (strcmp(text, "begin") == 0)
            return make_token(TOK_BEGIN, text, loc);
        if (strcmp(text, "end") == 0)
            return make_token(TOK_END, text, loc);
        if (strcmp(text, "true") == 0) {
            Token tok = make_token(TOK_KEYWORD, text, loc);
            return tok;
        }
        if (strcmp(text, "false") == 0) {
            Token tok = make_token(TOK_KEYWORD, text, loc);
            return tok;
        }
        if (is_keyword(text))
            return make_token(TOK_KEYWORD, text, loc);
        return make_token(TOK_IDENT, text, loc);
    }

    /* ASCII symbol: sigil(s) */
    if (cc == CHARCLASS_ASCII_SYMBOL) {
        /* Try compound sigil first */
        int clen = try_compound_sigil(t);
        if (clen > 1) {
            int start = t->pos;
            for (int i = 0; i < clen; i++) advance(t, 1);
            const char *text = intern(t->intern_tab, t->source + start, clen);
            return make_token(TOK_SIGIL, text, loc);
        }
        /* Single character sigil */
        int start = t->pos;
        advance(t, cplen);
        const char *text = intern(t->intern_tab, t->source + start, cplen);
        return make_token(TOK_SIGIL, text, loc);
    }

    /* Unicode non-ASCII: sigil or Unicode compound */
    if (cc == CHARCLASS_UNICODE) {
        int start = t->pos;
        /* Consume consecutive Unicode codepoints of same class, but check compounds */
        advance(t, cplen);
        /* For Unicode, each codepoint is typically its own token (class boundary with next) */
        const char *text = intern(t->intern_tab, t->source + start, t->pos - start);
        return make_token(TOK_SIGIL, text, loc);
    }

    /* Unknown character */
    error_add(t->errors, ERR_LEXER, loc, "unexpected character U+%04X", cp);
    advance(t, cplen);
    return tokenizer_next(t);
}

TokenList tokenize_all(Tokenizer *t) {
    TokenList list;
    da_init(&list);
    for (;;) {
        Token tok = tokenizer_next(t);
        da_push(&list, tok);
        if (tok.kind == TOK_EOF) break;
    }
    return list;
}
