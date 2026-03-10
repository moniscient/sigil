#include "algebra.h"
#include "types.h"
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

void algebra_registry_init(AlgebraRegistry *r, Arena *arena, InternTable *intern_tab) {
    da_init(&r->algebras);
    r->arena = arena;
    r->intern_tab = intern_tab;
}

AlgebraEntry *algebra_registry_add(AlgebraRegistry *r, const char *name) {
    AlgebraEntry *e = (AlgebraEntry *)arena_alloc(r->arena, sizeof(AlgebraEntry));
    e->name = intern_cstr(r->intern_tab, name);
    da_init(&e->bindings);
    e->precedence.sigils = NULL;
    e->precedence.count = 0;
    da_init(&e->trait_decls);
    da_init(&e->implement_blocks);
    da_init(&e->aliases);
    da_push(&r->algebras, e);
    return e;
}

AlgebraEntry *algebra_registry_find(AlgebraRegistry *r, const char *name) {
    const char *interned = intern_cstr(r->intern_tab, name);
    for (int i = 0; i < r->algebras.count; i++) {
        if (r->algebras.items[i]->name == interned)
            return r->algebras.items[i];
    }
    return NULL;
}

static void register_fn_binding(AlgebraRegistry *r, AlgebraEntry *alg, ASTNode *fn) {
    if (fn->kind != NODE_FN_DECL) return;
    if (fn->fn_decl.sigils.count == 0) return; /* no sigil = keyword-only function */

    SigilBinding b;
    memset(&b, 0, sizeof(b));
    b.fn_name = fn->fn_decl.fn_name;
    b.sigil = fn->fn_decl.sigils.items[0];
    b.fixity = fn->fn_decl.fixity;
    b.all_sigils = fn->fn_decl.sigils;
    b.pattern = fn->fn_decl.pattern;
    b.return_type = fn->fn_decl.return_type;
    b.fn_node = fn;

    /* Count and collect param types */
    int pc = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++)
        if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM) pc++;
    b.param_count = pc;
    b.param_types = (TypeRef **)arena_alloc(r->arena, pc * sizeof(TypeRef *));
    int idx = 0;
    for (int i = 0; i < fn->fn_decl.pattern.count; i++) {
        if (fn->fn_decl.pattern.items[i].kind == PAT_PARAM)
            b.param_types[idx++] = fn->fn_decl.pattern.items[i].type;
    }

    da_push(&alg->bindings, b);
}

/* Determine the token kind for an alias target text. */
static TokenKind alias_target_kind(const char *text) {
    if (strcmp(text, "begin") == 0) return TOK_BEGIN;
    if (strcmp(text, "end") == 0) return TOK_END;
    if (strcmp(text, "do") == 0) return TOK_DO;
    if (is_keyword(text)) return TOK_KEYWORD;
    /* Assume sigil if all chars are ASCII symbols */
    bool all_sym = true;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            all_sym = false;
            break;
        }
    }
    if (all_sym && text[0]) return TOK_SIGIL;
    return TOK_IDENT;
}

void algebra_register_declarations(AlgebraRegistry *r, AlgebraEntry *alg, ASTNode *algebra_node) {
    NodeList *decls = &algebra_node->algebra.declarations;
    for (int i = 0; i < decls->count; i++) {
        ASTNode *decl = decls->items[i];
        switch (decl->kind) {
            case NODE_FN_DECL:
                register_fn_binding(r, alg, decl);
                break;
            case NODE_PRECEDENCE: {
                alg->precedence.count = decl->precedence.sigils.count;
                alg->precedence.sigils = (const char **)arena_alloc(
                    r->arena, alg->precedence.count * sizeof(const char *));
                for (int j = 0; j < alg->precedence.count; j++)
                    alg->precedence.sigils[j] = decl->precedence.sigils.items[j];
                break;
            }
            case NODE_TRAIT_DECL:
                da_push(&alg->trait_decls, decl);
                break;
            case NODE_IMPLEMENT:
                da_push(&alg->implement_blocks, decl);
                break;
            case NODE_ALIAS: {
                AliasEntry ae;
                ae.from_text = intern_cstr(r->intern_tab, decl->alias.alias_from);
                ae.to_text = intern_cstr(r->intern_tab, decl->alias.alias_to);
                ae.to_kind = alias_target_kind(decl->alias.alias_to);
                da_push(&alg->aliases, ae);
                break;
            }
            default:
                break;
        }
    }
}

SigilBinding *algebra_find_sigil(AlgebraEntry *alg, const char *sigil, Fixity fixity) {
    for (int i = 0; i < alg->bindings.count; i++) {
        SigilBinding *b = &alg->bindings.items[i];
        if (b->sigil == sigil && (fixity == FIXITY_NONE || b->fixity == fixity))
            return b;
    }
    return NULL;
}

int algebra_get_precedence(AlgebraEntry *alg, const char *sigil) {
    for (int i = 0; i < alg->precedence.count; i++) {
        if (alg->precedence.sigils[i] == sigil)
            return i;
    }
    return -1;
}

bool algebra_check_collisions(AlgebraEntry *alg, ErrorList *errors) {
    bool ok = true;
    for (int i = 0; i < alg->bindings.count; i++) {
        SigilBinding *a = &alg->bindings.items[i];
        for (int j = i + 1; j < alg->bindings.count; j++) {
            SigilBinding *b = &alg->bindings.items[j];
            /* Same sigil and same fixity? */
            if (a->sigil != b->sigil) continue;
            if (a->fixity != b->fixity) continue;
            /* Same parameter count and types? */
            if (a->param_count != b->param_count) continue;
            bool same_types = true;
            for (int k = 0; k < a->param_count; k++) {
                if (!types_equal(a->param_types[k], b->param_types[k])) {
                    same_types = false;
                    break;
                }
            }
            if (same_types) {
                error_add(errors, ERR_RESOLVE, (SrcLoc){NULL, 0, 0, 0},
                         "collision in algebra '%s': sigil '%s' with same fixity and parameter types declared in both '%s' and '%s'",
                         alg->name, a->sigil, a->fn_name, b->fn_name);
                ok = false;
            }
        }
    }
    return ok;
}

SigilBinding *algebra_match_compound(AlgebraEntry *alg, const char **sigils, int count) {
    /* Find bindings whose sigil set matches the given sequence */
    for (int i = 0; i < alg->bindings.count; i++) {
        SigilBinding *b = &alg->bindings.items[i];
        if (b->all_sigils.count < 2) continue; /* not a compound */

        /* Check if all given sigils appear in this binding's pattern */
        bool all_match = true;
        int matched = 0;
        for (int j = 0; j < count && all_match; j++) {
            bool found = false;
            for (int k = 0; k < b->all_sigils.count; k++) {
                if (b->all_sigils.items[k] == sigils[j]) {
                    found = true;
                    matched++;
                    break;
                }
            }
            if (!found) all_match = false;
        }
        if (all_match && matched >= 2) return b;
    }
    return NULL;
}

/* ── Pre-parse alias rewriting ───────────────────────────────────── */

/* Lightweight per-algebra alias collection for pre-parse rewriting. */
typedef struct PreAlias {
    const char *algebra_name;
    AliasEntry *entries;
    int count;
    int capacity;
} PreAlias;

#define MAX_PRE_ALGEBRAS 64

/* Helper: add an alias entry to a PreAlias */
static void pre_alias_add(PreAlias *pa, const char *from, const char *to) {
    if (pa->count >= pa->capacity) {
        pa->capacity = pa->capacity ? pa->capacity * 2 : 8;
        pa->entries = realloc(pa->entries, pa->capacity * sizeof(AliasEntry));
    }
    AliasEntry *ae = &pa->entries[pa->count++];
    ae->from_text = from;
    ae->to_text = to;
    ae->to_kind = alias_target_kind(to);
}

/* Helper: find or create a PreAlias entry by algebra name */
static int pre_alias_find_or_create(PreAlias *algebras, int *num, const char *name) {
    for (int k = 0; k < *num; k++)
        if (strcmp(algebras[k].algebra_name, name) == 0) return k;
    if (*num >= MAX_PRE_ALGEBRAS) return -1;
    int ai = (*num)++;
    algebras[ai].algebra_name = name;
    algebras[ai].entries = NULL;
    algebras[ai].count = algebras[ai].capacity = 0;
    return ai;
}

/* Read a file for alias prescan */
static char *alias_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); return NULL; }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Scan source text for algebra alias declarations (lightweight text scan like prescan_compound_sigils) */
static void prescan_aliases_from_source(const char *source, const char *file_path,
                                         PreAlias *algebras, int *num_algebras,
                                         InternTable *intern_tab) {
    /* Simple line-by-line scan of raw source text */
    const char *p = source;
    const char *current_algebra = NULL;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\r') { p++; continue; }
        if (*p == '\0') break;

        /* Check for import */
        if (strncmp(p, "import", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
            const char *q = p + 6;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '"') {
                q++;
                const char *path_start = q;
                while (*q && *q != '"' && *q != '\n') q++;
                if (*q == '"') {
                    int path_len = (int)(q - path_start);
                    char import_path[4096];
                    if (path_len < (int)sizeof(import_path)) {
                        memcpy(import_path, path_start, path_len);
                        import_path[path_len] = '\0';
                        char resolved[4096];
                        bool ok = false;
                        if (import_path[0] == '/') {
                            ok = (realpath(import_path, resolved) != NULL);
                        } else if (file_path) {
                            char dir_buf[4096];
                            snprintf(dir_buf, sizeof(dir_buf), "%s", file_path);
                            char *dir = dirname(dir_buf);
                            char joined[4096];
                            snprintf(joined, sizeof(joined), "%s/%s", dir, import_path);
                            ok = (realpath(joined, resolved) != NULL);
                        }
                        if (ok) {
                            char *imported = alias_read_file(resolved);
                            if (imported) {
                                prescan_aliases_from_source(imported, resolved, algebras, num_algebras, intern_tab);
                                free(imported);
                            }
                        }
                    }
                }
            }
        }

        /* Check for algebra */
        if (strncmp(p, "algebra", 7) == 0 && (p[7] == ' ' || p[7] == '\t')) {
            const char *q = p + 7;
            while (*q == ' ' || *q == '\t') q++;
            const char *name_start = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') q++;
            if (q > name_start) {
                current_algebra = intern(intern_tab, name_start, (int)(q - name_start));
            }
        }

        /* Check for alias (within an algebra) */
        if (current_algebra && strncmp(p, "alias", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
            const char *q = p + 5;
            while (*q == ' ' || *q == '\t') q++;
            const char *from_start = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '\n') q++;
            if (q > from_start) {
                const char *from = intern(intern_tab, from_start, (int)(q - from_start));
                while (*q == ' ' || *q == '\t') q++;
                const char *to_start = q;
                while (*q && *q != ' ' && *q != '\t' && *q != '\n') q++;
                if (q > to_start) {
                    const char *to = intern(intern_tab, to_start, (int)(q - to_start));
                    int ai = pre_alias_find_or_create(algebras, num_algebras, current_algebra);
                    if (ai >= 0) pre_alias_add(&algebras[ai], from, to);
                }
            }
        }

        /* Check for use/library (end current algebra context) */
        if ((strncmp(p, "use", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) ||
            (strncmp(p, "library", 7) == 0 && (p[7] == ' ' || p[7] == '\t'))) {
            current_algebra = NULL;
        }

        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

void alias_rewrite_tokens(TokenList *tokens, InternTable *intern_tab, const char *file_path) {
    Token *t = tokens->items;
    int n = tokens->count;

    /* Pass 1: Collect alias declarations from algebra blocks in this file and imports */
    PreAlias algebras[MAX_PRE_ALGEBRAS];
    int num_algebras = 0;

    /* Scan the token stream for algebras with aliases */
    for (int i = 0; i < n; i++) {
        if (t[i].kind == TOK_KEYWORD && strcmp(t[i].text, "algebra") == 0) {
            int j = i + 1;
            while (j < n && t[j].kind == TOK_NEWLINE) j++;
            if (j >= n || t[j].kind != TOK_IDENT) continue;

            const char *alg_name = t[j].text;
            int ai = pre_alias_find_or_create(algebras, &num_algebras, alg_name);
            if (ai < 0) continue;

            for (int k = j + 1; k < n; k++) {
                if (t[k].kind == TOK_NEWLINE) continue;
                if (t[k].kind == TOK_KEYWORD &&
                    (strcmp(t[k].text, "algebra") == 0 ||
                     strcmp(t[k].text, "library") == 0 ||
                     strcmp(t[k].text, "use") == 0 ||
                     strcmp(t[k].text, "import") == 0))
                    break;
                if (t[k].kind == TOK_KEYWORD && strcmp(t[k].text, "alias") == 0) {
                    int f = k + 1;
                    while (f < n && t[f].kind == TOK_NEWLINE) f++;
                    if (f >= n) continue;
                    int g = f + 1;
                    while (g < n && t[g].kind == TOK_NEWLINE) g++;
                    if (g >= n) continue;
                    pre_alias_add(&algebras[ai], t[f].text, t[g].text);
                }
            }
        }
    }

    /* Also scan imported files for algebra aliases (source-level prescan) */
    for (int i = 0; i < n; i++) {
        if (t[i].kind == TOK_KEYWORD && strcmp(t[i].text, "import") == 0) {
            int j = i + 1;
            while (j < n && t[j].kind == TOK_NEWLINE) j++;
            if (j < n && t[j].kind == TOK_STRING_LIT) {
                const char *raw_path = t[j].text;
                char resolved[4096];
                bool ok = false;
                if (raw_path[0] == '/') {
                    ok = (realpath(raw_path, resolved) != NULL);
                } else if (file_path) {
                    char dir_buf[4096];
                    snprintf(dir_buf, sizeof(dir_buf), "%s", file_path);
                    char *dir = dirname(dir_buf);
                    char joined[4096];
                    snprintf(joined, sizeof(joined), "%s/%s", dir, raw_path);
                    ok = (realpath(joined, resolved) != NULL);
                }
                if (ok) {
                    char *imported = alias_read_file(resolved);
                    if (imported) {
                        prescan_aliases_from_source(imported, resolved, algebras, &num_algebras, intern_tab);
                        free(imported);
                    }
                }
            }
        }
    }

    if (num_algebras == 0) goto cleanup;

    /* Pass 2: Find use blocks and apply aliases */
    for (int i = 0; i < n; i++) {
        if (t[i].kind != TOK_KEYWORD || strcmp(t[i].text, "use") != 0) continue;

        /* use NAME */
        int j = i + 1;
        while (j < n && t[j].kind == TOK_NEWLINE) j++;
        if (j >= n || t[j].kind != TOK_IDENT) continue;

        const char *alg_name = t[j].text;

        /* Find algebra's aliases */
        int ai = -1;
        for (int k = 0; k < num_algebras; k++) {
            if (strcmp(algebras[k].algebra_name, alg_name) == 0) { ai = k; break; }
        }
        if (ai < 0 || algebras[ai].count == 0) continue;

        /* Apply aliases from j+1 until next top-level construct */
        for (int k = j + 1; k < n; k++) {
            if (t[k].kind == TOK_NEWLINE) continue;
            /* Stop at top-level constructs */
            if (t[k].kind == TOK_KEYWORD &&
                (strcmp(t[k].text, "algebra") == 0 ||
                 strcmp(t[k].text, "library") == 0 ||
                 strcmp(t[k].text, "import") == 0 ||
                 (strcmp(t[k].text, "use") == 0 && k != i)))
                break;
            /* Apply each alias */
            for (int a = 0; a < algebras[ai].count; a++) {
                if (strcmp(t[k].text, algebras[ai].entries[a].from_text) == 0) {
                    t[k].text = intern_cstr(intern_tab, algebras[ai].entries[a].to_text);
                    t[k].kind = algebras[ai].entries[a].to_kind;
                    break;
                }
            }
        }
    }

cleanup:
    for (int i = 0; i < num_algebras; i++)
        free(algebras[i].entries);
}

AliasEntry *algebra_find_alias(AlgebraEntry *alg, const char *text) {
    for (int i = 0; i < alg->aliases.count; i++) {
        if (strcmp(alg->aliases.items[i].from_text, text) == 0)
            return &alg->aliases.items[i];
    }
    return NULL;
}

bool algebra_apply_alias(AlgebraEntry *alg, Token *tok, InternTable *intern_tab) {
    AliasEntry *ae = algebra_find_alias(alg, tok->text);
    if (!ae) return false;
    tok->text = intern_cstr(intern_tab, ae->to_text);
    tok->kind = ae->to_kind;
    return true;
}
