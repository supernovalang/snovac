/* ast.h — Snovalang abstract syntax tree.
 *
 * Every node is arena-allocated and never individually freed. Sequences use
 * SnList, a growable array of node pointers.
 *
 * Shapes here follow the corpus, not intuition:
 *   - Parameters carry optional defaults: `frame: TabularFrame = TabularFrame.new()`.
 *   - Match arms are NOT comma-separated; an arm body is an expression or a block.
 *   - `if` / `while` take no parentheses; `for` does: `for (let x in xs) { }`.
 *   - Construction has no `new` keyword: `LiveWorkbook(path, frame, false)`.
 *   - Types include function types: `(Context) -> unit`.
 *   - A method with no body is legal and means `@native` (checked in P2, not here).
 */
#ifndef SNOVAC_AST_H
#define SNOVAC_AST_H

#include "arena.h"
#include "token.h"

typedef struct SnType SnType;
typedef struct SnExpr SnExpr;
typedef struct SnStmt SnStmt;
typedef struct SnDecl SnDecl;

/* Forward-declared only (never defined here) so check.c (P2.5) can cache a
 * resolved SnTypeRep* directly on the AST node — plan.md's G2 requires every
 * SnExpr to carry a resolved type after check.c runs. types.h can't be
 * #included here: types.h -> symbol.h -> ast.h would be circular. A pointer
 * field needs no more than this forward declaration. */
typedef struct SnTypeRep SnTypeRep;

/* ── list ─────────────────────────────────────────────────────────────────── */

typedef struct {
    void **items;
    size_t len;
    size_t cap;
} SnList;

void sn_list_push(SnArena *a, SnList *l, void *item);

#define SN_LIST_AT(l, T, i) ((T *)((l).items[(i)]))

/* ── types ────────────────────────────────────────────────────────────────── */

typedef enum {
    SN_TYPE_NAME,  /* Foo, a.b.Foo, List<T>, Result<T, E> */
    SN_TYPE_FUNC,  /* (A, B) -> R */
    SN_TYPE_TUPLE  /* (K, V) — element types live in `params` */
} SnTypeKind;

struct SnType {
    SnTypeKind kind;
    SnSpan span;
    const char *name; /* NAME: dotted, e.g. "builtin.collections.List" */
    SnList args;      /* NAME: SnType* generic arguments */
    SnList params;    /* FUNC: SnType* parameter types */
    SnType *ret;      /* FUNC: return type */
    uint8_t is_optional; /* T? optional syntax */
};

/* ── expressions ──────────────────────────────────────────────────────────── */

typedef enum {
    SN_EXPR_INT, SN_EXPR_LONG, SN_EXPR_DOUBLE, SN_EXPR_DECIMAL,
    SN_EXPR_STRING, SN_EXPR_CHAR, SN_EXPR_BOOL,
    SN_EXPR_IDENT,
    SN_EXPR_THIS,
    SN_EXPR_MEMBER,  /* a.b        */
    SN_EXPR_CALL,    /* f(x), a.b<T>(x) */
    SN_EXPR_INDEX,   /* a[i]       */
    SN_EXPR_UNARY,   /* !x, -x     */
    SN_EXPR_BINARY,  /* a + b      */
    SN_EXPR_ASSIGN,  /* a = b, a += b */
    SN_EXPR_LAMBDA,  /* (x) -> e   */
    SN_EXPR_AWAIT,   /* await e    */
    SN_EXPR_ARRAY,   /* [a, b, c]  */
    SN_EXPR_CAST,    /* e as T     */
    SN_EXPR_IS,      /* e is T     */
    SN_EXPR_MATCH,   /* match e { } used in expression position */
    SN_EXPR_IF,      /* if c { a } else { b } used in expression position */
    SN_EXPR_STRUCT_LIT /* UserDto { id: "1" } — construction by field list */
} SnExprKind;

typedef struct SnParam SnParam;
typedef struct SnMatchArm SnMatchArm;

struct SnExpr {
    SnExprKind kind;
    SnSpan span;

    const char *text;   /* literals, IDENT and MEMBER name */
    SnExpr *lhs;        /* receiver / left operand / callee / assign target */
    SnExpr *rhs;        /* right operand / assigned value / index */
    SnTokKind op;       /* UNARY, BINARY, ASSIGN */
    SnList args;        /* CALL: SnExpr*; ARRAY: SnExpr* */
    SnList type_args;   /* CALL: SnType* explicit generic arguments */
    SnList params;      /* LAMBDA: SnParam* */
    SnStmt *body;       /* LAMBDA with a block body */
    SnExpr *value;      /* LAMBDA with an expression body */
    SnType *type;       /* CAST / IS; LAMBDA: declared return type, if written */
    SnList arms;        /* MATCH: SnMatchArm* */
    /* IF: `lhs` is the condition, `body` the then-block, and the else side is
     * either `else_body` (a block) or `rhs` (a chained else-if expression). */
    SnStmt *else_body;
    SnList field_names; /* STRUCT_LIT: const char* — values are in `args` */
    uint8_t interpolated; /* STRING: contains at least one ${...} */

    /* Set by check.c (P2.5), NULL before it runs. plan.md G2: every SnExpr
     * gets a resolved type; SN_T_ERROR (never NULL) marks a node whose type
     * couldn't be determined, so a NULL check here only ever means "check.c
     * hasn't reached this node yet," never "this expression has no type." */
    SnTypeRep *resolved_type;
};

/* ── patterns ─────────────────────────────────────────────────────────────── */

typedef enum {
    SN_PAT_WILDCARD, /* _            */
    SN_PAT_BINDING,  /* name         */
    SN_PAT_VARIANT,  /* Ok(v), None, Some(x) */
    SN_PAT_LITERAL   /* 1, "s", true */
} SnPatKind;

typedef struct SnPattern SnPattern;
struct SnPattern {
    SnPatKind kind;
    SnSpan span;
    const char *name; /* BINDING / VARIANT path */
    SnList subs;      /* VARIANT: SnPattern* */
    SnExpr *literal;  /* LITERAL */
};

struct SnMatchArm {
    SnPattern *pattern;
    SnExpr *guard; /* `if <expr>` validation predicate, or NULL */
    SnExpr *value; /* arm body as expression, or NULL */
    SnStmt *body;  /* arm body as block/statement, or NULL */
    SnSpan span;
};

/* ── statements ───────────────────────────────────────────────────────────── */

typedef enum {
    SN_STMT_LET, SN_STMT_VAR,
    SN_STMT_EXPR,
    SN_STMT_RETURN,
    SN_STMT_IF,
    SN_STMT_WHILE,
    SN_STMT_FOR,
    SN_STMT_MATCH,
    SN_STMT_BLOCK,
    SN_STMT_BREAK, SN_STMT_CONTINUE,
    SN_STMT_THROW, SN_STMT_DEFER,
    SN_STMT_TRY,
    SN_STMT_PULSAR
} SnStmtKind;

struct SnStmt {
    SnStmtKind kind;
    SnSpan span;

    const char *name;  /* LET / VAR / FOR binding */
    SnType *type;      /* LET / VAR annotation, may be NULL */
    SnExpr *expr;      /* initializer / condition / subject / returned value */
    SnStmt *then_br;   /* IF then, WHILE/FOR body, TRY body, DEFER body */
    SnStmt *else_br;   /* IF else */
    SnList stmts;      /* BLOCK: SnStmt* */
    SnList arms;       /* MATCH: SnMatchArm* */
    SnList catches;    /* TRY: SnStmt* (each a BLOCK with name set) */
    SnStmt *finally_br; /* TRY: `finally { ... }` block, or NULL */
    /* Extra names bound by a multi-target receive-bind, `a, b <~ expr`. The
     * first name is in `name`; this holds the rest, so the single-target form
     * leaves it empty and reads exactly like LET/VAR. */
    SnList extra_names; /* VAR from `<~`: const char* */
};

/* ── declarations ─────────────────────────────────────────────────────────── */

typedef enum {
    SN_VIS_DEFAULT = 0, SN_VIS_PUBLIC, SN_VIS_PRIVATE, SN_VIS_PROTECTED
} SnVisibility;

typedef struct SnDecorator SnDecorator;
struct SnDecorator {
    const char *name;
    SnSpan span;
    SnList arg_names;  /* const char* (NULL when positional) */
    SnList arg_values; /* SnExpr* (may be NULL when the arg is a bare type) */
    SnList arg_types;  /* SnType* (set for bare type refs like returnType: List<T>) */
};

struct SnParam {
    const char *name;
    SnType *type;
    SnExpr *def; /* default value, or NULL */
    SnSpan span;
};

/* One accessor of a property block: `get;` / `get: (x) -> "*******";`.
 *
 * The bare form leaves `param` and `proj` NULL and means "expose the field
 * itself". The projection form names the field locally — the name is arbitrary
 * and scoped to the projection, so `password` may be spelled `x` — and yields
 * the expression in its place. Which side of the boundary a projection applies
 * to is semantics, owned by P2; the parser only records the shape. */
typedef struct {
    const char *param; /* projection parameter, NULL for the bare form */
    SnExpr *proj;      /* projection body, NULL for the bare form */
    SnSpan span;
} SnAccessor;

typedef enum {
    SN_DECL_CLASS, SN_DECL_STRUCT, SN_DECL_ENUM, SN_DECL_INTERFACE,
    SN_DECL_FUNC, SN_DECL_METHOD, SN_DECL_FIELD, SN_DECL_CONST,
    SN_DECL_VARIANT, SN_DECL_TYPEALIAS,
    SN_DECL_EXTENSION /* extension Counter { ... } — members in `members` */
} SnDeclKind;

struct SnDecl {
    SnDeclKind kind;
    SnSpan span;

    const char *name;
    SnVisibility vis;
    uint8_t is_static;
    uint8_t is_override;
    uint8_t is_async;
    uint8_t is_abstract; /* CLASS/STRUCT/INTERFACE declared `abstract` */
    uint8_t is_pulsar;   /* FUNC/METHOD declared `pulsar func`/`pulsar method` */
    uint8_t is_mutable; /* FIELD declared with `var` */

    SnList decorators;   /* SnDecorator* */
    SnList generics;     /* const char* type parameter names */
    SnList params;       /* FUNC / METHOD / VARIANT: SnParam* */
    SnType *ret;         /* FUNC / METHOD return type, may be NULL */
    SnType *type;        /* FIELD / CONST type, may be NULL */
    SnExpr *init;        /* FIELD / CONST initializer, may be NULL */
    /* FIELD: property accessors. `has_accessors` distinguishes a field written
     * with an empty block from one written with no block at all — a field with
     * no block keeps the plain visibility rules, so the unannotated corpus is
     * unaffected. `getter`/`setter` are NULL when that accessor is absent,
     * which is what makes the field unreadable/unassignable from outside. */
    uint8_t has_accessors;
    SnAccessor *getter;
    SnAccessor *setter;
    SnStmt *body;        /* FUNC / METHOD body; NULL means @native */
    SnList members;      /* CLASS / STRUCT / INTERFACE: SnDecl* */
    SnList variants;     /* ENUM: SnDecl* of kind SN_DECL_VARIANT */
    /* Supertypes written after `:` — base class and/or implemented interfaces,
     * in source order. The parser does not distinguish the two: telling a class
     * from an interface needs symbol resolution, which is P2's job. Empty for a
     * type that declares none; the implicit `object` root is supplied by P2, not
     * recorded here. */
    SnList supertypes;   /* CLASS / STRUCT / INTERFACE: SnType* */
};

/* ── compilation unit ─────────────────────────────────────────────────────── */

typedef struct {
    const char *package;  /* may be NULL */
    SnSpan package_span;
    SnList imports;       /* const char* */
    SnList decls;         /* SnDecl* */
} SnUnit;

#endif /* SNOVAC_AST_H */
