#include "tokenizer.h"
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

/* ── Keyword Tables ──────────────────────────────────────────────── */

static const char *structural_keywords[] = {
    "fn", "returns", "return", "repeats", "begin", "end", "do",
    "if", "elif", "else", "while", "for", "in",
    "match", "case", "default",
    "let", "var", "assign",
    "precedence", "trait", "implement", "requires",
    "algebra", "library", "use", "type",
    "lambda", "collect", "apply", "from", "where",
    "import", "break", "continue", "alias",
    "export", "required", "optional", "private", "as",
    "pure", "distributive", "over",
    NULL
};

static const char *primitive_keywords[] = {
    "add", "subtract", "times", "multiply", "divide", "modulo", "negate",
    "compare", "equal", "less", "greater", "less_equal", "greater_equal",
    "get", "set", "length", "range",
    "not", "and", "or", "print",
    "mapnew", "has", "remove", "mapcount",
    "invoke", "concat", "append", "clone", "keys", "values",
    "to_int", "to_float", "to_string",
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

/* ── Pre-scan for compound sigils ────────────────────────────────── */

/* Simple helper: is this byte an ASCII symbol character? */
static bool is_ascii_symbol(char c) {
    if (c <= ' ' || c > '~') return false;  /* non-printable or non-ASCII */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') return false;
    return true;
}

/* Skip whitespace (spaces, tabs) but not newlines. */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Check if position starts with the given word followed by whitespace/newline/EOF. */
static bool at_word(const char *p, const char *word) {
    int len = (int)strlen(word);
    if (strncmp(p, word, len) != 0) return false;
    char next = p[len];
    return next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '\0';
}

/* Skip to next line. */
static const char *next_line(const char *p) {
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    return p;
}

/* Read a file for prescan (same as read_file but local to tokenizer). */
static char *prescan_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

void prescan_compound_sigils(const char *source, const char *file_path,
                             CompoundSigilSet *cs, InternTable *intern_tab) {
    const char *p = source;
    bool in_algebra = false;

    while (*p) {
        p = skip_ws(p);
        if (*p == '\n' || *p == '\r') { p++; continue; }
        if (*p == '\0') break;

        /* Follow import statements recursively */
        if (at_word(p, "import")) {
            /* Skip "import" keyword */
            const char *q = p + 6; /* strlen("import") */
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"') {
                q++; /* skip opening quote */
                const char *path_start = q;
                while (*q && *q != '"' && *q != '\n') q++;
                if (*q == '"') {
                    int path_len = (int)(q - path_start);
                    char import_path[4096];
                    if (path_len < (int)sizeof(import_path)) {
                        memcpy(import_path, path_start, path_len);
                        import_path[path_len] = '\0';

                        /* Resolve relative to current file */
                        char resolved[4096];
                        bool resolved_ok = false;
                        if (import_path[0] == '/') {
                            resolved_ok = (realpath(import_path, resolved) != NULL);
                        } else if (file_path) {
                            char dir_buf[4096];
                            snprintf(dir_buf, sizeof(dir_buf), "%s", file_path);
                            char *dir = dirname(dir_buf);
                            char joined[4096];
                            snprintf(joined, sizeof(joined), "%s/%s", dir, import_path);
                            resolved_ok = (realpath(joined, resolved) != NULL);
                        } else {
                            resolved_ok = (realpath(import_path, resolved) != NULL);
                        }

                        if (resolved_ok) {
                            /* Check if already scanned (simple linear check on compound set file tracking) */
                            /* We use the compound set itself for dedup: if we've never seen this file,
                             * scan it. Use a simple heuristic: intern the path and check. */
                            char *imported_source = prescan_read_file(resolved);
                            if (imported_source) {
                                prescan_compound_sigils(imported_source, resolved, cs, intern_tab);
                                free(imported_source);
                            }
                        }
                    }
                }
            }
            p = next_line(p);
            continue;
        }

        /* Check for algebra/library keyword at start of line */
        if (at_word(p, "algebra") || at_word(p, "library")) {
            in_algebra = true;
            p = next_line(p);
            continue;
        }

        /* Lines that start a new top-level block end the algebra scope */
        if (in_algebra && !at_word(p, "fn") && !at_word(p, "precedence") &&
            !at_word(p, "trait") && !at_word(p, "implement") &&
            !at_word(p, "requires") && !at_word(p, "rem") &&
            !at_word(p, "begin") && !at_word(p, "end") &&
            !at_word(p, "type") && !at_word(p, "use") &&
            !is_ascii_symbol(*p) &&
            *p != '\n' && *p != '\r') {
            /* Just keep scanning — false positives in compound set are harmless. */
        }

        /* Extract sigil runs from fn declaration lines inside algebra blocks */
        if (in_algebra && at_word(p, "fn")) {
            /* Walk the entire fn line and collect any multi-char sigil runs */
            const char *line = p;
            while (*line && *line != '\n') {
                if (is_ascii_symbol(*line)) {
                    const char *start = line;
                    while (is_ascii_symbol(*line)) line++;
                    int len = (int)(line - start);
                    if (len > 1) {
                        char buf[64];
                        if (len < (int)sizeof(buf)) {
                            memcpy(buf, start, len);
                            buf[len] = '\0';
                            compound_sigil_set_add(cs, intern_tab, buf);
                        }
                    }
                } else {
                    line++;
                }
            }
        }

        p = next_line(p);
    }
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

    /* String literal: "..." or """...""" */
    if (cp == '"') {
        /* Check for triple-quote """...""" */
        if (t->source[t->pos + 1] == '"' && t->source[t->pos + 2] == '"') {
            advance(t, 3); /* skip opening """ */
            int start = t->pos;
            while (t->source[t->pos]) {
                if (t->source[t->pos] == '"' && t->source[t->pos + 1] == '"' && t->source[t->pos + 2] == '"') {
                    int content_len = t->pos - start;
                    const char *content = intern(t->intern_tab, t->source + start, content_len);
                    advance(t, 3); /* skip closing """ */
                    return make_token(TOK_STRING_LIT, content, loc);
                }
                advance(t, 1);
            }
            error_add(t->errors, ERR_LEXER, loc, "unterminated triple-quoted string");
            const char *content = intern(t->intern_tab, t->source + start, t->pos - start);
            return make_token(TOK_STRING_LIT, content, loc);
        }
        /* Single-quoted string "..." */
        advance(t, 1); /* skip opening " */
        /* Build escaped content into temp buffer */
        char buf[4096];
        int bi = 0;
        while (t->source[t->pos] && t->source[t->pos] != '"' && t->source[t->pos] != '\n') {
            if (t->source[t->pos] == '\\' && t->source[t->pos + 1]) {
                advance(t, 1); /* skip backslash */
                char esc = t->source[t->pos];
                switch (esc) {
                    case 'n':  if (bi < 4095) buf[bi++] = '\n'; break;
                    case 't':  if (bi < 4095) buf[bi++] = '\t'; break;
                    case 'r':  if (bi < 4095) buf[bi++] = '\r'; break;
                    case '\\': if (bi < 4095) buf[bi++] = '\\'; break;
                    case '"':  if (bi < 4095) buf[bi++] = '"';  break;
                    default:   if (bi < 4094) { buf[bi++] = '\\'; buf[bi++] = esc; } break;
                }
                advance(t, 1);
            } else {
                if (bi < 4095) buf[bi++] = t->source[t->pos];
                advance(t, 1);
            }
        }
        buf[bi] = '\0';
        if (t->source[t->pos] == '"') {
            advance(t, 1); /* skip closing " */
        } else {
            error_add(t->errors, ERR_LEXER, loc, "unterminated string literal");
        }
        const char *content = intern(t->intern_tab, buf, bi);
        return make_token(TOK_STRING_LIT, content, loc);
    }

    /* ASCII alphanumeric: keyword or identifier */
    if (cc == CHARCLASS_ASCII_ALNUM) {
        int start = t->pos;
        while (t->source[t->pos] && charclass_of((uint8_t)t->source[t->pos]) == CHARCLASS_ASCII_ALNUM)
            advance(t, 1);
        int len = t->pos - start;
        const char *text = intern(t->intern_tab, t->source + start, len);

if (strcmp(text, "begin") == 0)
            return make_token(TOK_BEGIN, text, loc);
        if (strcmp(text, "end") == 0)
            return make_token(TOK_END, text, loc);
        if (strcmp(text, "do") == 0)
            return make_token(TOK_DO, text, loc);
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
