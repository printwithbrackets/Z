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

/* Defined below, next to the rest of the cursor helpers. */
static Span span_at(int start, int len);
static int peek_char(Lexer *lx, int off);
static void bump(Lexer *lx);

/* The value of one hex digit, or -1 if `c` is not one. */
static int hex_val(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* The largest value Z's `int` can hold. A literal above this is rejected rather
 * than silently wrapping into a negative number. */
#define Z_INT_MAX 0x7FFFFFFFFFFFFFFFull

/* Decodes the escape sequence that follows a backslash, which the caller has
 * already consumed, appending the decoded bytes at `*len` in `dst` and stepping
 * the lexer past it.
 *
 * `in_interp` marks the raw text of an interpolated string, where a brace is
 * significant: a brace produced by *any* escape is written back as `\{` or `\}`
 * so the parser can still tell a literal brace from a hole delimiter. Every
 * other escape decodes identically in both contexts.
 *
 * An unrecognized escape is an error. The lexer used to pass the character
 * through, which meant "\q" silently meant "q" and a typo cost nothing but the
 * backslash. */
static void decode_escape(Lexer *lx, char *dst, int *len, int in_interp, Span s) {
    int e = peek_char(lx, 0);
    switch (e) {
    case 'n':
        dst[(*len)++] = '\n';
        bump(lx);
        return;
    case 't':
        dst[(*len)++] = '\t';
        bump(lx);
        return;
    case 'r':
        dst[(*len)++] = '\r';
        bump(lx);
        return;
    case '0':
        dst[(*len)++] = '\0';
        bump(lx);
        return;
    case '\\':
        dst[(*len)++] = '\\';
        bump(lx);
        return;
    case '"':
        dst[(*len)++] = '"';
        bump(lx);
        return;
    /* Braces are escapable only where they mean something, which is inside an
     * interpolated string. In a plain string they are ordinary characters and
     * "\{" is a mistake worth reporting. */
    case '{':
    case '}':
        if (!in_interp) {
            diag_error(s, "unknown escape '\\%c'", (char)e);
            dst[(*len)++] = (char)e;
            bump(lx);
            return;
        }
        dst[(*len)++] = '\\';
        dst[(*len)++] = (char)e;
        bump(lx);
        return;
    case 'x': {
        bump(lx);
        int v = 0, n = 0;
        for (;;) {
            int h = hex_val(peek_char(lx, 0));
            if (h < 0)
                break;
            v = v * 16 + h;
            n++;
            bump(lx);
        }
        if (n == 0) {
            diag_error(s, "\\x needs at least one hex digit");
            dst[(*len)++] = 'x';
            return;
        }
        if (n > 2)
            diag_error(s, "\\x takes at most two hex digits; write \\u%04x for a larger value", v);
        if (in_interp && (v == '{' || v == '}'))
            dst[(*len)++] = '\\';
        dst[(*len)++] = (char)v;
        return;
    }
    case 'u': {
        bump(lx);
        int v = 0, n = 0;
        for (;;) {
            int h = hex_val(peek_char(lx, 0));
            if (h < 0)
                break;
            v = v * 16 + h;
            n++;
            bump(lx);
        }
        if (n != 4) {
            diag_error(s, "\\u needs exactly four hex digits, got %d", n);
            dst[(*len)++] = 'u';
            return;
        }
        /* Surrogates and out-of-range values have no UTF-8 encoding; saying so
         * beats emitting bytes that no decoder will accept. */
        if ((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
            diag_error(s, "\\u%04x is not a Unicode code point", v);
            dst[(*len)++] = '?';
            return;
        }
        /* UTF-8. Only the one-byte case can be a brace, so only it needs the
         * interpolated-string escape. */
        if (v < 0x80) {
            if (in_interp && (v == '{' || v == '}'))
                dst[(*len)++] = '\\';
            dst[(*len)++] = (char)v;
        } else if (v < 0x800) {
            dst[(*len)++] = (char)(0xC0 | (v >> 6));
            dst[(*len)++] = (char)(0x80 | (v & 0x3F));
        } else {
            dst[(*len)++] = (char)(0xE0 | (v >> 12));
            dst[(*len)++] = (char)(0x80 | ((v >> 6) & 0x3F));
            dst[(*len)++] = (char)(0x80 | (v & 0x3F));
        }
        return;
    }
    default:
        if (e == -1) {
            diag_error(s, "unterminated string");
            return;
        }
        diag_error(s, "unknown escape '\\%c'", (char)e);
        dst[(*len)++] = (char)e;
        bump(lx);
        return;
    }
}

/* The file currently being lexed. Stamped onto every span so a diagnostic can
 * name the file the token actually came from, which matters once a compilation
 * unit spans several files through `import`. */
static const char *g_lex_file = NULL;

static Span span_at(int start, int len) {
    Span s;
    s.file = g_lex_file;
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
    {"int", T_KW_INT},         {"bool", T_KW_BOOL},        {"float", T_KW_FLOAT},
    {"string", T_KW_STRING},   {"void", T_KW_VOID},
    {"var", T_KW_VAR},         {"const", T_KW_CONST},
    {"new", T_KW_NEW},
    {"struct", T_KW_STRUCT},   {"enum", T_KW_ENUM},
    {"match", T_KW_MATCH},     {"this", T_KW_THIS},
    {"if", T_KW_IF},           {"else", T_KW_ELSE},
    {"while", T_KW_WHILE},     {"for", T_KW_FOR},
    {"foreach", T_KW_FOREACH}, {"in", T_KW_IN},
    {"return", T_KW_RETURN},   {"break", T_KW_BREAK},
    {"continue", T_KW_CONTINUE},
    {"true", T_KW_TRUE},      {"null", T_KW_NULL},      {"extern", T_KW_EXTERN},   {"export", T_KW_EXPORT},   {"fn", T_KW_FN},        {"closure", T_KW_CLOSURE},        {"method", T_KW_METHOD},        {"import", T_KW_IMPORT},    {"false", T_KW_FALSE},     {"class", T_KW_CLASS},{"interface", T_KW_INTERFACE},
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
Token *lex_all_file(Arena *arena, const char *src, int len, StringTable *strings, int *out_count,
                    const char *file) {
    g_lex_file = file;
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
            /* Radix prefixes, so a bit pattern can be written the way it is
             * printed: 0xff, 0o755, 0b1010_0110. Underscores are digit
             * separators and carry no meaning. */
            int base = 10;
            if (c == '0') {
                int n1 = peek_char(&lx, 1);
                if (n1 == 'x' || n1 == 'X') {
                    base = 16;
                    bump(&lx);
                    bump(&lx);
                } else if (n1 == 'o' || n1 == 'O') {
                    base = 8;
                    bump(&lx);
                    bump(&lx);
                } else if (n1 == 'b' || n1 == 'B') {
                    base = 2;
                    bump(&lx);
                    bump(&lx);
                }
            }
            unsigned long long val = 0;
            int overflow = 0, ndigits = 0;
            for (;;) {
                int d = peek_char(&lx, 0);
                if (d == '_') {
                    bump(&lx);
                    continue;
                }
                int dv = hex_val(d);
                if (dv < 0 || dv >= base)
                    break;
                /* Check before accumulating, so `val` can never wrap: once the
                 * value would pass what `int` holds we stop growing it and
                 * report at the end. */
                if (!overflow) {
                    if (val > (Z_INT_MAX - (unsigned)dv) / (unsigned)base)
                        overflow = 1;
                    else
                        val = val * (unsigned)base + (unsigned)dv;
                }
                ndigits++;
                bump(&lx);
            }
            Span nspan = span_at(start, lx.pos - start);
            nspan.line = sline;
            nspan.col = scol;

            /* A '.' begins a fraction only when a digit follows it. That is what
             * keeps `21.Twice()` working: there the '.' is followed by a letter
             * and belongs to the member access, not to a number. An `e` begins
             * an exponent, with or without a fraction, so `1e3` is a float too. */
            int is_float = 0;
            /* Seed the value with the integer part first, so a literal with an
             * exponent but no fraction -- `1e3` -- scales from the right number
             * instead of from zero. */
            tk.dval = (double)val;
            if (base == 10 && peek_char(&lx, 0) == '.' && is_digit(peek_char(&lx, 1))) {
                bump(&lx);
                double frac = 0, scale = 0.1;
                while (is_digit(peek_char(&lx, 0)) || peek_char(&lx, 0) == '_') {
                    if (peek_char(&lx, 0) != '_') {
                        frac += (peek_char(&lx, 0) - '0') * scale;
                        scale *= 0.1;
                    }
                    bump(&lx);
                }
                tk.dval = (double)val + frac;
                is_float = 1;
            }
            if (base == 10 && (peek_char(&lx, 0) == 'e' || peek_char(&lx, 0) == 'E')) {
                int esign = 1;
                bump(&lx);
                if (peek_char(&lx, 0) == '+') {
                    bump(&lx);
                } else if (peek_char(&lx, 0) == '-') {
                    esign = -1;
                    bump(&lx);
                }
                int e = 0, edigits = 0;
                while (is_digit(peek_char(&lx, 0)) || peek_char(&lx, 0) == '_') {
                    if (peek_char(&lx, 0) != '_' && e < 1000000)
                        e = e * 10 + (peek_char(&lx, 0) - '0');
                    edigits++;
                    bump(&lx);
                }
                if (edigits == 0) {
                    diag_error(nspan, "this exponent has no digits");
                } else {
                    /* Scaling by repeated multiplication rather than pow(), so the
                     * result does not depend on the host's libm. The exponent is
                     * clamped, which keeps `1e99999` finite instead of relying on
                     * whatever the host does with an overflow. */
                    int ae = e < 0 ? -e : e;
                    if (ae > 308)
                        ae = 308;
                    double p = 1.0;
                    for (int i = 0; i < ae; i++)
                        p *= 10.0;
                    tk.dval = (esign < 0) ? tk.dval / p : tk.dval * p;
                    is_float = 1;
                }
            }
            if (is_float) {
                /* Not a `break`: this branch sits directly in the lexing loop,
                 * so a break here would end the file rather than the number. */
                if (ndigits == 0)
                    diag_error(nspan, "this literal has no digits");
                /* A float literal is never an overflow error: 1e400 is a large
                 * number, not a mistake. */
                if (is_alnum(peek_char(&lx, 0)))
                    diag_error(nspan, "unexpected '%c' after a numeric literal",
                               peek_char(&lx, 0));
                tk.kind = T_F64;
            } else {
            if (ndigits == 0 && base != 10)
                diag_error(nspan, "this literal has no digits");
            if (overflow)
                diag_error(nspan, "this literal does not fit in 'int' (max %lld)", Z_INT_MAX);
            /* A digit run that runs into letters is a typo, not two tokens:
             * "123abc" and "0x1g" are both mistakes worth naming. */
            if (is_alnum(peek_char(&lx, 0)))
                diag_error(nspan, "unexpected '%c' after a numeric literal", peek_char(&lx, 0));
            tk.kind = T_INT;
            tk.ival = (long long)val;
            }
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
                    Span esc = span_at(start, lx.pos - start);
                    esc.line = sline;
                    esc.col = scol;
                    decode_escape(&lx, decoded, &dlen, 0, esc);
                    if (peek_char(&lx, 0) == -1)
                        break;
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
                    Span esc = span_at(start, lx.pos - start);
                    esc.line = sline;
                    esc.col = scol;
                    decode_escape(&lx, raw, &rlen, 1, esc);
                    if (peek_char(&lx, 0) == -1)
                        break;
                    continue;
                }
                if (d == '"' && bdepth == 0) {
                    bump(&lx);
                    break;
                }
                /* bdepth counts hole nesting so a quote inside {..} does not end
                 * the string. An escaped brace went through decode_escape above
                 * and is literal, so it must not move the count; and a `}` with
                 * no hole open is a literal brace too. Letting it decrement
                 * below zero is what used to make `$"a\{b}c"` run off the end
                 * of the string looking for a closing quote. */
                if (d == '{')
                    bdepth++;
                else if (d == '}' && bdepth > 0)
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
                {"=>", T_FATARROW}, {"->", T_ARROW}, {"==", T_EQ},         {"!=", T_NE},       {"<=", T_LE},
                {">=", T_GE},       {"&&", T_AND},        {"||", T_OR},       {"+=", T_PLUS_EQ},
                {"-=", T_MINUS_EQ}, {"*=", T_STAR_EQ},    {"/=", T_SLASH_EQ}, {"%=", T_PERCENT_EQ},
                {"++", T_PLUSPLUS}, {"--", T_MINUSMINUS}, {"+", T_PLUS},      {"-", T_MINUS},
                {"*", T_STAR},      {"/", T_SLASH},       {"%", T_PERCENT},   {"=", T_ASSIGN},
                {"<<=", T_SHL_EQ},  {">>=", T_SHR_EQ},  {"<<", T_SHL},     {">>", T_SHR},
                {"&=", T_AMP_EQ},   {"|=", T_PIPE_EQ},  {"^=", T_CARET_EQ},        {"<", T_LT},        {">", T_GT},
                {"!", T_NOT},       {"&", T_AMP},         {"|", T_PIPE},      {"^", T_CARET},
                {"~", T_TILDE},
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

Token *lex_all(Arena *arena, const char *src, int len, StringTable *strings, int *out_count) {
    Token *t = lex_all_file(arena, src, len, strings, out_count, NULL);
    g_lex_file = NULL;
    return t;
}
