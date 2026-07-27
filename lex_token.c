/* lex_token.c — the keyword table and token spelling used by diagnostics. */
#include "lex_internal.h"

typedef struct {
    const char *word;
    SnTokKind kind;
} Keyword;

/* Only words that actually introduce syntax. Deliberately absent, because the
 * corpus uses them as ordinary identifiers: and, or, not, when, select, spawn,
 * yield, short, byte, char, data, value, key, name, get, new-as-member.
 * Primitive type names (int, long, double, decimal, bool, string, unit) are NOT
 * keywords either — they are ordinary type references resolved by name, which
 * is what lets `builtin.types` declare them in .snova. */
static const Keyword KEYWORDS[] = {
    {"package", SN_TOK_PACKAGE},     {"import", SN_TOK_IMPORT},
    {"class", SN_TOK_CLASS},         {"struct", SN_TOK_STRUCT},
    {"enum", SN_TOK_ENUM},           {"interface", SN_TOK_INTERFACE},
    {"trait", SN_TOK_INTERFACE},
    {"method", SN_TOK_METHOD},       {"func", SN_TOK_FUNC},
    {"static", SN_TOK_STATIC},       {"public", SN_TOK_PUBLIC},
    {"private", SN_TOK_PRIVATE},     {"protected", SN_TOK_PROTECTED},
    {"override", SN_TOK_OVERRIDE},   {"let", SN_TOK_LET},
    {"var", SN_TOK_VAR},             {"const", SN_TOK_CONST},
    {"return", SN_TOK_RETURN},       {"if", SN_TOK_IF},
    {"else", SN_TOK_ELSE},           {"while", SN_TOK_WHILE},
    {"for", SN_TOK_FOR},             {"in", SN_TOK_IN},
    {"match", SN_TOK_MATCH},         {"break", SN_TOK_BREAK},
    {"continue", SN_TOK_CONTINUE},   {"try", SN_TOK_TRY},
    {"catch", SN_TOK_CATCH},         {"throw", SN_TOK_THROW},
    {"defer", SN_TOK_DEFER},         {"async", SN_TOK_ASYNC},
    {"await", SN_TOK_AWAIT},         {"pulsar", SN_TOK_PULSAR},
    {"this", SN_TOK_THIS},           {"new", SN_TOK_NEW},
    {"as", SN_TOK_AS},               {"is", SN_TOK_IS},
    {"true", SN_TOK_TRUE},           {"false", SN_TOK_FALSE},
};

#define KEYWORD_COUNT (sizeof(KEYWORDS) / sizeof(KEYWORDS[0]))

SnTokKind keyword_lookup(const char *s, size_t n) {
    for (size_t i = 0; i < KEYWORD_COUNT; i++) {
        if (strlen(KEYWORDS[i].word) == n &&
            memcmp(KEYWORDS[i].word, s, n) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return SN_TOK_IDENT;
}

const char *keyword_spelling(SnTokKind k) {
    for (size_t i = 0; i < KEYWORD_COUNT; i++) {
        if (KEYWORDS[i].kind == k) {
            return KEYWORDS[i].word;
        }
    }
    return NULL;
}

int sn_tok_is_keyword(SnTokKind k) {
    return k >= SN_TOK_PACKAGE && k <= SN_TOK_FALSE;
}

const char *sn_tok_name(SnTokKind k) {
    switch (k) {
    case SN_TOK_EOF: return "end of file";
    case SN_TOK_ERROR: return "invalid token";
    case SN_TOK_IDENT: return "identifier";
    case SN_TOK_INT: return "int literal";
    case SN_TOK_LONG: return "long literal";
    case SN_TOK_DOUBLE: return "double literal";
    case SN_TOK_DECIMAL: return "decimal literal";
    case SN_TOK_STRING: return "string literal";
    case SN_TOK_CHAR: return "char literal";
    default: break;
    }

    const char *kw = keyword_spelling(k);
    if (kw) {
        return kw;
    }

    switch (k) {
    case SN_TOK_LPAREN: return "(";
    case SN_TOK_RPAREN: return ")";
    case SN_TOK_LBRACE: return "{";
    case SN_TOK_RBRACE: return "}";
    case SN_TOK_LBRACKET: return "[";
    case SN_TOK_RBRACKET: return "]";
    case SN_TOK_COMMA: return ",";
    case SN_TOK_DOT: return ".";
    case SN_TOK_COLON: return ":";
    case SN_TOK_SEMI: return ";";
    case SN_TOK_AT: return "@";
    case SN_TOK_QUESTION: return "?";
    case SN_TOK_QQ: return "??";
    case SN_TOK_QDOT: return "?.";
    case SN_TOK_COLONCOLON: return "::";
    case SN_TOK_UNDERSCORE: return "_";
    case SN_TOK_PLUS: return "+";
    case SN_TOK_MINUS: return "-";
    case SN_TOK_STAR: return "*";
    case SN_TOK_SLASH: return "/";
    case SN_TOK_PERCENT: return "%";
    case SN_TOK_ASSIGN: return "=";
    case SN_TOK_EQ: return "==";
    case SN_TOK_NE: return "!=";
    case SN_TOK_LT: return "<";
    case SN_TOK_GT: return ">";
    case SN_TOK_LE: return "<=";
    case SN_TOK_GE: return ">=";
    case SN_TOK_ANDAND: return "&&";
    case SN_TOK_OROR: return "||";
    case SN_TOK_BANG: return "!";
    case SN_TOK_AMP: return "&";
    case SN_TOK_PIPE: return "|";
    case SN_TOK_CARET: return "^";
    case SN_TOK_TILDE: return "~";
    case SN_TOK_ARROW: return "->";
    case SN_TOK_TILDE_ARROW: return "~>";
    case SN_TOK_RECV_BIND: return "<~";
    case SN_TOK_FATARROW: return "=>";
    case SN_TOK_PLUS_EQ: return "+=";
    case SN_TOK_MINUS_EQ: return "-=";
    case SN_TOK_STAR_EQ: return "*=";
    case SN_TOK_SLASH_EQ: return "/=";
    case SN_TOK_SHL: return "<<";
    case SN_TOK_SHR: return ">>";
    default: return "token";
    }
}
