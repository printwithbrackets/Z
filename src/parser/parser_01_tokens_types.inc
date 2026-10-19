/* ---- token helpers ---- */

static Token *peek(Parser *p, int off) {
    int i = p->pos + off;
    if (i >= p->ntoks)
        i = p->ntoks - 1;
    return &p->toks[i];
}

static Token *cur(Parser *p) { return peek(p, 0); }

static Token *advance(Parser *p) {
    Token *t = cur(p);
    if (p->pos < p->ntoks - 1)
        p->pos++;
    return t;
}

static int at(Parser *p, TokenKind kind) { return cur(p)->kind == kind; }

static int match(Parser *p, TokenKind kind) {
    if (at(p, kind)) {
        advance(p);
        return 1;
    }
    return 0;
}

/* ---- AST constructors ---- */

static Expr *new_expr(Parser *p, ExprKind kind, Span span) {
    Expr *e = arena_alloc(p->arena, sizeof *e);
    e->kind = kind;
    e->span = span;
    e->type = NULL;
    e->slot = -1;
    e->gidx = -1;
    e->str_id = -1;
    return e;
}

static Stmt *new_stmt(Parser *p, StmtKind kind, Span span) {
    Stmt *s = arena_alloc(p->arena, sizeof *s);
    s->kind = kind;
    s->span = span;
    s->type = NULL;
    s->slot = -1;
    return s;
}

static int is_kind(Type *t, TypeKind k) { return t != NULL && t->kind == k; }
static int is_unk(Type *t) { return t == NULL; }

static Type *base_type_from_token(Parser *p, TokenKind k) {
    switch (k) {
    case T_KW_INT:
        return type_int(p->ty);
    case T_KW_BOOL:
        return type_bool(p->ty);
    case T_KW_FLOAT:
        return type_f64(p->ty);
    case T_KW_STRING:
        return type_string(p->ty);
    case T_KW_VOID:
        return type_void(p->ty);
    default:
        return NULL;
    }
}

static int is_type_token(TokenKind k) {
    /* `fn` starts a function type, so anywhere a type may begin it counts. */
    return k == T_KW_INT || k == T_KW_BOOL || k == T_KW_FLOAT || k == T_KW_STRING ||
           k == T_KW_VOID || k == T_KW_FN || k == T_KW_METHOD || k == T_KW_CLOSURE;
}

/* Parses a type possibly followed by `*` stars and `[]` array suffixes.
 * Used in declarations, parameters, and return positions. */
/* Resolves a type-name token: builtins, or a declared struct by name. */
static Type *base_type_or_name(Parser *p, Token *t) {
    Type *b = base_type_from_token(p, t->kind);
    if (b != NULL)
        return b;
    if (t->kind == T_IDENT) {
        /* An interface name resolves here too, so a parameter, a return type and
         * a local can all be written `I` with nothing else to go on. */
        Type *ifc = type_find_iface(p->ty, t->text);
        if (ifc == NULL)
            ifc = ns_lookup(p, t->text, look_iface);
        if (ifc != NULL)
            return ifc;
        /* An in-scope generic type-parameter binding (T) shadows any struct of
         * the same name. */
        for (int i = p->ntbind - 1; i >= 0; i--)
            if (strcmp(p->tbinds[i].name, t->text) == 0)
                return p->tbinds[i].ty;
        /* A constructor in a generic declaration is spelled with the name as
         * written (`Vec() { ... }`) while the struct being built is the instance,
         * named after its type arguments. Resolving the written name has to land
         * on the instance or the constructor is not recognised as one. */
        if (p->inst_sdef != NULL && p->inst_srcname != NULL &&
            strcmp(t->text, p->inst_srcname) == 0)
            return type_find_struct(p->ty, p->inst_sdef->name);
        /* The written name is tried first and the dialect second, so a project
         * that declares its own `TextBuffer` gets its own rather than the
         * standard library's `StringBuilder` under that name. */
        Type *st = type_find_struct(p->ty, t->text);
        if (st == NULL)
            st = ns_lookup(p, t->text, look_struct);
        if (st == NULL)
            st = type_find_struct(p->ty, surf(p, t->text));
        if (st != NULL)
            return st;
        Type *un = type_find_union(p->ty, t->text);
        if (un == NULL)
            un = ns_lookup(p, t->text, look_union);
        if (un == NULL)
            un = type_find_union(p->ty, surf(p, t->text));
        return un;
    }
    return NULL;
}

static Type *parse_type(Parser *p);
static Type *parse_type_at(Parser *p, int i);

/* Reads a function type: `fn` "(" type ("," type)* ")" "->" type. A null
 * parameter list means the function takes nothing. */
static Type *parse_fn_type(Parser *p) {
    /* `fn(...)` is a plain code address; `method(...)` binds a receiver; and
     * `closure(...)` is a lambda that captured something. Same syntax, different
     * value -- and the three are distinct types, because they are called
     * differently: only a method or a closure passes a hidden first argument. */
    int mptr = cur(p)->kind == T_KW_METHOD;
    int clos = cur(p)->kind == T_KW_CLOSURE;
    advance(p);
    if (!match(p, T_LPAREN)) {
        diag_error(cur(p)->span, "expected '(' after 'fn'");
        return NULL;
    }
    /* Arena-allocated: the Type keeps this pointer, so it must outlive this
     * function. A stack array would dangle the moment we return. */
    Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
    int np = 0;
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (np >= Z_MAX_ARGS) {
                diag_error(cur(p)->span, "a function type may take at most %d parameters",
                           Z_MAX_ARGS);
                break;
            }
            Type *pt = parse_type(p);
            if (pt == NULL) {
                diag_error(cur(p)->span, "expected a parameter type in function type");
                break;
            }
            ptypes[np++] = pt;
            if (!match(p, T_COMMA))
                break;
        }
    }
    if (!match(p, T_RPAREN)) {
        diag_error(cur(p)->span, "expected ')' to close the function type's parameters");
        return NULL;
    }
    if (!match(p, T_ARROW)) {
        diag_error(cur(p)->span, "expected '->' and a return type after the function type");
        return NULL;
    }
    Type *ret = parse_type(p);
    if (ret == NULL) {
        diag_error(cur(p)->span, "expected a return type in function type");
        return NULL;
    }
    if (clos)
        return type_closure(p->ty, ptypes, np, ret);
    return mptr ? type_mptr(p->ty, ptypes, np, ret) : type_fnptr(p->ty, ptypes, np, ret);
}

/* `Result<T,E>` is a builtin generic type: two type arguments in angle brackets.
 * Recognized by name rather than by a keyword so that a program could still use
 * `Result` as an ordinary identifier outside type position. */
static int at_result_type(Parser *p, int i) {
    return i + 1 < p->ntoks && p->toks[i].kind == T_IDENT &&
           strcmp(p->toks[i].text, "Result") == 0 && p->toks[i + 1].kind == T_LT;
}

/* Parses `Result<T,E>` with the cursor on `Result`. Returns NULL and reports if
 * the arguments do not name one-word types. */
static Type *parse_result_type(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* Result */
    if (!match(p, T_LT)) {
        diag_error(start, "'Result' needs two type arguments: Result<T, E>");
        return NULL;
    }
    Type *ok = parse_type(p);
    if (ok == NULL || !match(p, T_COMMA)) {
        diag_error(start, "'Result' needs two type arguments: Result<T, E>");
        return NULL;
    }
    Type *err = parse_type(p);
    if (err == NULL) {
        diag_error(start, "'Result' needs two type arguments: Result<T, E>");
        return NULL;
    }
    if (!match(p, T_GT)) {
        diag_error(cur(p)->span, "expected '>' to close 'Result<...>'");
        return NULL;
    }
    Type *r = type_result(p->ty, ok, err);
    if (r == NULL) {
        diag_error(start,
                   "'Result<%s, %s>' is not available: each side must be one word "
                   "(int, bool, float, string, or a pointer)",
                   type_name(p->ty, ok), type_name(p->ty, err));
        return NULL;
    }
    return r;
}

/* `Vec<int>`, `Map<string,int>`: a generic type applied to type arguments.
 *
 * The type arguments are parsed first, then the template is instantiated for
 * them -- which is the same order a generic *function* call works in, and for the
 * same reason: the arguments have to be known before there is a type to hand
 * back. The result is a plain struct under a mangled name, so nothing
 * downstream of here knows generics exist.
 *
 * Only reached from a real type position. `new Vec<int>()` goes through
 * parse_new, which does the same thing by hand because it has to recognise the
 * name before it is parsed as a type. */
static Type *parse_gtype(Parser *p, GenericType *g) {
    Span start = cur(p)->span;
    advance(p); /* the name */
    if (!match(p, T_LT)) {
        diag_error_code(cur(p)->span, "type_needs_arguments",
                        "'%s' is a generic type: write it as '%s<...>'", g->name, g->name);
        return NULL;
    }
    Type *args[8];
    int n = 0;
    if (!at(p, T_GT)) {
        for (;;) {
            if (n >= 8) {
                diag_error(cur(p)->span, "a generic type may take at most 8 type arguments");
                break;
            }
            args[n] = parse_type(p);
            if (args[n] == NULL) {
                while (!at(p, T_GT) && !at(p, T_EOF))
                    advance(p);
                break;
            }
            n++;
            if (!match(p, T_COMMA))
                break;
        }
    }
    /* `>>` is one token to the lexer, so `Vec<Vec<int>>` closes two argument
     * lists with it. Splitting it here rather than teaching the lexer about
     * nesting is the smaller and more local change: the token array is already
     * in the arena and nothing re-reads the source, so a `>>` can be rewritten as
     * two `>` in place. A `>>` that did not close two lists would leave the
     * outer one open and swallow everything after it -- which is how
     * `Vec<Vec<int>>` used to read as an error about the line after it. */
    split_shift_closer(p);
    if (!match(p, T_GT))
        diag_error(cur(p)->span, "expected '>' to close the type arguments of '%s'", g->name);
    StructDef *sd = resolve_gtype(p, g, args, n, start);
    return sd != NULL ? type_find_struct(p->ty, sd->name) : NULL;
}

/* Resolves the head identifier at `i` to an absolute namespace path: itself
 * when declared, else innermost-prefix-relative (`dd` inside `b` is `b.dd`).
 * Returns 1 with `out` set. */
static int ns_resolve_head(Parser *p, int i, char *out, size_t cap) {
    const char *t = p->toks[i].text;
    if (ns_known(p, t)) {
        size_t n = strlen(t);
        if (n + 1 > cap)
            return 0;
        memcpy(out, t, n + 1);
        return 1;
    }
    const char *pfx = ns_here(p);
    if (pfx[0] == '\0')
        return 0;
    size_t tn = strlen(t);
    const char *end = pfx + strlen(pfx);
    for (;;) {
        size_t pl = (size_t)(end - pfx);
        if (pl + 1 + tn + 1 > cap)
            return 0;
        memcpy(out, pfx, pl);
        out[pl] = '.';
        memcpy(out + pl + 1, t, tn + 1);
        if (ns_known(p, out))
            return 1;
        if (pl == 0)
            return 0;
        const char *dot = NULL;
        for (size_t k = 0; k < pl; k++)
            if (pfx[k] == '.')
                dot = pfx + k;
        if (dot == NULL)
            return 0;
        end = dot;
    }
}

/* Scans `A(.B)*` at token index `i`: consumes segments while the accumulated
 * path is a declared namespace, leaving the final member name unconsumed iff
 * more segments follow a non-namespace. On success writes the full dotted
 * path and the index just past its last identifier, and returns 1. Returns 0
 * leaving everything untouched when `i` is not a namespace-led dotted name.
 * Purely syntactic apart from the registry check, so skipping and resolving
 * share it. The head resolves relative to the enclosing prefix, so `dd.val`
 * inside `b` scans as `b.dd.val`. */
static int ns_scan_path(Parser *p, int i, char *out_path, size_t path_cap, int *out_end) {
    if (i >= p->ntoks || p->toks[i].kind != T_IDENT)
        return 0;
    if (i + 2 >= p->ntoks || p->toks[i + 1].kind != T_DOT || p->toks[i + 2].kind != T_IDENT)
        return 0;
    if (!ns_resolve_head(p, i, out_path, path_cap))
        return 0;
    size_t n = strlen(out_path);
    int j = i;
    for (;;) {
        /* out_path is a known namespace and `.ident` follows: consume it. */
        size_t tl = strlen(p->toks[j + 2].text);
        if (n + 1 + tl >= path_cap)
            return 0;
        out_path[n++] = '.';
        memcpy(out_path + n, p->toks[j + 2].text, tl + 1);
        n += tl;
        j += 2;
        /* Another `.ident` after a still-namespaced path continues the run. */
        if (j + 2 < p->ntoks && p->toks[j + 1].kind == T_DOT && p->toks[j + 2].kind == T_IDENT &&
            ns_known(p, out_path))
            continue;
        break;
    }
    *out_end = j;
    return 1;
}

/* Nonzero when the cursor opens a namespace-qualified type: `N.T` naming a
 * plain type, or `N.G<...>` naming a generic template. Registry-only (no
 * instantiation), so gates like the typed-declaration check can ask it
 * without consuming. */
static int at_ns_typed(Parser *p) {
    char path[256];
    int end = 0;
    if (!ns_scan_path(p, p->pos, path, sizeof path, &end))
        return 0;
    if (end + 1 < p->ntoks && p->toks[end + 1].kind == T_LT)
        return find_gtype(p, path) != NULL;
    return ns_lookup_plain_type(p, path) != NULL;
}

/* The plain (non-generic-argument) type named by an internal dotted path, or
 * NULL. Type parameters are not namespaced; everything else resolves exactly
 * as written. */
static Type *ns_lookup_plain_type(Parser *p, const char *internal) {
    Type *st = type_find_struct(p->ty, internal);
    if (st != NULL)
        return st;
    Type *id = type_find_iface(p->ty, internal);
    if (id != NULL)
        return id;
    return type_find_union(p->ty, internal);
}

static Type *parse_type(Parser *p) {
    /* A namespace-qualified name: `netlink.Lobby`, `netlink.Vec<int>`. */
    {
        char path[256];
        int end = 0;
        if (ns_scan_path(p, p->pos, path, sizeof path, &end)) {
            /* A generic template applied to arguments: the `<` follows the
             * member name, exactly as in the unqualified shape below. */
            if (end + 1 < p->ntoks && p->toks[end + 1].kind == T_LT) {
                GenericType *g = find_gtype(p, path);
                if (g == NULL)
                    return NULL;
                while (p->pos < end)
                    advance(p);
                Type *gt = parse_gtype(p, g);
                if (gt == NULL)
                    return NULL;
                for (;;) {
                    if (at(p, T_STAR)) {
                        advance(p);
                        gt = type_ptr(p->ty, gt);
                    } else if (at(p, T_LBRACKET) && peek(p, 1)->kind == T_RBRACKET) {
                        advance(p);
                        advance(p);
                        gt = type_array(p->ty, gt, -1);
                    } else {
                        break;
                    }
                }
                return gt;
            }
            Type *t = ns_lookup_plain_type(p, path);
            if (t == NULL)
                return NULL;
            while (p->pos <= end)
                advance(p);
            for (;;) {
                if (at(p, T_STAR)) {
                    advance(p);
                    t = type_ptr(p->ty, t);
                } else if (at(p, T_LBRACKET) && peek(p, 1)->kind == T_RBRACKET) {
                    advance(p);
                    advance(p);
                    t = type_array(p->ty, t, -1);
                } else {
                    break;
                }
            }
            return t;
        }
    }
    if (at(p, T_KW_FN) || at(p, T_KW_METHOD) || at(p, T_KW_CLOSURE))
        return parse_fn_type(p);
    if (at_result_type(p, p->pos))
        return parse_result_type(p);
    /* A generic type name in front of `<` is a template being applied, and has
     * to be recognised before base_type_or_name reports the name as unknown.
     * The result still falls through to the `*`/`[]` loop below, so
     * `Map<string,int>*` is a pointer to an instance and not a parse error. */
    if (at(p, T_IDENT) && peek(p, 1)->kind == T_LT) {
        GenericType *g = find_gtype(p, cur(p)->text);
        if (g == NULL)
            g = ns_lookup(p, cur(p)->text, look_gtype);
        if (g != NULL) {
            Type *gt = parse_gtype(p, g);
            if (gt == NULL)
                return NULL;
            for (;;) {
                if (at(p, T_STAR)) {
                    advance(p);
                    gt = type_ptr(p->ty, gt);
                } else if (at(p, T_LBRACKET) && peek(p, 1)->kind == T_RBRACKET) {
                    advance(p);
                    advance(p);
                    gt = type_array(p->ty, gt, -1);
                } else {
                    break;
                }
            }
            return gt;
        }
    }
    Type *t = base_type_or_name(p, cur(p));
    if (t == NULL)
        return NULL;
    advance(p);
    for (;;) {
        if (at(p, T_STAR)) {
            advance(p);
            t = type_ptr(p->ty, t);
        } else if (at(p, T_LBRACKET) && peek(p, 1)->kind == T_RBRACKET) {
            advance(p);
            advance(p);
            t = type_array(p->ty, t, -1);
        } else {
            break;
        }
    }
    return t;
}

/* Same as parse_type but reads from an explicit token index (used by the
 * pre-scan, which must not disturb the parser position). */
static Type *parse_type_at(Parser *p, int i) {
    if (i >= p->ntoks)
        return NULL;
    /* A function type in a pre-scanned parameter list: the parameter types are
     * separated by commas at depth 0 of the inner list, so a flat scan is
     * enough -- the only thing that matters is how many there are. */
    if (p->toks[i].kind == T_KW_FN || p->toks[i].kind == T_KW_METHOD ||
        p->toks[i].kind == T_KW_CLOSURE) {
        int mptr = p->toks[i].kind == T_KW_METHOD;
        int j = i + 1;
        if (j < p->ntoks && p->toks[j].kind == T_LPAREN) {
            j++;
            Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
            int np = 0;
            while (j < p->ntoks && p->toks[j].kind != T_RPAREN) {
                if (np < Z_MAX_ARGS) {
                    Type *pt = parse_type_at(p, j);
                    if (pt == NULL)
                        break;
                    ptypes[np++] = pt;
                }
                while (j < p->ntoks && p->toks[j].kind != T_COMMA && p->toks[j].kind != T_RPAREN)
                    j++;
                if (j < p->ntoks && p->toks[j].kind == T_COMMA)
                    j++;
            }
            while (j < p->ntoks && p->toks[j].kind != T_RPAREN)
                j++;
            if (j < p->ntoks && p->toks[j].kind == T_RPAREN)
                j++; /* step past ')' so the '->' is the current token */
            Type *ret = type_void(p->ty);
            if (j < p->ntoks && p->toks[j].kind == T_ARROW) {
                Type *r = parse_type_at(p, j + 1);
                if (r != NULL)
                    ret = r;
            }
            if (p->toks[i].kind == T_KW_CLOSURE)
                return type_closure(p->ty, ptypes, np, ret);
            return mptr ? type_mptr(p->ty, ptypes, np, ret) : type_fnptr(p->ty, ptypes, np, ret);
        }
        return NULL;
    }
    if (at_result_type(p, i)) {
        /* Reuse the real parser by pointing a shallow copy of the cursor at the
         * type arguments. parse_type walks p->toks and p->pos, so a copy of the
         * Parser with only the position moved parses the same thing without
         * disturbing the caller's cursor. */
        Parser sub = *p;
        sub.pos = i;
        Type *r = parse_result_type(&sub);
        if (r == NULL)
            return NULL;
        i = sub.pos;
        for (;;) {
            if (i < p->ntoks && p->toks[i].kind == T_STAR) {
                i++;
                r = type_ptr(p->ty, r);
            } else if (i + 1 < p->ntoks && p->toks[i].kind == T_LBRACKET &&
                       p->toks[i + 1].kind == T_RBRACKET) {
                i += 2;
                r = type_array(p->ty, r, -1);
            } else {
                break;
            }
        }
        return r;
    }
    {
        char path[256];
        int end = 0;
        if (ns_scan_path(p, i, path, sizeof path, &end)) {
            if (end + 1 < p->ntoks && p->toks[end + 1].kind == T_LT) {
                GenericType *g = find_gtype(p, path);
                if (g == NULL)
                    return NULL;
                Parser sub = *p;
                /* cursor onto the member name; parse_gtype steps past it */
                sub.pos = end;
                Type *gt = parse_gtype(&sub, g);
                if (gt == NULL)
                    return NULL;
                for (;;) {
                    if (sub.pos < p->ntoks && p->toks[sub.pos].kind == T_STAR) {
                        sub.pos++;
                        gt = type_ptr(p->ty, gt);
                    } else if (sub.pos + 1 < p->ntoks && p->toks[sub.pos].kind == T_LBRACKET &&
                               p->toks[sub.pos + 1].kind == T_RBRACKET) {
                        sub.pos += 2;
                        gt = type_array(p->ty, gt, -1);
                    } else {
                        break;
                    }
                }
                return gt;
            }
            Type *nt = ns_lookup_plain_type(p, path);
            if (nt == NULL)
                return NULL;
            return nt;
        }
    }
    /* A generic template applied to arguments in a signature being
     * pre-scanned: `Vec<int>`. The real parse_type handles this above; the
     * index-based pre-scan path never did, so a function returning or taking
     * an instance recorded a wrong signature. Same instantiation machinery,
     * same result. */
    if (i + 1 < p->ntoks && p->toks[i].kind == T_IDENT && p->toks[i + 1].kind == T_LT) {
        GenericType *g = find_gtype(p, p->toks[i].text);
        if (g == NULL)
            g = ns_lookup(p, p->toks[i].text, look_gtype);
        if (g != NULL) {
            Parser sub = *p;
            sub.pos = i;
            Type *gt = parse_gtype(&sub, g);
            if (gt == NULL)
                return NULL;
            for (;;) {
                if (sub.pos < p->ntoks && p->toks[sub.pos].kind == T_STAR) {
                    sub.pos++;
                    gt = type_ptr(p->ty, gt);
                } else if (sub.pos + 1 < p->ntoks && p->toks[sub.pos].kind == T_LBRACKET &&
                           p->toks[sub.pos + 1].kind == T_RBRACKET) {
                    sub.pos += 2;
                    gt = type_array(p->ty, gt, -1);
                } else {
                    break;
                }
            }
            return gt;
        }
    }
    Type *t = base_type_or_name(p, &p->toks[i]);
    if (t == NULL)
        return NULL;
    i++;
    for (;;) {
        if (i < p->ntoks && p->toks[i].kind == T_STAR) {
            i++;
            t = type_ptr(p->ty, t);
        } else if (i + 1 < p->ntoks && p->toks[i].kind == T_LBRACKET &&
                   p->toks[i + 1].kind == T_RBRACKET) {
            i += 2;
            t = type_array(p->ty, t, -1);
        } else {
            break;
        }
    }
    return t;
}

static void scope_push(Parser *p) {
    Scope *s = arena_alloc(p->arena, sizeof *s);
    s->vars = NULL;
    s->drops = NULL;
    s->ndrops = 0;
    s->drop_cap = 0;
    s->parent = p->scope;
    p->scope = s;
    /* Tracked so a `return` or a `?` can reach every scope it is leaving. A
     * scope beyond the limit is still pushed, so parsing continues and the
     * program gets its other diagnostics rather than nothing. */
    if (p->nopen < MAX_DROP_SCOPES)
        p->open_scopes[p->nopen++] = s;
    else
        diag_error(cur(p)->span, "blocks nested more than %d deep", MAX_DROP_SCOPES);
}

/* Leaves a scope, reporting any variable declared in it that was never
 * referenced. Runs whether or not the block turned out to be well-formed, so a
 * program that fails to compile still gets its warnings.
 *
 * The reporting is deferred: whether a variable is unused is not known until the
 * destructor pass has run, because a value that owns something is used by
 * existing -- the destructor call at the end of its scope is a real use, and
 * `var t = new T()` written only to be destroyed is not dead code. Warning here
 * told every such declaration it was unused, on every program with a
 * destructor in it. The scope is parked on `popped_scopes` instead and
 * `report_unused_locals` walks it once the drops are known. */
static void scope_pop(Parser *p) {
    Scope *s = p->scope;
    if (s == NULL)
        return;
    s->warn_next = p->popped_scopes;
    p->popped_scopes = s;
    if (p->nopen > 0 && p->open_scopes[p->nopen - 1] == s)
        p->nopen--;
    p->scope = s->parent;
}

/* Warns about every local left over from a closed scope that nothing ended up
 * reading. Runs after the destructor pass, which is what marks an owned value
 * used. Same order the warnings used to come out in: innermost scope first,
 * and within a scope, last declaration first. */
static void report_unused_locals(Parser *p) {
    if (!warn_enabled(W_UNUSED_LOCAL))
        return;
    for (Scope *s = p->popped_scopes; s != NULL; s = s->warn_next)
        for (Var *v = s->vars; v != NULL; v = v->next)
            if (!v->used && !v->is_synth)
                diag_warn(W_UNUSED_LOCAL, v->decl_span, "'%s' is declared but never used", v->name);
}

static Var *lookup_var_local(Parser *p, const char *name) {
    for (Var *v = p->scope->vars; v != NULL; v = v->next) {
        if (strcmp(v->name, name) == 0)
            return v;
    }
    return NULL;
}

/* Marks a just-declared binding as compiler-generated, so -Wunused-local does
 * not report the desugarer's own temporaries or a hidden result buffer. */
static void mark_synth(Parser *p, const char *name) {
    Var *v = lookup_var_local(p, name);
    if (v != NULL)
        v->is_synth = 1;
}

struct Var;
static Var *global_var_ref(Parser *p, struct GlobalDef *g);
static struct GlobalDef *find_global(Parser *p, const char *name);

/* Resolves a name to its binding, walking outward through the scopes still
 * open. `mark` records a use, which is right when something read the variable
 * and wrong when the caller was only asking which one a name means.
 *
 * The walk stops at the enclosing function's body scope, because a function body
 * is a frame of its own and continuing past it would let a function read a
 * variable declared beside it: `var g = 5; int f() { return g; }` used to
 * compile, and returned 0, because slot numbers are assigned per function and f
 * read whatever f's own frame happened to hold. */
static Var *resolve_var(Parser *p, const char *name, int mark) {
    /* Within one scope an `lvar` outranks a plain `var` of the same name, so a
     * scope can hold both and the plain one is only reachable as `var(x)`.
     *
     * Two passes over the same list rather than one that remembers a candidate:
     * the list is a prepend list, so a plain `var` declared *after* an `lvar`
     * is found first, and "first match wins" would give the wrong answer in that
     * order and the right one in the other. `var what = 2; lvar what = 1;` and
     * `lvar what = 1; var what = 2;` have to mean the same thing. */
    Var *plain = NULL;
    for (Scope *s = p->scope; s != NULL; s = s->parent) {
        for (Var *v = s->vars; v != NULL; v = v->next) {
            if (strcmp(v->name, name) != 0)
                continue;
            if (v->is_lvar) {
                if (mark)
                    v->used = 1;
                return v;
            }
            if (plain == NULL)
                plain = v;
        }
        if (plain != NULL) {
            if (mark)
                plain->used = 1;
            return plain;
        }
        if (s->is_fn_body)
            break;
    }

    /* Past the function body the scope walk deliberately stops, because a value
     * declared outside a function lives in a different frame. A global is the
     * exception that proves the rule: it has its own storage rather than
     * anyone's frame, so naming one from inside a function is reading what it
     * says rather than reading a slot that happens to hold a number. */
    GlobalDef *g = find_global(p, name);
    if (g == NULL)
        g = ns_lookup(p, name, look_global);
    if (g != NULL) {
        Var *v = global_var_ref(p, g);
        if (v != NULL && mark)
            v->used = 1;
        return v;
    }
    return NULL;
}

static Var *lookup_var(Parser *p, const char *name) {
    /* Resolving the name is a use. lookup_var_local deliberately does not, since
     * it also serves the redeclaration check, where merely testing for a name is
     * not a use. */
    return resolve_var(p, name, 1);
}

static int align_up(int n, int a) {
    if (a <= 0)
        return n;
    return (n + a - 1) / a * a;
}

/* Every name a reader could have meant at the point they wrote one: the
 * variables in scope, the constants, the functions and the type names.
 *
 * "undefined variable 'gt'" is true and useless on its own -- the reader has a
 * name they typed one character away and no way to learn what it was. This
 * collects the candidates so diag_suggest can find it. The list is bounded and
 * deduplicated by `out_used` because the same function is a candidate for both
 * an undefined variable and an undefined type, and a repeated entry can only
 * make the distance look better than it is. */
/* Candidate names for a "did you mean", split by what kind of name the reader
 * was reaching for.
 *
 * The split is the point. Offering `char_at` when someone wrote `char` in type
 * position, or `len` when they wrote `Vec`, is worse than saying nothing: the
 * name is *there* in the message, and it is not a type, so the reader's next
 * move is to wonder what is wrong with the compiler. Each list holds only names
 * that would actually have worked in the position. */
typedef enum { SK_VALUE, SK_TYPE, SK_FUNC } SuggKind;

#define MAX_SUGGEST 512
static int collect_names(Parser *p, SuggKind kind, const char **out, int cap) {
    int n = 0;
    /* The type and value lists are disjoint, and the function list gets a
     * reserved tail so the built-ins always fit. */
    int limit = cap - 24;
    if (kind == SK_FUNC) {
        for (int i = 0; i < p->nsigs && n < limit; i++) {
            if (p->sigs[i].name[0] != '$')
                out[n++] = p->sigs[i].name;
        }
        /* The built-in names are candidates too, and they live in no table the
         * parser owns -- they are the two macros in z_limits.h, reached through
         * the same lookup a call site uses. */
#define Z_ADD_BUILTIN(nm, sym, ret, ps)                                                            \
    if (n < cap)                                                                                   \
        out[n++] = nm;
        Z_BUILTIN_LIST(Z_ADD_BUILTIN)
#undef Z_ADD_BUILTIN
#define Z_ADD_INTRIN(nm, k)                                                                        \
    if (n < cap)                                                                                   \
        out[n++] = nm;
        Z_INTRINSIC_LIST(Z_ADD_INTRIN)
#undef Z_ADD_INTRIN
        /* Local functions are registered under a mangled name; the name as
         * written is what the reader can type, so recover it. */
        for (LocalFn *f = p->local_fns; f != NULL && n < cap; f = f->next)
            out[n++] = f->name;
        return n;
    }
    if (kind == SK_VALUE) {
        for (Scope *s = p->scope; s != NULL; s = s->parent) {
            for (Var *v = s->vars; v != NULL && n < limit; v = v->next) {
                /* The desugarer's own temporaries are not suggestions: nobody
                 * meant to type `$env`. */
                if (v->name == NULL || v->name[0] == '$' || v->name[0] == '\0')
                    continue;
                out[n++] = v->name;
            }
            if (s->is_fn_body)
                break;
        }
        for (int i = 0; i < p->nconsts && n < limit; i++)
            out[n++] = p->consts[i].name;
    } else {
        for (int i = 0; i < p->ty->nstructs && n < limit; i++)
            out[n++] = p->ty->structs[i]->name;
        for (int i = 0; i < p->ty->nunion && n < limit; i++)
            out[n++] = p->ty->unions[i]->name;
        for (int i = 0; i < p->ty->niface && n < limit; i++)
            out[n++] = p->ty->ifaces[i]->name;
    }
    return n;
}

/* Queues the note for the one type mismatch a reader cannot work out alone: a
 * class value is a pointer, so `Foo` and `Foo*` are different types and using the
 * wrong one reports a bare "cannot assign 'Counter*' to 'Counter'".
 *
 * The note names the fix, because the rule is a convention rather than a rule the
 * type system states: a class is a reference, so a field, a parameter or a local
 * holding one is declared `Counter*`. A method's *return* type does not need the
 * star -- the parser supplies it, since a method cannot return a class by value
 * -- which is the asymmetry that makes this one worth a sentence. */
static void note_missing_class_pointer(Parser *p, Type *want, Type *got) {
    (void)p; /* nothing here is type-name dependent yet; kept for the signature */
    if (want == NULL || got == NULL)
        return;
    if (!is_kind(want, TK_STRUCT) || want->sdef == NULL || !want->sdef->is_class)
        return;
    if (!is_kind(got, TK_PTR) || got->base != want)
        return;
    diag_note("'%s' is a class, so a value of it is a pointer; declare it as '%s*'",
              want->sdef->name, want->sdef->name);
}

/* Queues the "did you mean" note for a name the compiler could not resolve.
 * `what` is the word the note uses for the kind of thing. */
static void suggest_in_scope(Parser *p, SuggKind kind, const char *what, const char *name) {
    const char *cands[MAX_SUGGEST];
    int n = collect_names(p, kind, cands, MAX_SUGGEST);
    diag_suggest_note(what, name, cands, n);
}

/* The struct behind a type that may or may not be a pointer to one, or NULL. */
static StructDef *sdef_of(Type *t) {
    if (is_kind(t, TK_STRUCT))
        return t->sdef;
    if (is_kind(t, TK_PTR) && is_kind(t->base, TK_STRUCT))
        return t->base->sdef;
    return NULL;
}

/* Every name reachable with `.` on a type: fields, auto-property backing
 * fields, and methods. A member lookup that fails is the other place a reader
 * is one keystroke from the right answer, and the list is short enough that
 * offering the whole set is more useful than picking one. */
#define MAX_MEMBERS 256
static int collect_member_names(StructDef *sd, const char **out, int cap) {
    int n = 0;
    if (sd == NULL)
        return 0;
    for (int i = 0; i < sd->nfields && n < cap; i++)
        out[n++] = sd->fields[i].name;
    for (int i = 0; i < sd->nprops && n < cap; i++)
        out[n++] = sd->props[i].name;
    for (int i = 0; i < sd->nmethods && n < cap; i++)
        out[n++] = sd->methods[i]->name;
    return n;
}

static void suggest_member(StructDef *sd, const char *what, const char *name) {
    const char *cands[MAX_MEMBERS];
    int n = collect_member_names(sd, cands, MAX_MEMBERS);
    diag_suggest_note(what, name, cands, n);
}

/* The symbol a call to `name` should reach. A function declared inside another
 * one was emitted under a mangled name, and its signature is filed under that
 * same mangled name, so both the lookup and the emitted call have to go through
 * here. Anything not declared locally keeps the name the program wrote. */
static const char *resolve_fn_sym(Parser *p, const char *name) {
    for (LocalFn *f = p->local_fns; f != NULL; f = f->next)
        if (strcmp(f->name, name) == 0)
            return f->sym;
    return name;
}

/* Marks a just-declared local as a parameter rather than a local statement.
 *
 * The distinction is not cosmetic, and it is the reason a parameter is not torn
 * down when the function returns: a parameter is a borrow, so the value behind
 * it is still the caller's and the callee's teardown must not destroy it. A
 * local is the opposite -- the scope ending is what frees it.
 *
 * `check_return_of_owned_local` depends on that: `return p;` hands back
 * something this function was never going to destroy, so it is allowed, while
 * `return mine;` hands back a local whose destructor is about to run and is a
 * diagnostic. Nothing set this flag before, so the exemption never fired and
 * every parameter of a destructor-bearing class was diagnosed as though it were
 * a local about to be torn down -- the exact failure the flag was written to
 * prevent, on the one case where it does not apply. */
static void mark_param(Parser *p, const char *name) {
    Var *v = lookup_var_local(p, name);
    if (v != NULL)
        v->is_param = 1;
}

static int declare_var(Parser *p, const char *name, Type *type) {
    /* Shadowing an outer variable in a nested block is legal (as in C#); only
     * redeclaring within the same scope is an error. */
    Var *existing = lookup_var_local(p, name);
    /* `lvar` and `var` of one name in one scope are two declarations, not a
     * duplicate: the whole point is that the `lvar` is the one the name resolves
     * to, and the plain `var` is still reachable as `var(x)`. Refusing the pair
     * would make the disambiguation syntax unreachable.
     *
     * So the check is on the tier rather than on the name: two declarations
     * collide only when they are the same kind. The order they are written in
     * does not matter, which is what `resolve_var` needs too. */
    int same_tier = existing != NULL && existing->is_lvar == p->declaring_lvar;
    if (same_tier) {
        diag_error(cur(p)->span, "variable '%s' is already declared in this scope", name);
        return -1;
    }
    /* Shadowing an outer variable in a nested block is legal; it is also almost
     * always a mistake, because the inner value is what the rest of the block
     * sees and the outer one silently stops evolving. */
    /* Not a global: a plain `var` in front of a `gvar` is the documented
     * relationship between the two tiers, not a nested shadow someone should be
     * told off for. shadowed-local is about a *block* shadowing something around
     * it, which is where the mistake actually happens. */
    /* Not the other tier in the same scope, either: `var x` beside `lvar x` is
     * the documented pairing, and telling someone off for writing the two the
     * feature is named after is noise. */
    Var *shadow = lookup_var(p, name);
    if (warn_enabled(W_SHADOWED_LOCAL) && shadow != NULL && !shadow->is_global &&
        !(shadow->is_lvar != p->declaring_lvar && shadow->owner == p->cur_owner))
        diag_warn(W_SHADOWED_LOCAL, cur(p)->span,
                  "'%s' shadows a declaration in an enclosing scope", name);
    /* Locals live inline in the frame, growing *upward* from their base but
     * placed *below* rbp-8 so a multi-byte value never overwrites the saved
     * frame pointer / return address. A value of `size` bytes is anchored so
     * its top byte is just below the current high-water mark. */
    int size = type_size(type);
    if (size < 8)
        size = 8;
    int align = type_align(type);
    if (align < 8)
        align = 8;
    p->next_offset = align_up(p->next_offset, align);
    int base = 8 + p->next_offset + size; /* value base = rbp - base */
    p->next_offset += size;
    Var *v = arena_alloc(p->arena, sizeof *v);
    v->name = arena_strdup(p->arena, name);
    v->type = type;
    v->offset = base;
    /* Stamped so a reference from inside a lambda can tell, by ownership alone,
     * whether this is one of its own locals or something it captured. Without
     * the stamp every variable looks captured -- the lambda's own parameters
     * included, which is how the first version ended up building an environment
     * out of the parameters it had just been given. */
    v->owner = p->cur_owner;
    v->captured = 0;
    v->is_param = 0;
    v->is_synth = 0;
    v->decl_span = cur(p)->span;
    v->used = 0;
    /* A value that owns something is used by existing: the destructor call at
     * the end of its scope is a real use of the variable, and a program that
     * declares a value purely to be destroyed would otherwise be told it is
     * dead code. */
    /* Whether this owns something is decided when the scope closes, not here.
     *
     * A type can be incomplete at the point a local is declared: a class body
     * is parsed in a later pass than the functions that mention it, so a
     * `var t = new T()` written before `class T` has a destructor registered
     * yet. Deciding here made the answer depend on declaration order, which is
     * exactly the kind of thing that is correct in one test and wrong in
     * another. At the end of the scope every type the function mentioned is
     * complete, so the question has one answer. */
    v->needs_drop = 0;
    {
        Scope *sc = p->scope;
        if (sc->ndrops == sc->drop_cap) {
            int ncap = sc->drop_cap == 0 ? 4 : sc->drop_cap * 2;
            Var **nd = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Var *));
            if (sc->drops != NULL)
                memcpy(nd, sc->drops, (size_t)sc->ndrops * sizeof(Var *));
            sc->drops = nd;
            sc->drop_cap = ncap;
        }
        /* Recorded in declaration order; `build_drops` walks it backwards. */
        sc->drops[sc->ndrops++] = v;
    }
    v->next = p->scope->vars;
    p->scope->vars = v;
    return base;
}

/* Consumes a `{ .. }` block without parsing it, so a construct that has already
 * been reported as malformed does not cascade into an error for every name
 * inside its body. Used on the error path only. */
static void skip_braced_block(Parser *p) {
    if (!at(p, T_LBRACE))
        return;
    int depth = 0;
    do {
        if (at(p, T_LBRACE))
            depth++;
        else if (at(p, T_RBRACE))
            depth--;
        else if (at(p, T_EOF))
            return;
        advance(p);
    } while (depth > 0);
}
