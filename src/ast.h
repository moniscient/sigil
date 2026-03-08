#ifndef SIGIL_AST_H
#define SIGIL_AST_H

#include "common.h"

/* ── AST Node Types ──────────────────────────────────────────────── */

typedef enum {
    /* Declarations */
    NODE_FN_DECL,
    NODE_TRAIT_DECL,
    NODE_IMPLEMENT,
    NODE_ALGEBRA,
    NODE_LIBRARY,
    NODE_USE,
    NODE_PRECEDENCE,
    NODE_TYPE_DECL,

    /* Statements */
    NODE_LET,
    NODE_VAR,
    NODE_ASSIGN,
    NODE_RETURN,

    /* Control flow */
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_MATCH,
    NODE_CASE,
    NODE_DEFAULT,

    /* Expressions */
    NODE_CALL,         /* keyword-prefix function call */
    NODE_SIGIL_EXPR,   /* sigil expression (pre-desugar) */
    NODE_BEGIN_END,    /* begin/end group */
    NODE_IDENT,
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_BOOL_LIT,
    NODE_STRING_LIT,

    /* Special */
    NODE_BLOCK,        /* sequence of statements */
    NODE_PARAM,        /* function parameter */
    NODE_TYPE_REF,     /* type reference */
    NODE_PROGRAM       /* top-level program */
} NodeKind;

/* ── Type References ─────────────────────────────────────────────── */

typedef enum {
    TYPE_BOOL, TYPE_INT, TYPE_FLOAT, TYPE_CHAR, TYPE_MAP, TYPE_VOID,
    TYPE_INT8, TYPE_INT16, TYPE_INT32, TYPE_INT64,
    TYPE_FLOAT32, TYPE_FLOAT64,
    TYPE_NAMED,        /* user-defined / alias */
    TYPE_GENERIC,      /* type variable T */
    TYPE_TRAIT_BOUND,  /* Trait T */
    TYPE_ITER,
    TYPE_UNKNOWN
} TypeKind;

typedef struct TypeRef {
    TypeKind kind;
    const char *name;         /* for NAMED, GENERIC, TRAIT_BOUND */
    const char *trait_name;   /* for TRAIT_BOUND: the trait constraining the type var */
    struct TypeRef *key_type; /* for MAP */
    struct TypeRef *val_type; /* for MAP */
} TypeRef;

/* ── Fixity (for fn pattern analysis) ────────────────────────────── */

typedef enum {
    FIXITY_NONE,       /* no sigil */
    FIXITY_PREFIX,
    FIXITY_POSTFIX,
    FIXITY_INFIX,
    FIXITY_BRACKETED,
    FIXITY_NULLARY
} Fixity;

/* ── Pattern Element (in fn declaration) ─────────────────────────── */

typedef enum {
    PAT_PARAM,    /* typed parameter: type name */
    PAT_SIGIL,    /* sigil token */
    PAT_REPEATS   /* repeats keyword */
} PatElemKind;

typedef struct PatElem {
    PatElemKind kind;
    const char *sigil;     /* for PAT_SIGIL */
    TypeRef *type;         /* for PAT_PARAM */
    const char *param_name;/* for PAT_PARAM */
    bool is_mutable;       /* for PAT_PARAM: var annotation = pass by mutable reference */
} PatElem;

/* ── AST Node ────────────────────────────────────────────────────── */

typedef struct ASTNode ASTNode;

DA_TYPEDEF(ASTNode*, NodeList)
DA_TYPEDEF(PatElem, PatList)
DA_TYPEDEF(const char*, StrList)

struct ASTNode {
    NodeKind kind;
    SrcLoc loc;
    TypeRef *resolved_type;  /* populated by type checker for C emitter */

    union {
        /* NODE_FN_DECL */
        struct {
            const char *fn_name;
            PatList pattern;
            TypeRef *return_type;
            ASTNode *body;
            Fixity fixity;
            StrList sigils;    /* extracted sigils from pattern */
            bool is_primitive; /* body is <primitive> */
        } fn_decl;

        /* NODE_TRAIT_DECL */
        struct {
            const char *trait_name;
            const char *type_var;
            NodeList methods;  /* list of NODE_FN_DECL */
            StrList requires;  /* prerequisite trait names */
        } trait_decl;

        /* NODE_IMPLEMENT */
        struct {
            const char *trait_name;
            const char *concrete_type;
            NodeList methods;
        } implement;

        /* NODE_ALGEBRA / NODE_LIBRARY */
        struct {
            const char *algebra_name;
            NodeList declarations;
        } algebra;

        /* NODE_USE */
        struct {
            const char *algebra_name;
            ASTNode *body;
        } use_block;

        /* NODE_PRECEDENCE */
        struct {
            StrList sigils;  /* low to high */
        } precedence;

        /* NODE_LET / NODE_VAR */
        struct {
            const char *bind_name;
            ASTNode *value;
        } binding;

        /* NODE_ASSIGN */
        struct {
            const char *assign_name;
            ASTNode *value;
        } assign;

        /* NODE_RETURN */
        struct {
            ASTNode *value;
        } ret;

        /* NODE_IF */
        struct {
            ASTNode *condition;
            ASTNode *then_body;
            NodeList elifs;     /* pairs: condition, body */
            ASTNode *else_body; /* may be NULL */
        } if_stmt;

        /* NODE_WHILE */
        struct {
            ASTNode *condition;
            ASTNode *while_body;
        } while_stmt;

        /* NODE_FOR */
        struct {
            const char *var_name;
            ASTNode *iterable;
            ASTNode *for_body;
        } for_stmt;

        /* NODE_MATCH */
        struct {
            ASTNode *match_value;
            NodeList cases;
        } match_stmt;

        /* NODE_CASE */
        struct {
            ASTNode *case_pattern;
            ASTNode *case_body;
        } case_branch;

        /* NODE_DEFAULT */
        struct {
            ASTNode *default_body;
        } default_branch;

        /* NODE_CALL */
        struct {
            const char *call_name;
            NodeList args;
        } call;

        /* NODE_SIGIL_EXPR (pre-desugar) */
        struct {
            const char *sigil;
            NodeList operands;
            Fixity expr_fixity;
        } sigil_expr;

        /* NODE_BEGIN_END / NODE_BLOCK */
        struct {
            NodeList stmts;
        } block;

        /* NODE_IDENT */
        struct {
            const char *ident;
        } ident;

        /* NODE_INT_LIT */
        struct {
            int64_t int_val;
        } int_lit;

        /* NODE_FLOAT_LIT */
        struct {
            double float_val;
        } float_lit;

        /* NODE_BOOL_LIT */
        struct {
            bool bool_val;
        } bool_lit;

        /* NODE_STRING_LIT */
        struct {
            const char *str_val;  /* raw text content */
            int str_len;          /* length in bytes */
        } string_lit;

        /* NODE_PARAM */
        struct {
            TypeRef *param_type;
            const char *param_name;
        } param;

        /* NODE_TYPE_REF */
        struct {
            TypeRef *type;
        } type_ref;

        /* NODE_TYPE_DECL */
        struct {
            const char *type_name;
            int field_count;
            TypeRef **field_types;
            const char **field_names;
        } type_decl;

        /* NODE_PROGRAM */
        struct {
            NodeList top_level;
        } program;
    };
};

/* ── AST Construction Helpers ────────────────────────────────────── */

static inline ASTNode *ast_new(Arena *a, NodeKind kind, SrcLoc loc) {
    ASTNode *n = (ASTNode *)arena_alloc(a, sizeof(ASTNode));
    memset(n, 0, sizeof(ASTNode));
    n->kind = kind;
    n->loc = loc;
    return n;
}

#endif /* SIGIL_AST_H */
