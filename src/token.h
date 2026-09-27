#ifndef Z_TOKEN_H
#define Z_TOKEN_H

#include "source.h"

typedef enum {
    T_EOF,
    T_IDENT,
    T_INT,
    T_F64, /* a floating-point literal; value in `dval` */
    T_STRING,
    T_INTERP, /* $"...{expr}..." : raw content in `text` */

    /* Type keywords */
    T_KW_INT,
    T_KW_BOOL,
    T_KW_FLOAT,
    T_KW_STRING,
    T_KW_VOID,

    /* Statement keywords */
    T_KW_VAR,
    T_KW_CONST,
    T_KW_NEW,
    T_KW_STRUCT,
    T_KW_ENUM,
    T_KW_CLASS,
    T_KW_INTERFACE,
    T_KW_VIRTUAL,
    T_KW_OVERRIDE,
    T_KW_MATCH,
    T_KW_THIS,
    T_KW_IF,
    T_KW_ELSE,
    T_KW_WHILE,
    T_KW_FOR,
    T_KW_FOREACH,
    T_KW_IN,
    T_KW_RETURN,
    T_KW_BREAK,
    T_KW_CONTINUE,
    T_KW_TRUE,
    T_KW_FALSE,
    T_KW_NULL,
    T_KW_EXTERN,
    T_KW_EXPORT,
    T_KW_FN,
    T_KW_CLOSURE,
    T_KW_METHOD,
    T_KW_IMPORT,

    /* Punctuation */
    T_LPAREN,
    T_RPAREN,
    T_LBRACE,
    T_RBRACE,
    T_LBRACKET,
    T_RBRACKET,
    T_SEMI,
    T_COMMA,
    T_DOT,
    T_DOTDOT, /* `..`, the range in `s[a..b]`. Distinct from T_DOT so that the
               * lexer can tell a slice from a field access without lookahead. */
    T_QUESTION,
    T_COLON,
    T_FATARROW,
    T_ARROW,

    /* Operators */
    T_PLUS,
    T_MINUS,
    T_STAR,
    T_SLASH,
    T_PERCENT,
    T_ASSIGN,
    T_EQ,
    T_NE,
    T_LT,
    T_LE,
    T_GT,
    T_GE,
    T_NOT,
    T_AMP,
    T_AND,
    T_OR,
    T_PLUS_EQ,
    T_MINUS_EQ,
    T_STAR_EQ,
    T_SLASH_EQ,
    T_PERCENT_EQ,
    T_PLUSPLUS,
    T_MINUSMINUS,
    T_PIPE,   /* |  */
    T_CARET,  /* ^  */
    T_TILDE,  /* ~  */
    T_SHL,    /* << */
    T_SHR,    /* >> */
    T_AMP_EQ,
    T_PIPE_EQ,
    T_CARET_EQ,
    T_SHL_EQ,
    T_SHR_EQ,
} TokenKind;

typedef struct {
    TokenKind kind;
    Span span;
    long long ival; /* T_INT */
    double dval;    /* T_F64 */
    char *text;     /* T_IDENT name, or decoded T_STRING bytes */
    int str_id;     /* T_STRING: index into the string table */
} Token;

/* Human-readable token name, for error messages ("';'" vs "identifier"). */
const char *token_kind_name(TokenKind kind);

#endif /* Z_TOKEN_H */
