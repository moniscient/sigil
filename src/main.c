#include "common.h"
#include "unicode.h"
#include "tokenizer.h"
#include "parser.h"
#include "algebra.h"
#include "desugarer.h"
#include "types.h"
#include "traits.h"
#include "resolver.h"
#include "skw_emitter.h"
#include "c_emitter.h"
#include "errors.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        fprintf(stderr, "error: cannot read '%s'\n", path);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return false;
    return strcmp(s + sl - xl, suffix) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sigil <input.sigil|input.skw> [-o output] [-c]\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = NULL;
    bool emit_c = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output_path = argv[++i];
        else if (strcmp(argv[i], "-c") == 0)
            emit_c = true;
    }

    char *source = read_file(input_path);
    if (!source) return 1;

    bool is_skw = ends_with(input_path, ".skw");

    /* Initialize infrastructure */
    Arena arena;
    arena_init(&arena);
    InternTable intern_tab;
    intern_init(&intern_tab);
    ErrorList errors;
    error_list_init(&errors, &arena);
    CompoundSigilSet compounds;
    compound_sigil_set_init(&compounds);

    /* Phase 1: Tokenize */
    Tokenizer tokenizer;
    tokenizer_init(&tokenizer, source, input_path, &intern_tab, &errors, &compounds);
    TokenList tokens = tokenize_all(&tokenizer);

    if (error_has_errors(&errors)) {
        error_print_all(&errors);
        goto cleanup;
    }

    /* Phase 2: Parse */
    Parser parser;
    parser_init(&parser, tokens, &arena, &intern_tab, &errors);
    ASTNode *ast = parse_program(&parser);

    if (error_has_errors(&errors)) {
        error_print_all(&errors);
        goto cleanup;
    }

    /* Phase 3: Algebra registration & desugaring (skip for .skw) */
    AlgebraRegistry algebra_reg;
    algebra_registry_init(&algebra_reg, &arena, &intern_tab);

    /* Initialize trait registry before desugaring (needed for chain grouping) */
    TraitRegistry trait_reg;
    trait_registry_init(&trait_reg, &arena, &intern_tab, &errors);

    if (!is_skw) {
        Desugarer desugarer;
        desugarer_init(&desugarer, &arena, &intern_tab, &algebra_reg, &trait_reg, &errors);
        ast = desugar_ast(&desugarer, ast);

        if (error_has_errors(&errors)) {
            error_print_all(&errors);
            goto cleanup;
        }
    }

    /* Phase 4: Collision checking, trait registration & checking */
    for (int i = 0; i < algebra_reg.algebras.count; i++) {
        algebra_check_collisions(algebra_reg.algebras.items[i], &errors);
        trait_register_from_algebra(&trait_reg, algebra_reg.algebras.items[i]);
    }

    trait_check_all(&trait_reg);

    if (error_has_errors(&errors)) {
        error_print_all(&errors);
        goto cleanup;
    }

    /* Phase 5: Type checking */
    TypeChecker type_checker;
    type_checker_init(&type_checker, &arena, &intern_tab, &errors);
    type_check(&type_checker, ast);

    if (error_has_errors(&errors)) {
        error_print_all(&errors);
        goto cleanup;
    }

    /* Phase 6: Resolution */
    Resolver resolver;
    resolver_init(&resolver, &arena, &intern_tab, &algebra_reg, &trait_reg, &type_checker, &errors);
    resolve_ast(&resolver, ast);

    if (error_has_errors(&errors)) {
        error_print_all(&errors);
        goto cleanup;
    }

    /* Phase 7: Output */
    {
        FILE *out = stdout;
        if (output_path) {
            out = fopen(output_path, "w");
            if (!out) {
                fprintf(stderr, "error: cannot open output '%s'\n", output_path);
                goto cleanup;
            }
        }
        if (emit_c) {
            CEmitter c_emitter;
            c_emitter_init(&c_emitter, out, &type_checker, &arena);
            c_emit(&c_emitter, ast);
        } else {
            SkwEmitter emitter;
            skw_emitter_init(&emitter, out);
            skw_emit(&emitter, ast);
        }
        if (output_path) fclose(out);
    }

cleanup:
    da_free(&tokens);
    compound_sigil_set_free(&compounds);
    intern_free(&intern_tab);
    arena_free(&arena);
    free(source);

    return error_has_errors(&errors) ? 1 : 0;
}
