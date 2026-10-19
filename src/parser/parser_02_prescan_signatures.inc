/* ---- function signature table (pre-scan) ---- */

static void add_sig_ex(Parser *p, const char *name, Type *ret, Type **ptypes, int nparams,
                       int is_ext, int is_extern);

static void add_sig(Parser *p, const char *name, Type *ret, Type **ptypes, int nparams,
                    int is_ext) {
    add_sig_ex(p, name, ret, ptypes, nparams, is_ext, 0);
}

static void add_sig_ex(Parser *p, const char *name, Type *ret, Type **ptypes, int nparams,
                       int is_ext, int is_extern) {
    if (p->nsigs == p->sig_cap) {
        int ncap = p->sig_cap == 0 ? 8 : p->sig_cap * 2;
        Sig *ns = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Sig));
        if (p->sigs != NULL)
            memcpy(ns, p->sigs, (size_t)p->nsigs * sizeof(Sig));
        p->sigs = ns;
        p->sig_cap = ncap;
    }
    Sig *s = &p->sigs[p->nsigs++];
    s->name = arena_strdup(p->arena, name);
    s->ret = ret;
    s->nparams = nparams;
    s->is_ext = is_ext;
    s->is_extern = is_extern;
    s->is_export = 0;
    s->ext_recv = (is_ext && nparams > 0) ? ptypes[0] : NULL;
    s->ptypes = NULL;
    if (nparams > 0) {
        s->ptypes = arena_alloc_array(p->arena, (size_t)nparams, sizeof(Type *));
        memcpy(s->ptypes, ptypes, (size_t)nparams * sizeof(Type *));
    }
}

static Sig *find_sig(Parser *p, const char *name) {
    for (int i = 0; i < p->nsigs; i++) {
        if (strcmp(p->sigs[i].name, name) == 0)
            return &p->sigs[i];
    }
    return NULL;
}

/* Renders a declared signature the way the reader wrote it, as a buffer in the
 * arena.
 *
 * "argument 2 of 'read' expects 'int' but got 'string'" states the violation.
 * It does not state the constraint, and a constraint is what tells you what to
 * do: the reader has to go and look the declaration up to find out what `read`
 * wanted and whether the 2 is even a parameter. Putting the declaration in the
 * message is the difference between reporting a problem and handing over the
 * thing needed to fix it. */
static char *sig_text(Parser *p, const char *name, const Sig *s) {
    size_t cap = 64;
    for (int i = 0; i < s->nparams; i++)
        cap += 32;
    char *buf = arena_alloc(p->arena, cap);
    size_t off = 0;
    if (s->ret != NULL)
        off += (size_t)snprintf(buf, cap, "%s ", type_name(p->ty, s->ret));
    off += (size_t)snprintf(buf + off, cap - off, "%s(", name);
    for (int i = 0; i < s->nparams && off < cap; i++) {
        if (i > 0)
            off += (size_t)snprintf(buf + off, cap - off, ", ");
        if (off < cap)
            off += (size_t)snprintf(buf + off, cap - off, "%s",
                                    s->ptypes[i] ? type_name(p->ty, s->ptypes[i]) : "?");
    }
    if (off < cap)
        snprintf(buf + off, cap - off, ")");
    return buf;
}

/* The small integer a Z program sees from `typeof`, one per TypeKind.
 *
 * These are the TypeKind values, in the enum's own order, and the standard
 * library's TYPE_* constants are these numbers. Exporting them is what lets a
 * generic function branch on a type without an interface every type would have
 * to satisfy -- and the enum is the only place the two can be kept in step, so
 * the ordering is load-bearing rather than incidental. A new kind appends, and
 * the constant follows. */
int type_kind_code(Type *t) {
    switch (t->kind) {
    case TK_VOID:
        return 0;
    case TK_INT:
        return 1;
    case TK_BOOL:
        return 2;
    case TK_F64:
        return 3;
    case TK_STRING:
        return 4;
    case TK_PTR:
        return 5;
    case TK_ARRAY:
        return 6;
    case TK_STRUCT:
        return 7;
    case TK_UNION:
        return 8;
    case TK_FNPTR:
        return 9;
    case TK_MPTR:
        return 10;
    case TK_CLOSURE:
        return 11;
    case TK_IFACE:
        return 12;
    case TK_TYPEPARAM:
        return 13;
    /* `any` never has a static type at a call site -- it is only ever what a
     * built-in's parameter is *declared* as -- so this cannot be reached from Z.
     * It is reported as void rather than as a code of its own, because a code
     * the standard library does not name would be worse than a wrong-but-total
     * answer. */
    case TK_ANY:
        return 0;
    }
    return 0;
}

/* Given a token index that begins a type, returns the index just past it. */
static int skip_type_tokens(Parser *p, int i);

static Generic *find_generic(Parser *p, const char *name);

/* The lexical namespace prefix for definitions: "" outside any block. */
static const char *ns_here(Parser *p) { return p->ns_prefix != NULL ? p->ns_prefix : ""; }

/* "prefix" + "." + "name", or just "name" at the top level. */
static char *ns_join(Parser *p, const char *prefix, const char *name) {
    if (prefix == NULL || prefix[0] == '\0')
        return arena_strdup(p->arena, name);
    size_t n = strlen(prefix) + 1 + strlen(name);
    char *out = arena_alloc(p->arena, n + 1);
    snprintf(out, n + 1, "%s.%s", prefix, name);
    return out;
}

/* Register a namespace path, once however many blocks or aliases declare it:
 * one `namespace netlink` in two files is one namespace, which is what lets a
 * namespace span files. */
static void ns_declare_one(Parser *p, const char *path) {
    for (int i = 0; i < p->nns; i++)
        if (strcmp(p->ns_paths[i], path) == 0)
            return;
    if (p->nns == p->ns_cap) {
        int ncap = p->ns_cap == 0 ? 8 : p->ns_cap * 2;
        char **nb = arena_alloc_array(p->arena, (size_t)ncap, sizeof(char *));
        if (p->nns > 0)
            memcpy(nb, p->ns_paths, (size_t)p->nns * sizeof(char *));
        p->ns_paths = nb;
        p->ns_cap = ncap;
    }
    p->ns_paths[p->nns++] = arena_strdup(p->arena, path);
}

/* Register a namespace path, once however many blocks or aliases declare it.
 * Every intermediate prefix registers too, so `namespace absolute.path`
 * makes both `absolute` and `absolute.path` known and a use site can scan
 * past either. An intermediate holds no members itself; resolving through one
 * simply reports the missing member. */
static void ns_declare(Parser *p, const char *path) {
    char prefix[256];
    size_t n = 0;
    for (const char *s = path;; s++) {
        if (*s == '.' || *s == '\0') {
            if (n > 0 && n < sizeof prefix) {
                prefix[n] = '\0';
                ns_declare_one(p, prefix);
            }
            if (*s == '\0')
                break;
            if (n + 1 < sizeof prefix)
                prefix[n++] = '.';
            else
                break;
        } else {
            if (n + 1 < sizeof prefix)
                prefix[n++] = *s;
            else
                break;
        }
    }
}

static int ns_known(Parser *p, const char *path) {
    for (int i = 0; i < p->nns; i++)
        if (strcmp(p->ns_paths[i], path) == 0)
            return 1;
    return 0;
}

/* Unqualified `name` fell through; retry as prefix.name, walking outward.
 * `look` is one of the plain table lookups below. */
/* Unqualified `name` fell through; retry as prefix.name, walking outward.
 * `look` is one of the plain table lookups below. */
static void *ns_lookup(Parser *p, const char *name, void *(*look)(Parser *, const char *)) {
    void *r = look(p, name);
    if (r != NULL)
        return r;
    const char *pfx = ns_here(p);
    if (pfx[0] == '\0')
        return NULL;
    char cand[512];
    size_t plen = strlen(pfx);
    size_t nlen = strlen(name);
    const char *end = pfx + plen;
    for (;;) {
        size_t pl = (size_t)(end - pfx);
        if (pl + 1 + nlen >= sizeof cand)
            return NULL;
        memcpy(cand, pfx, pl);
        cand[pl] = '.';
        memcpy(cand + pl + 1, name, nlen + 1);
        r = look(p, cand);
        if (r != NULL)
            return r;
        if (pl == 0)
            return NULL;
        const char *dot = NULL;
        for (size_t k = 0; k < pl; k++)
            if (pfx[k] == '.')
                dot = pfx + k;
        if (dot == NULL)
            return NULL;
        end = dot;
    }
}

/* The internal name `name` denotes: itself when it resolves directly, the
 * prefixed candidate when only a namespace fallback hits, NULL when neither
 * does. Lets call sites and value positions share one fallback walk. */
static char *ns_resolve(Parser *p, const char *name, void *(*look)(Parser *, const char *)) {
    if (look(p, name) != NULL)
        return (char *)name;
    const char *pfx = ns_here(p);
    if (pfx[0] == '\0')
        return NULL;
    char cand[512];
    size_t nlen = strlen(name);
    const char *end = pfx + strlen(pfx);
    for (;;) {
        size_t pl = (size_t)(end - pfx);
        if (pl + 1 + nlen >= sizeof cand)
            return NULL;
        memcpy(cand, pfx, pl);
        cand[pl] = '.';
        memcpy(cand + pl + 1, name, nlen + 1);
        if (look(p, cand) != NULL)
            return arena_strdup(p->arena, cand);
        if (pl == 0)
            return NULL;
        const char *dot = NULL;
        for (size_t k = 0; k < pl; k++)
            if (pfx[k] == '.')
                dot = pfx + k;
        if (dot == NULL)
            return NULL;
        end = dot;
    }
}

static void *look_sig(Parser *p, const char *n) { return find_sig(p, n); }

static void *look_generic(Parser *p, const char *n) { return find_generic(p, n); }

static void *look_gtype(Parser *p, const char *n) { return find_gtype(p, n); }

static void *look_const(Parser *p, const char *n) { return const_find(p, n); }

static void *look_global(Parser *p, const char *n) { return find_global(p, n); }

static void *look_struct(Parser *p, const char *n) { return type_find_struct(p->ty, n); }

static void *look_iface(Parser *p, const char *n) { return type_find_iface(p->ty, n); }

static void *look_union(Parser *p, const char *n) { return type_find_union(p->ty, n); }

/* If `i` points at `fn`, returns the index just past the whole function type
 * `fn(params) -> ret`, including any `*`/`[]` on the return type. Falls back to
 * just past the keyword if the type is malformed, so a syntax error is reported
 * by the real parser rather than here. */
static int skip_fn_type(Parser *p, int i) {
    i++; /* fn */
    if (i >= p->ntoks || p->toks[i].kind != T_LPAREN)
        return i;
    int depth = 0;
    for (; i < p->ntoks; i++) {
        if (p->toks[i].kind == T_LPAREN)
            depth++;
        else if (p->toks[i].kind == T_RPAREN) {
            depth--;
            if (depth == 0) {
                i++;
                break;
            }
        }
    }
    if (i < p->ntoks && p->toks[i].kind == T_ARROW)
        i++;
    /* The return type may itself be a function type. */
    if (i < p->ntoks && (p->toks[i].kind == T_KW_FN || p->toks[i].kind == T_KW_METHOD ||
                         p->toks[i].kind == T_KW_CLOSURE))
        return skip_fn_type(p, i);
    return skip_type_tokens(p, i);
}

static int skip_type_tokens(Parser *p, int i) {
    if (i >= p->ntoks)
        return i;
    if (p->toks[i].kind == T_KW_FN || p->toks[i].kind == T_KW_METHOD ||
        p->toks[i].kind == T_KW_CLOSURE)
        return skip_fn_type(p, i);
    if (!is_type_token(p->toks[i].kind) && p->toks[i].kind != T_IDENT)
        return i;
    /* A namespace-qualified name: `netlink.Lobby`. Segments are consumed only
     * while the accumulated path is a declared namespace, so `a.b` over values
     * keeps its old meaning everywhere this lookahead runs. */
    if (p->toks[i].kind == T_IDENT) {
        char path[256];
        if (ns_resolve_head(p, i, path, sizeof path)) {
            size_t n = strlen(path);
            int j = i;
            while (ns_known(p, path) && j + 2 < p->ntoks && p->toks[j + 1].kind == T_DOT &&
                   p->toks[j + 2].kind == T_IDENT) {
                size_t tl = strlen(p->toks[j + 2].text);
                if (n + 1 + tl >= sizeof path)
                    break;
                path[n++] = '.';
                memcpy(path + n, p->toks[j + 2].text, tl + 1);
                n += tl;
                j += 2;
            }
            if (j != i) {
                /* A generic template named by its qualified path: `N.Vec<int>`. */
                if (j + 1 < p->ntoks && p->toks[j + 1].kind == T_LT &&
                    find_gtype(p, path) != NULL) {
                    int depth = 0;
                    int k = j + 1;
                    for (; k < p->ntoks; k++) {
                        TokenKind kk = p->toks[k].kind;
                        if (kk == T_LT) {
                            depth++;
                        } else if (kk == T_GT) {
                            if (--depth == 0) {
                                k++;
                                break;
                            }
                        } else if (kk == T_SHR) {
                            depth -= 2;
                            k++;
                            if (depth <= 0)
                                break;
                        } else if (kk == T_EOF) {
                            break;
                        }
                    }
                    i = k;
                } else {
                    i = j + 1; /* past the member name, like the single-name path */
                }
                for (;;) {
                    if (i < p->ntoks && p->toks[i].kind == T_STAR) {
                        i++;
                    } else if (i + 1 < p->ntoks && p->toks[i].kind == T_LBRACKET &&
                               p->toks[i + 1].kind == T_RBRACKET) {
                        i += 2;
                    } else {
                        break;
                    }
                }
                return i;
            }
        }
    }
    /* A generic type applied to its arguments: `Vec<T>`, `Map<K,V>`. Every
     * caller here is deciding where a type ends, so the argument list has to be
     * part of it -- otherwise `Vec<T> reversed()` looked like a bare `Vec`
     * followed by a `<` that was not a member name, and the whole signature
     * pre-scan stopped there. In a generic class that is fatal rather than
     * merely annoying: Vec's own methods return `Vec<T>`, so the first one ended
     * the pass and every method after it went unregistered -- which is why a
     * `Vec` had no `Grow` even though `Grow` was written in the same class.
     *
     * `>>` is one token to the lexer, so a nested `Map<string,Vec<int>>` closes
     * two lists with one of them and the depth has to count it as two. */
    if (p->toks[i].kind == T_IDENT && i + 1 < p->ntoks && p->toks[i + 1].kind == T_LT &&
        (find_gtype(p, p->toks[i].text) != NULL ||
         ns_lookup(p, p->toks[i].text, look_gtype) != NULL)) {
        int depth = 0;
        for (; i < p->ntoks; i++) {
            TokenKind k = p->toks[i].kind;
            if (k == T_LT) {
                depth++;
            } else if (k == T_GT) {
                if (--depth == 0) {
                    i++;
                    break;
                }
            } else if (k == T_SHR) {
                depth -= 2;
                i++;
                if (depth <= 0)
                    break;
            } else if (k == T_EOF) {
                break;
            }
        }
    } else if (at_result_type(p, i)) {
        /* `Result<T,E>` carries its own type arguments, so step over the whole
         * thing. Skipping only the name would leave the `<` behind, and every
         * caller here is deciding where a type ends. The angle brackets nest, and
         * the lexer makes `>>` a single token, so `Result<Result<int,string>,int>`
         * needs the `>>` to count as two closers. */
        int depth = 0;
        for (; i < p->ntoks; i++) {
            TokenKind k = p->toks[i].kind;
            if (k == T_LT) {
                depth++;
            } else if (k == T_GT) {
                if (--depth == 0) {
                    i++;
                    break;
                }
            } else if (k == T_SHR) {
                depth -= 2;
                i++;
                if (depth <= 0)
                    break;
            } else if (k == T_EOF) {
                break;
            }
        }
    } else {
        i++;
    }
    for (;;) {
        if (i < p->ntoks && p->toks[i].kind == T_STAR) {
            i++;
        } else if (i + 1 < p->ntoks && p->toks[i].kind == T_LBRACKET &&
                   p->toks[i + 1].kind == T_RBRACKET) {
            i += 2;
        } else {
            break;
        }
    }
    return i;
}

/* If `i` points at '<', returns the index just after the matching '>'. */
static int skip_generic_params(Parser *p, int i) {
    if (i >= p->ntoks || p->toks[i].kind != T_LT)
        return i;
    int depth = 0;
    while (i < p->ntoks) {
        if (p->toks[i].kind == T_LT)
            depth++;
        else if (p->toks[i].kind == T_GT) {
            depth--;
            if (depth == 0)
                return i + 1;
        }
        i++;
    }
    return i;
}

/* A top-level function starts with a (possibly pointer/array) type, a name,
 * and '('. A generic function is `name<T,...>(`. Uses absolute token indices. */
static int at_func_decl(Parser *p) {
    int j = p->pos;
    if (j < p->ntoks && (p->toks[j].kind == T_KW_EXTERN || p->toks[j].kind == T_KW_EXPORT))
        j++; /* `extern int f(...);` / `export int f(...) { }` / `extern fn f(...)` */
    /* Rust-style `fn name<T..>(params)`. Only when `fn` is followed by a
     * name: a C-style function returning a function-pointer type also leads
     * with `fn`, as in `fn(int) -> int makeAdder(int n)`, and that shape falls
     * through to the type-skipping path below. */
    if (j < p->ntoks && p->toks[j].kind == T_KW_FN) {
        int k = j + 1;
        if (k < p->ntoks && p->toks[k].kind == T_IDENT) {
            k++;
            if (k < p->ntoks && p->toks[k].kind == T_LT) {
                if (k + 1 >= p->ntoks || p->toks[k + 1].kind != T_IDENT)
                    return 0;
                k = skip_generic_params(p, k);
            }
            if (k < p->ntoks && p->toks[k].kind == T_LPAREN)
                return 1;
        }
    }
    j = skip_type_tokens(p, j);
    if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
        return 0;
    j++;
    if (j < p->ntoks && p->toks[j].kind == T_LT) {
        /* Require `<IDENT` so a comparison `a < b` is not mistaken for a
         * generic parameter list. */
        if (j + 1 >= p->ntoks || p->toks[j + 1].kind != T_IDENT)
            return 0;
        j = skip_generic_params(p, j);
    }
    return j < p->ntoks && p->toks[j].kind == T_LPAREN;
}

/* An explicitly typed local declaration at the start of a statement:
 * `Type name`, where the type may be a keyword, a struct/union/enum name, a type
 * parameter or a function type, followed by any number of `*` and `[]`.
 *
 * This is the same shape at_func_decl recognizes, and the difference is the token
 * after the name: `(` makes it a nested function, anything else a declaration.
 * Requiring the name to be an identifier is what keeps an ordinary expression
 * statement out -- `foo();` and `foo.bar();` have `(` or `.` there, never a
 * second identifier. */
static int at_typed_decl(Parser *p) {
    int j = p->pos;
    if (j >= p->ntoks)
        return 0;
    if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
        return 0;
    j = skip_type_tokens(p, j);
    return j < p->ntoks && p->toks[j].kind == T_IDENT;
}

/* Registers every top-level `struct Name` so the signature pre-scan can
 * resolve struct-typed parameters (declarations are parsed later). */
/* Registers a generic class/struct template, or returns the one already
 * registered under this name. */
static GenericType *find_gtype(Parser *p, const char *name) {
    for (int i = 0; i < p->ngtypes; i++)
        if (strcmp(p->gtypes[i].name, name) == 0)
            return &p->gtypes[i];
    return NULL;
}

static GenericType *add_gtype(Parser *p, const char *name, int is_class) {
    if (p->ngtypes == p->gtype_cap) {
        int ncap = p->gtype_cap == 0 ? 4 : p->gtype_cap * 2;
        GenericType *ng = arena_alloc_array(p->arena, (size_t)ncap, sizeof(GenericType));
        if (p->gtypes != NULL)
            memcpy(ng, p->gtypes, (size_t)p->ngtypes * sizeof(GenericType));
        p->gtypes = ng;
        p->gtype_cap = ncap;
    }
    GenericType *g = &p->gtypes[p->ngtypes++];
    memset(g, 0, sizeof *g);
    g->name = arena_strdup(p->arena, name);
    g->is_class = is_class;
    return g;
}

/* Index just past a `<...>` type-parameter list starting at `i`, or `i` if
 * there is not one. Reads bare identifiers separated by commas, which is the
 * whole grammar: a bound on a type parameter would need something to bound
 * against, and monomorphization checks the argument where it is resolved. */
/* Rewrites a `>>` at the cursor into two `>` tokens, if that is what is there.
 *
 * The lexer is not told about nesting, so a nested generic's closing brackets
 * arrive as one shift token: `Vec<Vec<int>>` is `>>`. Any pass that walks a type
 * has to undo that, and the undo is a splice into the token array -- which is
 * safe here because lexing is finished: nothing will re-read the source, and a
 * token's Span is copied by value, so a split keeps both halves pointing at the
 * same two source bytes. The array is replaced rather than grown in place,
 * because an arena block cannot be resized. */
static void split_shift_closer(Parser *p) {
    if (!at(p, T_SHR))
        return;
    Token *bigger = arena_alloc_array(p->arena, (size_t)p->ntoks + 1, sizeof(Token));
    for (int i = 0; i < p->pos; i++)
        bigger[i] = p->toks[i];
    /* The first half keeps the token's kind and position; the second gets the
     * same span minus the byte the shift's second character covered. */
    bigger[p->pos].kind = T_GT;
    bigger[p->pos + 1] = p->toks[p->pos];
    bigger[p->pos + 1].kind = T_GT;
    if (bigger[p->pos + 1].span.len > 1)
        bigger[p->pos + 1].span.len = 1;
    for (int i = p->pos + 1; i < p->ntoks; i++)
        bigger[i + 1] = p->toks[i];
    p->toks = bigger;
    p->ntoks++;
}

static int skip_type_param_list(Parser *p, int i) {
    if (i >= p->ntoks || p->toks[i].kind != T_LT)
        return i;
    int j = i + 1, depth = 1;
    while (j < p->ntoks && depth > 0) {
        if (p->toks[j].kind == T_LT)
            depth++;
        else if (p->toks[j].kind == T_GT)
            depth--;
        else if (p->toks[j].kind == T_EOF)
            return i;
        if (depth == 0)
            break;
        j++;
    }
    return depth == 0 ? j + 1 : i;
}

/* Index of the `}` closing the body that starts at `i`, or -1. Counts braces
 * so a nested class, an enum or a method body does not end it early. */
static int find_body_end(Parser *p, int i) {
    int depth = 0;
    for (int j = i; j < p->ntoks; j++) {
        if (p->toks[j].kind == T_LBRACE)
            depth++;
        else if (p->toks[j].kind == T_RBRACE) {
            depth--;
            if (depth == 0)
                return j + 1;
        } else if (p->toks[j].kind == T_EOF) {
            return -1;
        }
    }
    return -1;
}

/* Steps over a `class Name<...> { ... }` declaration, leaving the cursor just
 * past its closing brace. The registered template already knows where the body
 * ends, so this is a lookup rather than a brace count. */
static int skip_generic_type_decl(Parser *p) {
    /* Namespace-aware: `class Vec<T>` inside `namespace n` is registered as
     * `n.Vec`, so the bare name misses. Falling back here matters more than
     * anywhere: returning `p->pos` unconsumed parks the top-level loop on the
     * same declaration forever. */
    GenericType *g = find_gtype(p, peek(p, 1)->text);
    if (g == NULL)
        g = ns_lookup(p, peek(p, 1)->text, look_gtype);
    if (g == NULL)
        return p->pos;
    return g->tok_end < p->ntoks ? g->tok_end : p->pos;
}

/* Lexical namespace sweep shared by every pre-scan. Each pre-scan walks the
 * token stream forward exactly once; feeding positions through NsSweep tracks
 * `namespace` blocks (brace-matched, like the expander) so the sweep always
 * knows the path enclosing the current token and whether that token sits
 * directly in a member body (a top-level declaration when the stack is empty
 * and the depth is zero).
 *
 * Single-identifier blocks nest relative to the enclosing block; a dotted
 * header is absolute. Registration is idempotent, so every sweep declaring
 * the same paths is harmless -- and `import "f.z" as N` arrives as
 * synthesized `namespace N {` tokens, which is why aliases need no code of
 * their own anywhere in the parser. */
typedef struct {
    int depth;
    int nst;
    char paths[16][256];
    int levels[16];
} NsSweep;

static void ns_sweep_init(NsSweep *s) {
    s->depth = 0;
    s->nst = 0;
}

static void ns_sweep_step(NsSweep *s, Parser *p, int i) {
    TokenKind k = p->toks[i].kind;
    if (k == T_LBRACE) {
        s->depth++;
        return;
    }
    if (k == T_RBRACE) {
        if (s->depth > 0)
            s->depth--;
        while (s->nst > 0 && s->levels[s->nst - 1] > s->depth)
            s->nst--;
        return;
    }
    if (k != T_KW_NAMESPACE)
        return;
    int j = i + 1;
    if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
        return;
    char name[256];
    size_t n = 0;
    int dotted = 0;
    int jj = j;
    while (jj < p->ntoks && p->toks[jj].kind == T_IDENT) {
        const char *t = p->toks[jj].text;
        if (n > 0)
            dotted = 1;
        size_t tl = strlen(t);
        if (n + tl + 1 >= sizeof name)
            return;
        if (n > 0)
            name[n++] = '.';
        memcpy(name + n, t, tl);
        n += tl;
        if (jj + 2 < p->ntoks && p->toks[jj + 1].kind == T_DOT && p->toks[jj + 2].kind == T_IDENT) {
            jj += 2;
            continue;
        }
        break;
    }
    if (n == 0)
        return;
    name[n] = '\0';
    jj++;
    if (jj >= p->ntoks || p->toks[jj].kind != T_LBRACE)
        return;
    char full[256];
    if (dotted || s->nst == 0) {
        if (n >= sizeof full)
            return;
        memcpy(full, name, n + 1);
    } else {
        int need = snprintf(full, sizeof full, "%s.%s", s->paths[s->nst - 1], name);
        if (need < 0 || (size_t)need >= sizeof full)
            return;
    }
    ns_declare(p, full);
    if (s->nst < 16) {
        size_t fl = strlen(full);
        memcpy(s->paths[s->nst], full, fl + 1);
        s->levels[s->nst] = s->depth + 1;
        s->nst++;
    }
}

/* Prefix enclosing position i: "" when global. Call after stepping past i. */
static const char *ns_sweep_here(NsSweep *s) { return s->nst > 0 ? s->paths[s->nst - 1] : ""; }

/* Nonzero when a declaration starting at the just-stepped position is a
 * member: global at depth zero, or directly in the innermost open block. */
static int ns_sweep_member(NsSweep *s) {
    if (s->nst == 0)
        return s->depth == 0;
    return s->depth == s->levels[s->nst - 1];
}

/* The one lexical pre-pass everything namespace-related depends on. Runs
 * before every other pre-scan so use sites may precede definitions. With the
 * sweep doing the tracking, this pass is only registration. */
static void prescan_namespaces(Parser *p) {
    NsSweep s;
    ns_sweep_init(&s);
    for (int i = 0; i < p->ntoks; i++)
        ns_sweep_step(&s, p, i);
}

static void prescan_struct_names(Parser *p) {
    NsSweep ns;
    ns_sweep_init(&ns);
    int si = 0;
    for (int i = 0; i + 1 < p->ntoks; i++) {
        while (si < i)
            ns_sweep_step(&ns, p, si++);
        ns_sweep_step(&ns, p, i);
        si = i + 1;
        const char *pfx = ns_sweep_here(&ns);
        p->ns_prefix = (char *)pfx;
        if ((p->toks[i].kind == T_KW_STRUCT || p->toks[i].kind == T_KW_CLASS) &&
            p->toks[i + 1].kind == T_IDENT) {
            /* `class Vec<T>` is a template, not a type: registering `Vec` as a
             * struct would make the bare name resolve to a struct with no
             * fields, and the declaration would then be re-parsed as a second
             * definition of it. The name is only ever written as `Vec<...>`, so
             * it never needs to resolve on its own. */
            int after_params = skip_type_param_list(p, i + 2);
            if (after_params != i + 2) {
                char *gtname = ns_join(p, pfx, p->toks[i + 1].text);
                if (find_gtype(p, gtname) == NULL) {
                    GenericType *g = add_gtype(p, gtname, p->toks[i].kind == T_KW_CLASS);
                    g->src_name = arena_strdup(p->arena, p->toks[i + 1].text);
                    g->ns_prefix = arena_strdup(p->arena, pfx);
                    g->tok_start = i;
                    int lb = after_params;
                    while (lb < p->ntoks && p->toks[lb].kind != T_LBRACE) {
                        if (p->toks[lb].kind == T_EOF || p->toks[lb].kind == T_SEMI)
                            break;
                        lb++;
                    }
                    int end = lb < p->ntoks ? find_body_end(p, lb) : -1;
                    g->tok_end = end > 0 ? end : p->ntoks;
                    int cap = 0;
                    for (int j = i + 2; j < after_params; j++) {
                        if (p->toks[j].kind == T_IDENT) {
                            if (cap >= g->ntparams) {
                                int ncap = g->ntparams + 4;
                                char **nt =
                                    arena_alloc_array(p->arena, (size_t)ncap, sizeof(char *));
                                if (g->tparams != NULL)
                                    memcpy(nt, g->tparams, (size_t)g->ntparams * sizeof(char *));
                                g->tparams = nt;
                            }
                            g->tparams[g->ntparams++] = p->toks[j].text;
                            cap = g->ntparams;
                        }
                    }
                }
                i = after_params - 1;
                continue;
            }
            StructDef *sd = type_define_struct(p->ty, ns_join(p, pfx, p->toks[i + 1].text));
            if (sd != NULL && p->toks[i].kind == T_KW_CLASS)
                sd->is_class = 1;
            i += 1;
        }
        if (p->toks[i].kind == T_KW_ENUM && p->toks[i + 1].kind == T_IDENT) {
            type_define_union(p->ty, ns_join(p, pfx, p->toks[i + 1].text));
            i += 1;
        }
    }
    p->ns_prefix = NULL;
}

/* Registers the *signature* of a method without its body, for the pre-scan.
 *
 * Field layout alone was not enough to let a type be used before it is declared:
 * `Sq` has to be seen to have the methods an interface requires, and a struct
 * declared below the function that converts it had none registered yet. The body
 * is left to the real parser, which finds this entry by name and fills it in
 * rather than adding a second one -- so the method is emitted once.
 *
 * Returns the index just past the signature, or `i` unchanged if this is not a
 * method. Recognised shapes: `[virtual|override] type name ( params )`, and a
 * property `type name {`, which is a zero-argument method of the field's type. */
static int scan_method_signature(Parser *p, int i, StructDef *sd, const char *ctor_name) {
    int j = i;
    int is_virtual = 0;
    if (j < p->ntoks && p->toks[j].kind == T_KW_VIRTUAL) {
        is_virtual = 1;
        j++;
    } else if (j < p->ntoks && p->toks[j].kind == T_KW_OVERRIDE) {
        /* `override` went with inheritance. The pre-scan steps over it so both
         * passes agree on where the signature starts; the diagnostic belongs to
         * the real parse, which is the one with a span to point at. */
        is_virtual = 1;
        j++;
    }
    /* Rust-style `fn Name(params) [-> Ret]`. */
    if (j < p->ntoks && p->toks[j].kind == T_KW_FN) {
        if (j + 1 >= p->ntoks || p->toks[j + 1].kind != T_IDENT)
            return i;
        char *fname = p->toks[j + 1].text;
        int k = j + 2;
        if (k < p->ntoks && p->toks[k].kind == T_IDENT && ctor_name != NULL &&
            strcmp(p->toks[k].text, ctor_name) == 0) {
            /* `fn C(...)` constructor spelling: no signature to register. */
        }
        if (k >= p->ntoks || p->toks[k].kind != T_LPAREN)
            return i;
        /* Count params for the shallow signature. */
        int depth = 1;
        int nparams = 0;
        int expect_type = 1;
        int q = k + 1;
        for (; q < p->ntoks && depth > 0; q++) {
            TokenKind kk = p->toks[q].kind;
            if (kk == T_LPAREN)
                depth++;
            else if (kk == T_RPAREN)
                depth--;
            else if (kk == T_COMMA && depth == 1)
                expect_type = 1;
            else if (depth == 1 && expect_type &&
                     (is_type_token(kk) || kk == T_IDENT || kk == T_KW_MUT)) {
                if (kk == T_KW_MUT)
                    continue;
                expect_type = 0;
                nparams++;
            }
        }
        if (depth != 0)
            return i;
        int after = q;
        Type *rt = type_void(p->ty);
        if (after < p->ntoks && p->toks[after].kind == T_ARROW) {
            Parser sub2 = *p;
            sub2.pos = after + 1;
            Type *prt = parse_type(&sub2);
            if (prt != NULL) {
                rt = prt;
                after = sub2.pos;
            }
        }
        if (strcmp(fname, ctor_name ? ctor_name : "") == 0)
            return after;
        if (find_method(p, sd, fname) != NULL)
            return after;
        /* Parse param types for real via sub-parser. */
        Parser sub = *p;
        sub.pos = k + 1;
        Type **ptypes =
            arena_alloc_array(p->arena, (size_t)(nparams > 0 ? nparams : 1), sizeof(Type *));
        int np = 0;
        while (sub.pos < p->ntoks && p->toks[sub.pos].kind != T_RPAREN &&
               p->toks[sub.pos].kind != T_EOF) {
            if (p->toks[sub.pos].kind == T_KW_MUT) {
                sub.pos++;
                continue;
            }
            if (np >= nparams) {
                sub.pos++;
                continue;
            }
            Type *pt = parse_type(&sub);
            if (pt == NULL)
                return i;
            ptypes[np++] = pt;
            while (sub.pos < p->ntoks && p->toks[sub.pos].kind != T_COMMA &&
                   p->toks[sub.pos].kind != T_RPAREN && p->toks[sub.pos].kind != T_EOF)
                sub.pos++;
            if (sub.pos < p->ntoks && p->toks[sub.pos].kind == T_COMMA)
                sub.pos++;
        }
        StructMethod *m = arena_alloc(p->arena, sizeof *m);
        m->name = arena_strdup(p->arena, fname);
        m->ret = rt;
        m->ptypes = ptypes;
        m->nparams = np;
        m->body = NULL;
        m->is_virtual = is_virtual;
        m->vtable_index = -1;
        struct_add_method(p->ty, sd, m);
        return after;
    }
    int tstart = j;
    if (j >= p->ntoks)
        return i;
    if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
        return i;
    /* A constructor is named after the class: `C(params)`. It has no return
     * type, so the name is where the type would be. */
    int is_ctor = 0;
    char *mname;
    if (j < p->ntoks && p->toks[j].kind == T_IDENT && ctor_name != NULL &&
        strcmp(p->toks[j].text, ctor_name) == 0 && j + 1 < p->ntoks &&
        p->toks[j + 1].kind == T_LPAREN) {
        is_ctor = 1;
        mname = p->toks[j].text;
    } else {
        j = skip_type_tokens(p, j);
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            return i;
        mname = p->toks[j].text;
    }
    /* Step over the name in both cases, so a constructor's parameter list is
     * reached the same way an ordinary method's is. Leaving the cursor on the
     * name meant the caller's body-skipping started at the `(`, saw no `{`, and
     * stopped the pass -- so every member written after a constructor went
     * unregistered and a constructor could not call any of them. */
    j++;
    if (j >= p->ntoks)
        return i;

    /* A property: `type Name { ... }` reads as a zero-argument method. */
    if (!is_ctor && j < p->ntoks && p->toks[j].kind == T_LBRACE) {
        if (find_method(p, sd, mname) != NULL)
            return i;
        Parser sub = *p;
        sub.pos = tstart;
        Type *rt = parse_type(&sub);
        if (rt == NULL)
            return i;
        StructMethod *m = arena_alloc(p->arena, sizeof *m);
        m->name = arena_strdup(p->arena, mname);
        m->ret = rt;
        m->ptypes = NULL;
        m->nparams = 0;
        m->body = NULL; /* the real parser fills this in */
        m->is_virtual = 0;
        m->vtable_index = -1;
        struct_add_method(p->ty, sd, m);
        return j; /* the '{' and everything in it is the real parser's */
    }

    if (p->toks[j].kind != T_LPAREN)
        return i;
    /* Scan the parameter list for its shape only; the types come from the real
     * parse, and a signature here is enough to satisfy an interface. */
    int depth = 1;
    int nparams = 0;
    int expect_type = 1;
    for (j++; j < p->ntoks && depth > 0; j++) {
        TokenKind k = p->toks[j].kind;
        if (k == T_LPAREN) {
            depth++;
        } else if (k == T_RPAREN) {
            depth--;
        } else if (k == T_COMMA && depth == 1) {
            expect_type = 1;
        } else if (depth == 1 && expect_type && (is_type_token(k) || k == T_IDENT)) {
            expect_type = 0;
            nparams++;
        }
    }
    if (depth != 0)
        return i;
    /* Two cases where nothing is registered, and both still have to *advance*:
     *
     *   - a constructor is not part of an interface's surface, so it is not
     *     registered here; the real parser gives it its body;
     *   - a signature this pass has already recorded.
     *
     * Returning the unchanged index for either looks like "this is not a member",
     * and the caller stops the pass. So a struct whose first member was a
     * constructor had no later method registered at all, and a method body could
     * not call a method written below it -- the failure was invisible until
     * something needed a sibling call, and then the "undefined method" pointed at
     * a method that was right there in the file. */
    if (is_ctor || find_method(p, sd, mname) != NULL)
        return j;
    /* The types come from the real parser, not from a token scan.
     *
     * The scan this replaces walked the tokens looking for a type followed by a
     * name, and on `void Grow(int want)` it matched `void` with `Grow` -- the
     * return type paired with the method's *own* name -- so the recorded
     * signature was `(void)`. It went unnoticed because parse_method overwrites
     * the entry with the real one when it reads the body, so only a call site
     * written *above* the declaration saw the wrong types. In a generic class
     * that is every method, because the instance has no pass over the whole unit
     * and `push` calling the `Grow` declared below it is checked against this.
     * It also fed the interface-satisfaction check, which compared real
     * signatures against these.
     *
     * A shallow copy of the parser with only the cursor moved parses the same
     * signature without disturbing the caller. */
    Parser sub = *p;
    sub.pos = tstart;
    Type *rt = parse_type(&sub);
    if (rt == NULL || sub.pos >= p->ntoks || p->toks[sub.pos].kind != T_IDENT)
        return i;
    sub.pos++; /* the name */
    if (sub.pos >= p->ntoks || p->toks[sub.pos].kind != T_LPAREN)
        return i;
    sub.pos++;
    Type **ptypes =
        arena_alloc_array(p->arena, (size_t)(nparams > 0 ? nparams : 1), sizeof(Type *));
    int np = 0;
    while (sub.pos < p->ntoks && p->toks[sub.pos].kind != T_RPAREN &&
           p->toks[sub.pos].kind != T_EOF) {
        if (np >= nparams) {
            sub.pos++;
            continue;
        }
        Type *pt = parse_type(&sub);
        if (pt == NULL)
            return i;
        ptypes[np++] = pt;
        while (sub.pos < p->ntoks && p->toks[sub.pos].kind != T_COMMA &&
               p->toks[sub.pos].kind != T_RPAREN && p->toks[sub.pos].kind != T_EOF)
            sub.pos++;
        if (sub.pos < p->ntoks && p->toks[sub.pos].kind == T_COMMA)
            sub.pos++;
    }
    StructMethod *m = arena_alloc(p->arena, sizeof *m);
    m->name = arena_strdup(p->arena, mname);
    m->ret = rt;
    m->ptypes = ptypes;
    m->nparams = np;
    m->body = NULL; /* the real parser fills this in */
    m->is_virtual = is_virtual;
    m->vtable_index = -1;
    struct_add_method(p->ty, sd, m);
    return j;
}

/* Whether the token at `j` can begin the member that follows an
 * expression-bodied member whose ';' was omitted. A member starts with a type,
 * a destructor's `~`, or a `virtual`/`override` modifier. A bare identifier is
 * deliberately not a stop: an expression body often ends in one (`=> v`), and
 * stopping there would leave the body's last token for the member scanners to
 * misread as a member of its own.
 *
 * The one identifier that is a stop is one that resolves as a type and is
 * followed by the member's name. A member typed with a user-defined struct
 * begins with an identifier, so `=> v` written before `P Copy()` reads as
 * `v P Copy (` at the token level and nothing but type resolution tells the
 * body's last token from the member's first: a value expression cannot begin
 * with a type, while a member with no declared type is not legal. parse_type
 * runs on a sub-parser here, the way scan_plain_field does, and its cursor
 * landing on a name is what makes the shape `type name`. */
static int at_member_start(Parser *p, int j) {
    if (j >= p->ntoks)
        return 1;
    TokenKind k = p->toks[j].kind;
    if (k == T_EOF || k == T_TILDE || k == T_KW_VIRTUAL || k == T_KW_OVERRIDE || is_type_token(k))
        return 1;
    if (k != T_IDENT)
        return 0;
    Parser sub = *p;
    sub.pos = j;
    if (parse_type(&sub) == NULL)
        return 0;
    return sub.pos < p->ntoks && p->toks[sub.pos].kind == T_IDENT;
}

/* Whether the token at `j` can begin whatever follows a top-level expression
 * body: a declaration's leading type, a statement keyword, or two adjacent
 * identifiers, which at statement level can only be a declaration's type and
 * name. A lone identifier is not a stop, because a body often begins with one
 * (a call's callee); stopping early only costs the pass a few tokens it then
 * re-scans one at a time, while skipping past the next declaration would lose
 * its signature and break calls made before it. */
static int at_decl_start(Parser *p, int j) {
    if (j >= p->ntoks)
        return 1;
    TokenKind k = p->toks[j].kind;
    if (k == T_EOF || is_type_token(k))
        return 1;
    switch (k) {
    case T_KW_VAR:
    case T_KW_CONST:
    case T_KW_RETURN:
    case T_KW_IF:
    case T_KW_WHILE:
    case T_KW_FOR:
    case T_KW_FOREACH:
    case T_KW_BREAK:
    case T_KW_CONTINUE:
    case T_KW_MATCH:
    case T_KW_MUT:
    case T_KW_NAMESPACE:
    case T_KW_IMPORT:
    case T_KW_EXTERN:
    case T_KW_EXPORT:
        return 1;
    default:
        break;
    }
    return k == T_IDENT && j + 1 < p->ntoks && p->toks[j + 1].kind == T_IDENT;
}

/* Index just past one plain field, `type name` with an optional `;`, or `i`
 * unchanged if this is not one. A plain field is exactly that shape, which is
 * what lets the pre-scan stop at the first method and leave it to the real
 * parser -- registering a method twice would emit it twice. */
static int scan_plain_field(Parser *p, int i, StructDef *sd) {
    if (i >= p->ntoks)
        return i;
    if (!is_type_token(p->toks[i].kind) && p->toks[i].kind != T_IDENT)
        return i;
    int j = skip_type_tokens(p, i);
    if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
        return i;
    char *name = p->toks[j].text;
    j++;
    /* The ';' is optional: a field ends before '}', before the next member, or
     * at an explicit ';'. Anything else (e.g. '(') is a method, not a field. */
    if (j < p->ntoks && p->toks[j].kind == T_SEMI) {
        j++;
    } else if (j >= p->ntoks || p->toks[j].kind == T_RBRACE || p->toks[j].kind == T_TILDE ||
               p->toks[j].kind == T_KW_VIRTUAL || p->toks[j].kind == T_KW_OVERRIDE ||
               p->toks[j].kind == T_KW_FN || is_type_token(p->toks[j].kind) ||
               p->toks[j].kind == T_IDENT) {
        /* implicit terminator: leave j where it is */
    } else {
        return i;
    }
    /* Resolve the type with the cursor on it, without moving the caller's. */
    Parser sub = *p;
    sub.pos = i;
    Type *ft = parse_type(&sub);
    if (ft == NULL)
        return i;
    /* The instance of a generic type registers in parse_struct_decl, which
     * re-reads every member with the parameters bound; this shared scan also
     * runs for it, but only to keep the walk moving. Registering here as well
     * put each field in the layout twice, which inflated a positional
     * constructor's expected argument count and padded every object out to
     * three sizes. */
    if (p->inst_name == NULL)
        struct_add_field(p->ty, sd, name, ft);
    return j;
}

/* Gives every struct, class and enum its layout before anything is parsed.
 *
 * The name pre-scan above exists so a signature can name a struct declared later
 * in the file. A name alone is not enough: `p.x` needs the field and
 * `new Pt(3, 4)` needs the size, and both were filled in by parse_struct_decl,
 * which runs in source order. A struct declared after its use was therefore
 * registered as a name and nothing more, and using it read a struct with no
 * fields at all.
 *
 * Only plain fields are read here, and only as far as they run. A method, a
 * property or a constructor needs the real parser, so the scan stops at the
 * first of those; the declaration is completed as before when the parser reaches
 * it, after discarding what this pass filled in so the fields are not added
 * twice. The result is that a type may be used before it is declared. */
/* Registers every field and method signature of the body starting at `j`, and
 * returns the index of the closing brace.
 *
 * Signatures before bodies, so a method may call a method declared later in the
 * same class. That is why it is a separate pass at all: the members are read
 * again by parse_struct_decl, which fills the bodies in, and re-registering one
 * that already has a body would emit it twice.
 *
 * `ctor_name` is the name a constructor is spelled with, which for a generic
 * declaration is the template's rather than the instance's -- a `Vec<int>` is
 * built from `Vec() { ... }`, and comparing the token against the instance's
 * mangled name would file the constructor as an ordinary method called `Vec`. */
static int prescan_struct_members(Parser *p, StructDef *sd, int j, const char *ctor_name) {
    while (j < p->ntoks) {
        if (p->toks[j].kind == T_RBRACE)
            break;
        /* `~Type() { ... }`: a destructor registers no signature here, because
         * the real parse reaches it through parse_method. The form still has to
         * be recognised, or this pass stops dead.
         *
         * It used to stop, and that is worse than it sounds: `~` is not a member
         * the two scanners below know, the pass gave up rather than stepping
         * over one token, and every member written after a destructor went
         * unregistered. A `Vec<T>` with a destructor could not call its own
         * `Grow`, because the pass that exists so a method can call a method
         * declared later had never heard of the destructor sitting in front of
         * it. */
        if (p->toks[j].kind == T_TILDE) {
            int k = j + 1;
            if (k < p->ntoks && p->toks[k].kind == T_IDENT)
                k++;
            /* Past the parameter list, which a destructor spells empty. */
            if (k < p->ntoks && p->toks[k].kind == T_LPAREN) {
                int d = 0;
                while (k < p->ntoks) {
                    if (p->toks[k].kind == T_LPAREN) {
                        d++;
                    } else if (p->toks[k].kind == T_RPAREN) {
                        d--;
                        if (d == 0) {
                            k++;
                            break;
                        }
                    } else if (p->toks[k].kind == T_EOF) {
                        break;
                    }
                    k++;
                }
            }
            if (k < p->ntoks && p->toks[k].kind == T_LBRACE) {
                Parser sub = *p;
                sub.pos = k;
                skip_braced_block(&sub);
                k = sub.pos;
            } else if (k < p->ntoks && p->toks[k].kind == T_FATARROW) {
                /* Expression-bodied destructor: skip to its terminator. The
                 * ';' is optional, so a '}' (or end of file) ends it too -- as
                 * does the first token that can begin the next member, which
                 * keeps that member registered for the methods before it. */
                while (k < p->ntoks && p->toks[k].kind != T_SEMI && p->toks[k].kind != T_EOF &&
                       p->toks[k].kind != T_RBRACE && !at_member_start(p, k))
                    k++;
                if (k < p->ntoks && p->toks[k].kind == T_SEMI)
                    k++;
            }
            if (k == j + 1) {
                /* A `~` that is not the start of a destructor, so leave it to
                 * the real parse to report. */
                j++;
                continue;
            }
            j = k;
            continue;
        }
        int next = scan_plain_field(p, j, sd);
        if (next != j) {
            j = next;
            continue;
        }
        next = scan_method_signature(p, j, sd, ctor_name);
        if (next != j) {
            j = next;
            /* Step over the body. A method's is not this pass's to read, but
             * stopping in front of it would mean every member after the first
             * method went unregistered -- so a struct with two methods looked
             * like it had one. */
            if (j < p->ntoks && p->toks[j].kind == T_LBRACE) {
                int d = 0;
                while (j < p->ntoks) {
                    if (p->toks[j].kind == T_LBRACE) {
                        d++;
                    } else if (p->toks[j].kind == T_RBRACE) {
                        d--;
                        if (d == 0) {
                            j++;
                            break;
                        }
                    }
                    j++;
                }
            } else if (j < p->ntoks && p->toks[j].kind == T_FATARROW) {
                /* An expression-bodied member: skip to its terminator. The ';'
                 * is optional, so a '}' (or end of file) ends it too -- as does
                 * the first token that can begin the next member, which keeps
                 * that member registered for the methods before it. */
                while (j < p->ntoks && p->toks[j].kind != T_SEMI && p->toks[j].kind != T_EOF &&
                       p->toks[j].kind != T_RBRACE && !at_member_start(p, j))
                    j++;
                if (j < p->ntoks && p->toks[j].kind == T_SEMI)
                    j++;
            }
            continue;
        }
        break;
    }
    return j;
}

static void prescan_struct_fields(Parser *p) {
    NsSweep ns;
    ns_sweep_init(&ns);
    int si = 0;
    for (int i = 0; i < p->ntoks; i++) {
        while (si < i)
            ns_sweep_step(&ns, p, si++);
        ns_sweep_step(&ns, p, i);
        si = i + 1;
        p->ns_prefix = (char *)ns_sweep_here(&ns);
        int is_class = p->toks[i].kind == T_KW_CLASS;
        if (!is_class && p->toks[i].kind != T_KW_STRUCT)
            continue;
        int j = i + 1;
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            continue;
        char *name = ns_join(p, ns_sweep_here(&ns), p->toks[j].text);
        char *written = p->toks[j].text;
        j++;
        /* A generic declaration has no layout of its own: each instance gets its
         * fields laid out when it is built, and registering the template as a
         * plain struct here would produce a type with no fields that a bare
         * `Vec` would then resolve to. */
        if (find_gtype(p, name) != NULL) {
            i = j - 1;
            continue;
        }
        StructDef *sd = type_define_struct(p->ty, name);
        if (sd == NULL)
            continue;
        sd->is_class = is_class;
        if (is_class) {
            sd->align = 8;
            sd->size = 8; /* vptr */
            /* A `: Base` is skipped here so the pre-scan and the real parse
             * agree on where the body starts. The diagnostic for it belongs to
             * the real parse, which is the one that has a span to point at. */
            if (j < p->ntoks && p->toks[j].kind == T_COLON) {
                j++;
                if (j < p->ntoks && p->toks[j].kind == T_IDENT)
                    j++;
            }
        }
        if (j >= p->ntoks || p->toks[j].kind != T_LBRACE)
            continue;
        prescan_struct_members(p, sd, j + 1, written);
        if (sd->align < 1)
            sd->align = 1;
        sd->size = (sd->size + sd->align - 1) / sd->align * sd->align;
        sd->complete = 1;
        sd->prescanned = 1;
        i = j - 1;
    }

    /* Interfaces: the same problem again. A signature that names an interface is
     * prescanned before any interface declaration is parsed, so without this the
     * name did not resolve yet and a function returning one came back typed
     * `int`. The methods are read here too, because a call site resolves a method
     * name to an itab index, and that has to work whatever order the declarations
     * are in. An interface is only signatures, so there is no body to skip. */
    for (int i = 0; i < p->ntoks; i++) {
        while (si < i)
            ns_sweep_step(&ns, p, si++);
        ns_sweep_step(&ns, p, i);
        si = i + 1;
        p->ns_prefix = (char *)ns_sweep_here(&ns);
        if (p->toks[i].kind != T_KW_INTERFACE)
            continue;
        int j = i + 1;
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            continue;
        char *name = ns_join(p, ns_sweep_here(&ns), p->toks[j].text);
        j++;
        IfaceDef *id = type_define_iface(p->ty, name);
        if (id == NULL || id->prescanned)
            continue;
        if (j >= p->ntoks || p->toks[j].kind != T_LBRACE)
            continue;
        j++;
        while (j < p->ntoks && p->toks[j].kind != T_RBRACE && p->toks[j].kind != T_EOF) {
            /* Rust-style `fn Name(params) [-> Ret];` */
            if (p->toks[j].kind == T_KW_FN) {
                if (j + 1 >= p->ntoks || p->toks[j + 1].kind != T_IDENT)
                    break;
                char *mname = p->toks[j + 1].text;
                int k = j + 2;
                if (k >= p->ntoks || p->toks[k].kind != T_LPAREN)
                    break;
                int depth = 1;
                for (k++; k < p->ntoks && depth > 0; k++) {
                    if (p->toks[k].kind == T_LPAREN)
                        depth++;
                    else if (p->toks[k].kind == T_RPAREN)
                        depth--;
                }
                if (depth != 0)
                    break;
                int after = k;
                Type *rt = type_void(p->ty);
                if (after < p->ntoks && p->toks[after].kind == T_ARROW) {
                    Parser sr = *p;
                    sr.pos = after + 1;
                    Type *prt = parse_type(&sr);
                    if (prt == NULL)
                        break;
                    rt = prt;
                    after = sr.pos;
                }
                /* param types */
                Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
                int np = 0;
                {
                    Parser s2 = *p;
                    s2.pos = j + 3; /* after `fn Name (` */
                    while (s2.pos < k - 1 && np < Z_MAX_ARGS) {
                        if (p->toks[s2.pos].kind == T_KW_MUT) {
                            s2.pos++;
                            continue;
                        }
                        if (!is_type_token(p->toks[s2.pos].kind) &&
                            p->toks[s2.pos].kind != T_IDENT) {
                            s2.pos++;
                            continue;
                        }
                        int e = skip_type_tokens(p, s2.pos);
                        if (e >= p->ntoks || p->toks[e].kind != T_IDENT) {
                            s2.pos++;
                            continue;
                        }
                        Parser s3 = *p;
                        s3.pos = s2.pos;
                        Type *pt = parse_type(&s3);
                        if (pt != NULL)
                            ptypes[np++] = pt;
                        s2.pos = e + 1;
                        while (s2.pos < k - 1 && p->toks[s2.pos].kind != T_COMMA)
                            s2.pos++;
                        if (s2.pos < k - 1 && p->toks[s2.pos].kind == T_COMMA)
                            s2.pos++;
                    }
                }
                if (iface_find_method(id, mname) == NULL)
                    iface_add_method(p->ty, id, mname, type_fnptr(p->ty, ptypes, np, rt));
                j = after;
                /* Signatures end with an optional ';'. */
                if (j < p->ntoks && p->toks[j].kind == T_SEMI)
                    j++;
                continue;
            }
            int tstart = j;
            if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
                break;
            j = skip_type_tokens(p, j);
            if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
                break;
            char *mname = p->toks[j].text;
            j++;
            if (j >= p->ntoks || p->toks[j].kind != T_LPAREN)
                break;
            int depth = 1;
            for (j++; j < p->ntoks && depth > 0; j++) {
                if (p->toks[j].kind == T_LPAREN)
                    depth++;
                else if (p->toks[j].kind == T_RPAREN)
                    depth--;
            }
            if (depth != 0)
                break;
            Parser sub = *p;
            sub.pos = tstart;
            Type *rt = parse_type(&sub);
            if (rt == NULL)
                break;
            /* Re-read the parameter list for its types. */
            Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
            int np = 0;
            for (int k = tstart + 1; k < j - 1 && np < Z_MAX_ARGS; k++) {
                if (!is_type_token(p->toks[k].kind) && p->toks[k].kind != T_IDENT)
                    continue;
                int e = skip_type_tokens(p, k);
                if (e >= p->ntoks || p->toks[e].kind != T_IDENT)
                    continue;
                Parser s2 = *p;
                s2.pos = k;
                Type *pt = parse_type(&s2);
                if (pt != NULL)
                    ptypes[np++] = pt;
                k = e;
            }
            if (iface_find_method(id, mname) == NULL)
                iface_add_method(p->ty, id, mname, type_fnptr(p->ty, ptypes, np, rt));
            /* Signatures end with an optional ';'. */
            if (j < p->ntoks && p->toks[j].kind == T_SEMI)
                j++;
        }
        id->prescanned = 1;
        i = j - 1;
    }

    /* Enums: the same problem, with variants instead of fields. A `match` on an
     * enum declared later needs its variant list to check exhaustiveness and to
     * bind payloads. */
    for (int i = 0; i < p->ntoks; i++) {
        while (si < i)
            ns_sweep_step(&ns, p, si++);
        ns_sweep_step(&ns, p, i);
        si = i + 1;
        p->ns_prefix = (char *)ns_sweep_here(&ns);
        if (p->toks[i].kind != T_KW_ENUM)
            continue;
        int j = i + 1;
        if (j >= p->ntoks || p->toks[j].kind != T_IDENT)
            continue;
        char *name = ns_join(p, ns_sweep_here(&ns), p->toks[j].text);
        j++;
        UnionDef *ud = type_define_union(p->ty, name);
        if (ud == NULL)
            continue;
        if (j >= p->ntoks || p->toks[j].kind != T_LBRACE)
            continue;
        j++;
        while (j < p->ntoks && p->toks[j].kind != T_RBRACE && p->toks[j].kind != T_EOF) {
            if (p->toks[j].kind != T_IDENT)
                break;
            char *vname = p->toks[j].text;
            j++;
            VariantFields vf = {NULL, NULL, 0, 0};
            if (j < p->ntoks && p->toks[j].kind == T_LPAREN) {
                j++;
                while (j < p->ntoks && p->toks[j].kind != T_RPAREN && p->toks[j].kind != T_EOF) {
                    if (!is_type_token(p->toks[j].kind) && p->toks[j].kind != T_IDENT)
                        break;
                    int k = skip_type_tokens(p, j);
                    if (k >= p->ntoks || p->toks[k].kind != T_IDENT)
                        break;
                    Parser sub = *p;
                    sub.pos = j;
                    Type *ft = parse_type(&sub);
                    variant_fields_add(p, &vf, ft, p->toks[k].text);
                    j = k + 1;
                    if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                        j++;
                }
                if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                    j++;
            }
            union_add_variant(p->ty, ud, vname, vf.ftypes, vf.fnames, vf.n);
            if (j < p->ntoks && p->toks[j].kind == T_COMMA) {
                j++;
                continue;
            }
            break;
        }
        union_finish(ud);
        ud->prescanned = 1;
        i = j - 1;
    }
    p->ns_prefix = NULL;
}

/* Prescan one Rust-style `fn name(params) [-> ret]` starting with `fn` at index
 * `fi` (or `extern`/`export` at `fi` followed by `fn`). On success records the
 * signature, sets `*out_next` past the body/declaration, and returns 1.
 * Returns 0 if this is not an fn-form (caller falls through). Generic
 * `fn f<T>` is skipped (returns 1 with body skipped but no signature), since
 * parse_func records generics. */
static int prescan_fn_sig(Parser *p, int fi, int is_extern_export, int kw_extern,
                          const char *ns_pfx, int *out_next) {
    int i = fi;
    if (is_extern_export) {
        /* fi points at extern/export; fn must follow */
        if (i + 2 >= p->ntoks || p->toks[i + 1].kind != T_KW_FN || p->toks[i + 2].kind != T_IDENT)
            return 0;
        i += 1; /* now at fn */
    } else {
        if (i >= p->ntoks || p->toks[i].kind != T_KW_FN)
            return 0;
    }
    /* i at 'fn', i+1 at name */
    if (i + 1 >= p->ntoks || p->toks[i + 1].kind != T_IDENT)
        return 0;
    const char *fname = ns_join(p, ns_pfx, p->toks[i + 1].text);
    int j = i + 2;
    if (j < p->ntoks && p->toks[j].kind == T_LT)
        return 0; /* generic: leave to parse_func; do not consume here */
    if (j >= p->ntoks || p->toks[j].kind != T_LPAREN)
        return 0;
    j++;
    Type *ptypes[16];
    int np = 0;
    int is_ext = 0;
    if (j < p->ntoks && p->toks[j].kind == T_KW_THIS) {
        is_ext = 1;
        j++;
    }
    while (j < p->ntoks && p->toks[j].kind != T_RPAREN) {
        if (p->toks[j].kind == T_KW_MUT) {
            j++;
            continue;
        }
        if (is_type_token(p->toks[j].kind) || p->toks[j].kind == T_IDENT) {
            int k = skip_type_tokens(p, j);
            if (k < p->ntoks && p->toks[k].kind == T_IDENT) {
                if (np < 16)
                    ptypes[np++] = parse_type_at(p, j);
                j = k + 1;
                while (j < p->ntoks && p->toks[j].kind != T_COMMA && p->toks[j].kind != T_RPAREN)
                    j++;
                if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                    j++;
                continue;
            }
        }
        j++;
    }
    if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
        j++;
    Type *ret = type_void(p->ty);
    if (j < p->ntoks && p->toks[j].kind == T_ARROW) {
        ret = parse_type_at(p, j + 1);
        Parser sub = *p;
        sub.pos = j + 1;
        if (parse_type(&sub) != NULL)
            j = sub.pos;
        else
            j++;
    }
    if (j < p->ntoks && p->toks[j].kind == T_LBRACE) {
        if (is_extern_export) {
            if (kw_extern)
                return 0; /* extern cannot have body; leave to real parser for error */
            add_sig_ex(p, fname, ret, ptypes, np, 0, 0);
            Sig *ex = find_sig(p, fname);
            if (ex != NULL)
                ex->is_export = 1;
        } else {
            add_sig(p, fname, ret, ptypes, np, is_ext);
        }
        int d = 0;
        while (j < p->ntoks) {
            if (p->toks[j].kind == T_LBRACE)
                d++;
            else if (p->toks[j].kind == T_RBRACE) {
                d--;
                if (d == 0) {
                    j++;
                    break;
                }
            }
            j++;
        }
        *out_next = j;
        return 1;
    }
    if (j < p->ntoks && p->toks[j].kind == T_FATARROW) {
        if (is_extern_export && kw_extern)
            return 0;
        if (is_extern_export) {
            add_sig_ex(p, fname, ret, ptypes, np, 0, 0);
            Sig *ex = find_sig(p, fname);
            if (ex != NULL)
                ex->is_export = 1;
        } else {
            add_sig(p, fname, ret, ptypes, np, is_ext);
        }
        j++;
        /* Skip the expression body. The ';' is optional, so an unmatched
         * closer or the end of the file ends it too -- an expression cannot
         * contain one at depth zero. Depth is tracked so a ';' inside
         * parens/braces (e.g. a for header in a lambda body) does not end
         * the skip early, and the first token that can begin the next
         * declaration or statement ends it as well, so a declaration written
         * after the omitted ';' stays registered for the calls before it. */
        int pd = 0, brd = 0, bcd = 0;
        while (j < p->ntoks) {
            TokenKind k = p->toks[j].kind;
            if (k == T_EOF)
                break;
            if (k == T_LPAREN)
                pd++;
            else if (k == T_LBRACKET)
                brd++;
            else if (k == T_LBRACE)
                bcd++;
            else if (k == T_RPAREN) {
                if (pd == 0)
                    break;
                pd--;
            } else if (k == T_RBRACKET) {
                if (brd == 0)
                    break;
                brd--;
            } else if (k == T_RBRACE) {
                if (bcd == 0)
                    break;
                bcd--;
            } else if (k == T_SEMI && pd == 0 && brd == 0 && bcd == 0) {
                j++;
                break;
            } else if (pd == 0 && brd == 0 && bcd == 0 && at_decl_start(p, j)) {
                break;
            }
            j++;
        }
        *out_next = j;
        return 1;
    }
    if (j < p->ntoks && p->toks[j].kind == T_SEMI) {
        if (is_extern_export) {
            add_sig_ex(p, fname, ret, ptypes, np, 0, kw_extern ? 1 : 0);
        } else {
            add_sig(p, fname, ret, ptypes, np, is_ext);
        }
        *out_next = j + 1;
        return 1;
    }
    return 0;
}

static void prescan_signatures(Parser *p) {
    NsSweep ns;
    ns_sweep_init(&ns);
    int si = 0;
    int i = 0;
    while (i < p->ntoks) {
        /* Jumps below land past balanced regions, so catching the sweep up
         * keeps brace/namespace tracking exact for legal programs. */
        while (si < i)
            ns_sweep_step(&ns, p, si++);
        ns_sweep_step(&ns, p, i);
        si = i + 1;
        Token *t = &p->toks[i];
        const char *pfx = ns_sweep_here(&ns);
        int member = ns_sweep_member(&ns);
        p->ns_prefix = (char *)pfx;
        /* `extern int f(...);` declares a C symbol and `export int f(...) {...}`
         * publishes a Z one. Both need their signature recorded here so calls
         * type-check, and both make codegen use the name as written. */
        if (member && (t->kind == T_KW_EXTERN || t->kind == T_KW_EXPORT)) {
            int kw_extern = t->kind == T_KW_EXTERN;
            {
                int nx = 0;
                if (prescan_fn_sig(p, i, 1, kw_extern, pfx, &nx)) {
                    i = nx;
                    continue;
                }
            }
            int j = skip_type_tokens(p, i + 1);
            if (j + 1 < p->ntoks && p->toks[j].kind == T_IDENT && p->toks[j + 1].kind == T_LPAREN) {
                j += 2;
                Type *ptypes[16];
                int np = 0;
                while (j < p->ntoks && p->toks[j].kind != T_RPAREN) {
                    if (is_type_token(p->toks[j].kind) || p->toks[j].kind == T_IDENT) {
                        int k = skip_type_tokens(p, j);
                        if (k < p->ntoks && p->toks[k].kind == T_IDENT) {
                            if (np < 16)
                                ptypes[np++] = parse_type_at(p, j);
                            j = k + 1;
                            while (j < p->ntoks && p->toks[j].kind != T_COMMA &&
                                   p->toks[j].kind != T_RPAREN)
                                j++;
                            if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                                j++;
                            continue;
                        }
                    }
                    j++;
                }
                if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                    j++;
                /* An extern declaration ends with an optional ';': record the
                 * signature whenever the shape matched, consuming a ';' when
                 * one is written. */
                if (kw_extern &&
                    (j >= p->ntoks || p->toks[j].kind == T_SEMI || p->toks[j].kind == T_RBRACE ||
                     p->toks[j].kind == T_EOF || is_type_token(p->toks[j].kind) ||
                     p->toks[j].kind == T_IDENT || p->toks[j].kind == T_KW_FN ||
                     p->toks[j].kind == T_KW_EXTERN || p->toks[j].kind == T_KW_EXPORT)) {
                    Type *ret = parse_type_at(p, i + 1);
                    add_sig_ex(p, ns_join(p, pfx, p->toks[skip_type_tokens(p, i + 1)].text), ret,
                               ptypes, np, 0, 1);
                    if (j < p->ntoks && p->toks[j].kind == T_SEMI)
                        j++;
                    i = j;
                    continue;
                }
                if (j < p->ntoks && p->toks[j].kind == T_LBRACE && !kw_extern) {
                    /* Record the signature, then skip the body by brace
                     * matching, exactly as the plain function path does. */
                    Type *ret = parse_type_at(p, i + 1);
                    const char *exname = ns_join(p, pfx, p->toks[skip_type_tokens(p, i + 1)].text);
                    add_sig_ex(p, exname, ret, ptypes, np, 0, 0);
                    Sig *ex = find_sig(p, exname);
                    if (ex != NULL)
                        ex->is_export = 1;
                    int d = 0;
                    while (j < p->ntoks) {
                        if (p->toks[j].kind == T_LBRACE)
                            d++;
                        else if (p->toks[j].kind == T_RBRACE) {
                            d--;
                            if (d == 0) {
                                j++;
                                break;
                            }
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
            }
        }
        /* A lambda's parameter list is shaped exactly like a function
         * declaration's -- a type, a name, and a '(' -- so the scan below would
         * read `(int x) => ...` as the declaration of a function called `x` and
         * register a nonsense signature under a name the program never declared.
         *
         * Skipping the body is enough to tell them apart: a lambda can only
         * appear where an expression can, and the `=>` is unambiguous. The skip
         * runs to the statement's semicolon, tracking brace depth so a
         * block-bodied lambda's own semicolons are not mistaken for the end. */
        if (member && t->kind == T_FATARROW) {
            int braces = 0;
            while (i < p->ntoks) {
                TokenKind k = p->toks[i].kind;
                if (k == T_LBRACE) {
                    braces++;
                } else if (k == T_RBRACE) {
                    if (braces == 0)
                        break; /* the end of the enclosing block */
                    braces--;
                } else if (k == T_SEMI && braces == 0) {
                    i++;
                    break;
                } else if (k == T_EOF) {
                    break;
                } else if (braces == 0 && at_decl_start(p, i)) {
                    /* The ';' after the lambda was omitted: stop at the first
                     * token that can begin the next declaration or statement,
                     * so what follows stays registered for the calls before
                     * it. */
                    break;
                }
                i++;
            }
            continue;
        }
        if (member && t->kind == T_KW_FN) {
            int nx = 0;
            if (prescan_fn_sig(p, i, 0, 0, pfx, &nx)) {
                i = nx;
                continue;
            }
            i++;
            continue;
        }
        if (member && (is_type_token(t->kind) || t->kind == T_IDENT)) {
            int j = skip_type_tokens(p, i);
            if (j + 1 < p->ntoks && p->toks[j].kind == T_IDENT && p->toks[j + 1].kind == T_LPAREN) {
                /* Parse a candidate parameter list. */
                j += 2; /* skip name and '(' */
                Type *ptypes[16];
                int np = 0;
                int is_ext = 0;
                if (j < p->ntoks && p->toks[j].kind == T_KW_THIS) {
                    is_ext = 1;
                    j++;
                }
                while (j < p->ntoks && p->toks[j].kind != T_RPAREN) {
                    if (is_type_token(p->toks[j].kind) || p->toks[j].kind == T_IDENT) {
                        int k = skip_type_tokens(p, j);
                        if (k < p->ntoks && p->toks[k].kind == T_IDENT) {
                            if (np < 16)
                                ptypes[np++] = parse_type_at(p, j);
                            j = k + 1; /* skip param name */
                            while (j < p->ntoks && p->toks[j].kind != T_COMMA &&
                                   p->toks[j].kind != T_RPAREN) {
                                j++;
                            }
                            if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                                j++;
                            continue;
                        }
                    }
                    j++;
                }
                if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                    j++;
                if (j < p->ntoks && p->toks[j].kind == T_LBRACE) {
                    Type *ret = parse_type_at(p, i);
                    add_sig(p, ns_join(p, pfx, p->toks[skip_type_tokens(p, i)].text), ret, ptypes,
                            np, is_ext);
                    /* Skip the function body via brace matching. */
                    int d = 0;
                    while (j < p->ntoks) {
                        if (p->toks[j].kind == T_LBRACE)
                            d++;
                        else if (p->toks[j].kind == T_RBRACE) {
                            d--;
                            if (d == 0) {
                                j++;
                                break;
                            }
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
                if (j < p->ntoks && p->toks[j].kind == T_FATARROW) {
                    /* Expression-bodied function: record signature, skip the
                     * body. The ';' is optional, so an unmatched closer or the
                     * end of the file ends it too -- as does the first token
                     * that can begin the next declaration or statement, so a
                     * declaration written after the omitted ';' stays
                     * registered for the calls before it. */
                    Type *ret = parse_type_at(p, i);
                    add_sig(p, ns_join(p, pfx, p->toks[skip_type_tokens(p, i)].text), ret, ptypes,
                            np, is_ext);
                    j++;
                    int pd = 0, brd = 0, bcd = 0;
                    while (j < p->ntoks) {
                        TokenKind k = p->toks[j].kind;
                        if (k == T_EOF)
                            break;
                        if (k == T_LPAREN)
                            pd++;
                        else if (k == T_LBRACKET)
                            brd++;
                        else if (k == T_LBRACE)
                            bcd++;
                        else if (k == T_RPAREN) {
                            if (pd == 0)
                                break;
                            pd--;
                        } else if (k == T_RBRACKET) {
                            if (brd == 0)
                                break;
                            brd--;
                        } else if (k == T_RBRACE) {
                            if (bcd == 0)
                                break;
                            bcd--;
                        } else if (k == T_SEMI && pd == 0 && brd == 0 && bcd == 0) {
                            j++;
                            break;
                        } else if (pd == 0 && brd == 0 && bcd == 0 && at_decl_start(p, j)) {
                            break;
                        }
                        j++;
                    }
                    i = j;
                    continue;
                }
                i = j;
                continue;
            }
        }
        i++;
    }
    p->ns_prefix = NULL;
}
