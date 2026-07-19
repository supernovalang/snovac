/* token.h — Snovalang token kinds.
 *
 * The keyword set and the operator set below are derived from the real corpus
 * (245 .snova files across libs/, stdlib/, tests/, examples/), not from
 * intuition. Findings that shaped this file, each verified by measurement:
 *
 *   - There is no `>>` or `<<` token. Every `>>` in the corpus (214 of them) is
 *     a nested generic close: `Task<Result<unit, DataError>>`. Snovalang has no
 *     shift operators, so the lexer always emits two separate SN_TOK_GT.
 *   - There is no `::` and no `..`. Both appear only inside comments and string
 *     literals, never in code.
 *   - `and` / `or` / `not` are NOT operators. They appear as method names
 *     (`static method and(left: SqlFragment, ...)`) and in prose. Boolean logic
 *     is spelled `&&` / `||` / `!`.
 *   - `method`, `catch`, `spawn`, `select`, `yield`, `when`, `byte`, `char` and
 *     `short` all occur as identifiers in real code (`func send(method: string)`,
 *     `handle.catch(...)`). Keywords are therefore CONTEXTUAL: the lexer emits a
 *     keyword token, and the parser demotes it to an identifier wherever a name
 *     is expected — after `.`, and in parameter/field name position. See
 *     sn_tok_is_keyword() and the parser's expect_name().
 */
#ifndef SNOVAC_TOKEN_H
#define SNOVAC_TOKEN_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SN_TOK_EOF = 0,
    SN_TOK_ERROR,

    SN_TOK_IDENT,
    SN_TOK_INT,      /* 42, 0xFF            */
    SN_TOK_LONG,     /* 42L                 */
    SN_TOK_DOUBLE,   /* 1.5, 1e9            */
    SN_TOK_DECIMAL,  /* 1.5d / decimal ctx  */
    SN_TOK_STRING,   /* "...", may carry ${} interpolation */
    SN_TOK_CHAR,     /* '.'                 */

    /* Declaration keywords */
    SN_TOK_PACKAGE, SN_TOK_IMPORT,
    SN_TOK_CLASS, SN_TOK_STRUCT, SN_TOK_ENUM, SN_TOK_INTERFACE,
    SN_TOK_METHOD, SN_TOK_FUNC, SN_TOK_STATIC,
    SN_TOK_PUBLIC, SN_TOK_PRIVATE, SN_TOK_PROTECTED, SN_TOK_OVERRIDE,
    SN_TOK_LET, SN_TOK_VAR, SN_TOK_CONST,

    /* Statement / expression keywords */
    SN_TOK_RETURN, SN_TOK_IF, SN_TOK_ELSE, SN_TOK_WHILE, SN_TOK_FOR,
    SN_TOK_IN, SN_TOK_MATCH, SN_TOK_BREAK, SN_TOK_CONTINUE,
    SN_TOK_TRY, SN_TOK_CATCH, SN_TOK_THROW, SN_TOK_DEFER,
    SN_TOK_ASYNC, SN_TOK_AWAIT, SN_TOK_PULSAR,
    SN_TOK_THIS, SN_TOK_NEW, SN_TOK_AS, SN_TOK_IS,
    SN_TOK_TRUE, SN_TOK_FALSE,

    /* Punctuation */
    SN_TOK_LPAREN, SN_TOK_RPAREN,
    SN_TOK_LBRACE, SN_TOK_RBRACE,
    SN_TOK_LBRACKET, SN_TOK_RBRACKET,
    SN_TOK_COMMA, SN_TOK_DOT, SN_TOK_COLON, SN_TOK_SEMI,
    SN_TOK_AT,          /* @decorator */
    SN_TOK_QUESTION,
    SN_TOK_QQ,          /* ??  null-coalescing */
    /* `::` is NOT Snovalang syntax. It is lexed as its own token purely so the
     * parser can say "found `::`" instead of reporting two stray colons. The
     * only occurrence in the repository is
     * examples/snovalang/lambda_functions.snova:59 (`Int::toString`), which is
     * invalid code in an unvalidated example — not a language feature. */
    SN_TOK_COLONCOLON,
    SN_TOK_UNDERSCORE,  /* wildcard pattern */

    /* Operators */
    SN_TOK_PLUS, SN_TOK_MINUS, SN_TOK_STAR, SN_TOK_SLASH, SN_TOK_PERCENT,
    SN_TOK_ASSIGN,      /* =  */
    SN_TOK_EQ, SN_TOK_NE, SN_TOK_LT, SN_TOK_GT, SN_TOK_LE, SN_TOK_GE,
    SN_TOK_ANDAND, SN_TOK_OROR, SN_TOK_BANG,
    SN_TOK_AMP, SN_TOK_PIPE, SN_TOK_CARET, SN_TOK_TILDE,
    SN_TOK_ARROW,       /* ->  lambda / return type */
    SN_TOK_TILDE_ARROW, /* ~>  pulsar stream return: `(T) ~> Channel<R>` */
    SN_TOK_FATARROW,    /* =>  match arm */
    SN_TOK_PLUS_EQ, SN_TOK_MINUS_EQ, SN_TOK_STAR_EQ, SN_TOK_SLASH_EQ,

    SN_TOK__COUNT
} SnTokKind;

typedef struct {
    uint32_t offset; /* byte offset into the source buffer */
    uint32_t len;
    uint32_t line;   /* 1-based */
    uint32_t col;    /* 1-based, in bytes */
} SnSpan;

typedef struct {
    SnTokKind kind;
    SnSpan span;
    const char *text; /* interned; for STRING this is the raw slice with quotes */
    /* STRING only: set when the literal contains at least one `${...}`.
     * Interpolation is parsed in the parser, not here — the lexer only records
     * that the string is not a plain constant. `$$` is a literal `$` and does
     * not set this flag. */
    uint8_t has_interpolation;
} SnToken;

const char *sn_tok_name(SnTokKind k);
int         sn_tok_is_keyword(SnTokKind k);

#endif /* SNOVAC_TOKEN_H */
