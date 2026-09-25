#include "lexer.h"

#include "diag.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *src;
    int len;
    int pos;
    int line;
    int col;
    StringTable *strings;
} Lexer;

void string_table_init(StringTable *table, Arena *arena) {
    table->arena = arena;
    table->items = NULL;
    table->count = 0;
    table->cap = 0;
}

int string_intern(StringTable *table, const char *bytes, int len) {
    for (int i = 0; i < table->count; i++) {
        char *s = table->items[i];
        if ((int)strlen(s) == len && memcmp(s, bytes, (size_t)len) == 0)
            return i;
    }
    if (table->count == table->cap) {
        int ncap = table->cap == 0 ? 8 : table->cap * 2;
        char **nitems = arena_alloc_array(table->arena, (size_t)ncap, sizeof(char *));
        if (table->items != NULL)
            memcpy(nitems, table->items, (size_t)table->count * sizeof(char *));
        table->items = nitems;
        table->cap = ncap;
    }
    char *copy = arena_strndup(table->arena, bytes, (size_t)len);
    table->items[table->count++] = copy;
    return table->count - 1;
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

static Span span_at(int start, int len) {
    Span s;
    s.file = NULL;
    s.start = start;
    s.len = len;
    s.line = 0;
    s.col = 0;
    return s;
}

typedef struct {
    const char *name;
    TokenKind kind;
} Keyword;

/* A multi-byte operator/punctuation literal and the token it produces. */
typedef struct {
    const char *text;
    TokenKind kind;
} Op;
static const Keyword KEYWORDS[] = {
    {"int", T_KW_INT},         {"bool", T_KW_BOOL},
    {"string", T_KW_STRING},   {"void", T_KW_VOID},
    {"var", T_KW_VAR},         {"new", T_KW_NEW},
    {"struct", T_KW_STRUCT},   {"enum", T_KW_ENUM},
    {"match", T_KW_MATCH},     {"this", T_KW_THIS},
    {"if", T_KW_IF},           {"else", T_KW_ELSE},
    {"while", T_KW_WHILE},     {"for", T_KW_FOR},
    {"foreach", T_KW_FOREACH}, {"in", T_KW_IN},
    {"return", T_KW_RETURN},   {"true", T_KW_TRUE},
    {"false", T_KW_FALSE},     {"class", T_KW_CLASS},
    {"virtual", T_KW_VIRTUAL}, {"override", T_KW_OVERRIDE},
};

static int peek_char(Lexer *lx, int off) {
    int i = lx->pos + off;
    if (i >= lx->len)
        return -1;
    return (unsigned char)lx->src[i];
}

/* Advances one byte, maintaining line/col. */
static void bump(Lexer *lx) {
    if (lx->pos < lx->len) {
        if (lx->src[lx->pos] == '\n') {
            lx->line++;
            lx->col = 1;
        } else {
            lx->col++;
        }
        lx->pos++;
    }
}

static void skip_trivia(Lexer *lx) {
    for (;;) {
        int c = peek_char(lx, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            bump(lx);
        } else if (c == '/' && peek_char(lx, 1) == '/') {
            while (peek_char(lx, 0) != -1 && peek_char(lx, 0) != '\n')
                bump(lx);
        } else if (c == '/' && peek_char(lx, 1) == '*') {
            int start = lx->pos, sline = lx->line, scol = lx->col;
            bump(lx);
            bump(lx);
            while (!(peek_char(lx, 0) == '*' && peek_char(lx, 1) == '/')) {
                if (peek_char(lx, 0) == -1) {
                    Span s = span_at(start, 2);
                    s.line = sline;
                    s.col = scol;
                    diag_error(s, "unterminated block comment");
                    return;
                }
                bump(lx);
            }
            bump(lx);
            bump(lx);
        } else {
            return;
        }
    }
}

/* Tokenizes the entire source into an arena array. */
Token *lex_all(Arena *arena, const char *src, int len, StringTable *strings, int *out_count) {
    Lexer lx;
    lx.src = src;
    lx.len = len;
    lx.pos = 0;
    lx.line = 1;
    lx.col = 1;
    lx.strings = strings;

    int cap = 64, count = 0;
    Token *toks = arena_alloc_array(arena, (size_t)cap, sizeof(Token));

    for (;;) {
        skip_trivia(&lx);
        int start = lx.pos, sline = lx.line, scol = lx.col;
        int c = peek_char(&lx, 0);

        if (count + 1 >= cap) {
            int ncap = cap * 2;
            Token *nt = arena_alloc_array(arena, (size_t)ncap, sizeof(Token));
            memcpy(nt, toks, (size_t)count * sizeof(Token));
            toks = nt;
            cap = ncap;
        }

        Token tk;
        memset(&tk, 0, sizeof tk);
        tk.ival = 0;
        tk.str_id = -1;

        if (c == -1) {
            tk.kind = T_EOF;
            tk.span = span_at(start, 0);
            tk.span.line = sline;
            tk.span.col = scol;
            toks[count++] = tk;
            break;
        }

        if (is_digit(c)) {
            long long val = 0;
            while (is_digit(peek_char(&lx, 0))) {
                val = val * 10 + (peek_char(&lx, 0) - '0');
                bump(&lx);
            }
            tk.kind = T_INT;
            tk.ival = val;
        } else if (is_alpha(c)) {
            int b = lx.pos;
            while (is_alnum(peek_char(&lx, 0)))
                bump(&lx);
            tk.text = arena_strndup(arena, src + b, (size_t)(lx.pos - b));
            tk.kind = T_IDENT;
            for (size_t k = 0; k < sizeof KEYWORDS / sizeof(*KEYWORDS); k++) {
                if (strcmp(tk.text, KEYWORDS[k].name) == 0) {
                    tk.kind = KEYWORDS[k].kind;
                    break;
                }
            }
        } else if (c == '"') {
            bump(&lx);
            char *decoded = arena_alloc(arena, (size_t)len + 1);
            int dlen = 0;
            for (;;) {
                int d = peek_char(&lx, 0);
                if (d == -1 || d == '\n') {
                    Span s = span_at(start, lx.pos - start);
                    s.line = sline;
                    s.col = scol;
                    diag_error(s, "unterminated string");
                    break;
                }
                if (d == '"') {
                    bump(&lx);
                    break;
                }
                if (d == '\\') {
                    bump(&lx);
                    int e = peek_char(&lx, 0);
                    switch (e) {
                    case 'n':
                        decoded[dlen++] = '\n';
                        break;
                    case 't':
                        decoded[dlen++] = '\t';
                        break;
                    case 'r':
                        decoded[dlen++] = '\r';
                        break;
                    case '0':
                        decoded[dlen++] = '\0';
                        break;
                    case '\\':
                        decoded[dlen++] = '\\';
                        break;
                    case '"':
                        decoded[dlen++] = '"';
                        break;
                    default:
                        decoded[dlen++] = (char)e;
                        break;
                    }
                    bump(&lx);
                } else {
                    decoded[dlen++] = (char)d;
                    bump(&lx);
                }
            }
            decoded[dlen] = '\0';
            tk.kind = T_STRING;
            tk.text = decoded;
            tk.str_id = string_intern(strings, decoded, dlen);
        } else if (c == '$' && peek_char(&lx, 1) == '"') {
            /* Interpolated string $"...". Store the raw content (with {expr}
             * markers preserved); the parser splits it into parts. */
            bump(&lx); /* '$' */
            bump(&lx); /* '"' */
            char *raw = arena_alloc(arena, (size_t)len + 1);
            int rlen = 0;
            int bdepth = 0; /* nesting of {..} so quotes inside are kept */
            for (;;) {
                int d = peek_char(&lx, 0);
                if (d == -1 || d == '\n') {
                    Span s = span_at(start, lx.pos - start);
                    s.line = sline;
                    s.col = scol;
                    diag_error(s, "unterminated string");
                    break;
                }
                if (d == '\\') {
                    bump(&lx);
                    int e = peek_char(&lx, 0);
                    switch (e) {
                    case 'n':
                        raw[rlen++] = '\n';
                        break;
                    case 't':
                        raw[rlen++] = '\t';
                        break;
                    case 'r':
                        raw[rlen++] = '\r';
                        break;
                    case '0':
                        raw[rlen++] = '\0';
                        break;
                    case '\\':
                        raw[rlen++] = '\\';
                        break;
                    case '"':
                        raw[rlen++] = '"';
                        break;
                    case '{':
                        raw[rlen++] = '{';
                        break;
                    case '}':
                        raw[rlen++] = '}';
                        break;
                    default:
                        raw[rlen++] = (char)e;
                        break;
                    }
                    bump(&lx);
                    continue;
                }
                if (d == '"' && bdepth == 0) {
                    bump(&lx);
                    break;
                }
                if (d == '{')
                    bdepth++;
                if (d == '}')
                    bdepth--;
                raw[rlen++] = (char)d;
                bump(&lx);
            }
            raw[rlen] = '\0';
            tk.kind = T_INTERP;
            tk.text = raw;
        } else {
            /* Operators and punctuation, longest match first. */
            static const Op OPS[] = {
                {"=>", T_FATARROW}, {"==", T_EQ},         {"!=", T_NE},       {"<=", T_LE},
                {">=", T_GE},       {"&&", T_AND},        {"||", T_OR},       {"+=", T_PLUS_EQ},
                {"-=", T_MINUS_EQ}, {"*=", T_STAR_EQ},    {"/=", T_SLASH_EQ}, {"%=", T_PERCENT_EQ},
                {"++", T_PLUSPLUS}, {"--", T_MINUSMINUS}, {"+", T_PLUS},      {"-", T_MINUS},
                {"*", T_STAR},      {"/", T_SLASH},       {"%", T_PERCENT},   {"=", T_ASSIGN},
                {"<", T_LT},        {">", T_GT},          {"!", T_NOT},       {"&", T_AMP},
                {"(", T_LPAREN},    {")", T_RPAREN},      {"{", T_LBRACE},    {"}", T_RBRACE},
                {"[", T_LBRACKET},  {"]", T_RBRACKET},    {";", T_SEMI},      {",", T_COMMA},
                {".", T_DOT},       {"?", T_QUESTION},    {":", T_COLON},
            };
            int matched = 0;
            for (size_t k = 0; k < sizeof OPS / sizeof(*OPS) && !matched; k++) {
                int olen = (int)strlen(OPS[k].text);
                if (lx.pos + olen <= lx.len &&
                    memcmp(src + lx.pos, OPS[k].text, (size_t)olen) == 0) {
                    tk.kind = OPS[k].kind;
                    for (int i = 0; i < olen; i++)
                        bump(&lx);
                    matched = 1;
                }
            }
            if (!matched) {
                Span s = span_at(start, 1);
                s.line = sline;
                s.col = scol;
                diag_error(s, "unexpected character '%c'", c);
                bump(&lx);
                continue; /* drop the bad token */
            }
        }

        tk.span = span_at(start, lx.pos - start);
        tk.span.line = sline;
        tk.span.col = scol;
        toks[count++] = tk;
    }

    *out_count = count;
    return toks;
}
