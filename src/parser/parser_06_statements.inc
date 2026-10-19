/* ---- statement parsing ---- */

/* Statement terminators are optional: a trailing ';' is consumed when one is
 * written, and a missing one is fine. Statements already end where their
 * syntax ends, so `var x = 5` followed by a newline parses as one declaration
 * whether or not a ';' is written between them. A ';' remains useful to show
 * where a statement ends -- e.g. two statements on one line. */
static void expect_semi(Parser *p) { match(p, T_SEMI); }

/* Registers a compile-time constant. */
static void const_define(Parser *p, char *name, Type *ty, long long ival, int str_id) {
    for (int i = p->nconsts - 1; i >= 0; i--) {
        if (strcmp(p->consts[i].name, name) == 0) {
            diag_error(p->toks[p->pos].span, "const '%s' is already defined", name);
            return;
        }
    }
    if (p->nconsts == p->const_cap) {
        int ncap = p->const_cap ? p->const_cap * 2 : 8;
        ConstDef *nc = arena_alloc_array(p->arena, ncap, sizeof(ConstDef));
        if (p->nconsts > 0)
            memcpy(nc, p->consts, p->nconsts * sizeof(ConstDef));
        p->consts = nc;
        p->const_cap = ncap;
    }
    p->consts[p->nconsts].name = name;
    p->consts[p->nconsts].type = ty;
    p->consts[p->nconsts].ival = ival;
    p->consts[p->nconsts].str_id = str_id;
    p->nconsts++;
}

static ConstDef *const_find(Parser *p, const char *name) {
    for (int i = p->nconsts - 1; i >= 0; i--)
        if (strcmp(p->consts[i].name, name) == 0)
            return &p->consts[i];
    return NULL;
}

/* Materialises a const reference as a literal expression, so consts cost
 * nothing at run time and can be used in constant expressions. */
static Expr *const_expr(Parser *p, ConstDef *c, Span span) {
    Expr *e = new_expr(
        p, is_kind(c->type, TK_STRING) ? E_STRING : (is_kind(c->type, TK_BOOL) ? E_BOOL : E_INT),
        span);
    e->ival = c->ival;
    e->str_id = c->str_id;
    e->type = c->type;
    return e;
}

/* Folds an expression that is built only from literals, so `const` initializers
 * can be written as arithmetic rather than forcing a pre-computed number. */
static int const_fold_static(Parser *p, Expr *e, long long *out) {
    if (e == NULL)
        return 0;
    long long a, b;
    switch (e->kind) {
    case E_INT:
    case E_BOOL:
        *out = e->ival;
        return 1;
    case E_UNARY:
        if (!const_fold_static(p, e->lhs, &a))
            return 0;
        if (e->op == T_MINUS) {
            *out = -a;
            return 1;
        }
        if (e->op == T_TILDE) {
            *out = ~a;
            return 1;
        }
        if (e->op == T_NOT) {
            *out = !a;
            return 1;
        }
        return 0;
    case E_BINARY:
        if (!const_fold_static(p, e->lhs, &a) || !const_fold_static(p, e->rhs, &b))
            return 0;
        switch (e->op) {
        case T_PLUS:
            *out = a + b;
            return 1;
        case T_MINUS:
            *out = a - b;
            return 1;
        case T_STAR:
            *out = a * b;
            return 1;
        case T_SLASH:
            if (b == 0)
                return 0;
            *out = a / b;
            return 1;
        case T_PERCENT:
            if (b == 0)
                return 0;
            *out = a % b;
            return 1;
        case T_AMP:
            *out = a & b;
            return 1;
        case T_PIPE:
            *out = a | b;
            return 1;
        case T_CARET:
            *out = a ^ b;
            return 1;
        case T_SHL:
            if (b < 0 || b > 63)
                return 0;
            *out = a << b;
            return 1;
        case T_SHR:
            if (b < 0 || b > 63)
                return 0;
            *out = a >> b;
            return 1;
        case T_EQ:
            *out = a == b;
            return 1;
        case T_NE:
            *out = a != b;
            return 1;
        case T_LT:
            *out = a < b;
            return 1;
        case T_LE:
            *out = a <= b;
            return 1;
        case T_GT:
            *out = a > b;
            return 1;
        case T_GE:
            *out = a >= b;
            return 1;
        case T_AND:
            *out = a && b;
            return 1;
        case T_OR:
            *out = a || b;
            return 1;
        default:
            return 0;
        }
    case E_TERNARY:
        if (!const_fold_static(p, e->lhs, &a))
            return 0;
        if (a) {
            if (!const_fold_static(p, e->rhs, &b))
                return 0;
        } else {
            if (!const_fold_static(p, e->args[0], &b))
                return 0;
        }
        *out = b;
        return 1;
    default:
        return 0;
    }
}

/* `const T NAME = <compile-time constant>;`
 *
 * The initializer must fold to a constant; anything else is rejected so a
 * const can never quietly become a runtime value. Like functions and types,
 * a const is visible from its declaration onward, so it must be declared
 * before use. */
static Stmt *parse_const_decl(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* const */
    Type *ty = parse_type(p);
    if (ty == NULL) {
        diag_error(cur(p)->span, "const needs a type");
        while (!at(p, T_SEMI) && !at(p, T_EOF) && !at(p, T_RBRACE))
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, start);
    }
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected const name");
        while (!at(p, T_SEMI) && !at(p, T_EOF) && !at(p, T_RBRACE))
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, start);
    }
    char *name = ns_join(p, ns_here(p), cur(p)->text);
    Span name_span = cur(p)->span;
    advance(p);
    if (!match(p, T_ASSIGN)) {
        diag_error(cur(p)->span, "const '%s' needs an initializer", name);
        while (!at(p, T_SEMI) && !at(p, T_EOF) && !at(p, T_RBRACE))
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, start);
    }
    Expr *init = parse_expr(p);
    expect_semi(p);

    if (!is_unk(ty) && !is_unk(init->type) && !type_equals(ty, init->type)) {
        diag_error(name_span, "const '%s' is '%s' but the initializer is '%s'", name,
                   type_name(p->ty, ty), type_name(p->ty, init->type));
        return new_stmt(p, S_EXPR, start);
    }
    /* Fold the initializer; only constants are allowed. */
    if (init->kind == E_INT || init->kind == E_BOOL) {
        const_define(p, name, ty, init->ival, 0);
    } else if (init->kind == E_STRING) {
        const_define(p, name, ty, 0, init->str_id);
    } else if (init->kind == E_BINARY || init->kind == E_UNARY) {
        long long v;
        if (const_fold_static(p, init, &v)) {
            const_define(p, name, ty, v, 0);
        } else {
            diag_error(name_span, "const '%s' needs a compile-time constant initializer", name);
        }
    } else {
        diag_error(name_span, "const '%s' needs a compile-time constant initializer", name);
    }
    return new_stmt(p, S_EXPR, start);
}

/* ---- gvar / lvar: the `g`- and `l`-prefixed declarations ----
 *
 * A declaration is spelled `<prefix><TypeName> <name> = <init>;`, where the
 * prefix is `g` or `l` and the type word is any type name the compiler knows:
 * `var`, `auto`, `int`, `bool`, `string`, `float`, or a class or struct the
 * project declared. So `gvar`, `lvar`, `gint`, `lint`, `gPoint` are all one
 * rule rather than a list of keywords.
 *
 * They are lexed as ordinary identifiers, which is what keeps the list open --
 * a new keyword per user type would be impossible, and `gFoo` for a `Foo` the
 * compiler has not seen yet is the only way the spelling can work at all.
 *
 * What tells a prefixed declaration apart from an expression statement is the
 * token after the name: `gvar why = 1;` is IDENT IDENT `=`, and no expression
 * in Z is two identifiers in a row. `gvar(why)` -- the disambiguation form -- is
 * IDENT `(`, so it is not mistaken for one, which is convenient rather than
 * lucky.
 */
static int at_prefixed_decl(Parser *p) {
    if (!at(p, T_IDENT))
        return 0;
    const char *t = cur(p)->text;
    if (t[0] != 'g' && t[0] != 'l')
        return 0;
    if (t[1] == '\0')
        return 0;
    return peek(p, 1)->kind == T_IDENT;
}

/* The type a prefixed declaration names, after the prefix. `infer` is set for
 * `var` and `auto`, which take the type from the initializer the way a plain
 * `var` does. A name that is neither a built-in nor a declared type is left to
 * the caller's type check rather than diagnosed here: `gFoo x = 1;` in a file
 * that has not declared `Foo` should say the type is unknown, which is what
 * parse_type would have said for `Foo x = 1;`. */
static Type *prefixed_type(Parser *p, const char *word, int *infer) {
    if (strcmp(word, "var") == 0 || strcmp(word, "auto") == 0) {
        *infer = 1;
        return NULL;
    }
    if (strcmp(word, "int") == 0)
        return type_int(p->ty);
    if (strcmp(word, "bool") == 0)
        return type_bool(p->ty);
    if (strcmp(word, "string") == 0)
        return type_string(p->ty);
    if (strcmp(word, "float") == 0)
        return type_f64(p->ty);
    Type *st = type_find_struct(p->ty, word);
    if (st == NULL)
        st = type_find_struct(p->ty, surf(p, word));
    if (st != NULL)
        return st;
    Type *un = type_find_union(p->ty, word);
    if (un == NULL)
        un = type_find_union(p->ty, surf(p, word));
    return un;
}

/* True when the expression is something a constant initializer can be built
 * from, which decides whether the global's storage can be filled at compile
 * time or needs the init function.
 *
 * Only literals and the operators over them. Not a string concatenation and not
 * an enum access, though both are constants a C compiler would fold: this is the
 * shape a single machine instruction can encode, and anything wider is easier to
 * reason about at run time than to convince the reader it was folded. */
static int init_is_constant(Expr *e) {
    if (e == NULL)
        return 1;
    switch (e->kind) {
    case E_INT:
    case E_F64:
    case E_BOOL:
    case E_NULL:
        return 1;
    /* Deliberately not E_STRING. A Z string is a `{len, cap}` header followed by
     * the bytes, and the header is written by the runtime's string constructor --
     * a literal is an instruction to build one, not a pointer to a finished
     * value. Emitting the pointer as data gave a global whose length was zero, so
     * it printed as the empty string however long the literal was. It goes
     * through z_ginit like any other constructed value. */
    default:
        return 0;
    }
}

/* Registers a `gvar`. Returns the Var that stands for it, or NULL if the name is
 * already taken -- a second declaration of the same global would otherwise emit
 * two symbols and let the assembler pick, which is not a decision to leave to
 * the assembler. */
static Var *declare_global(Parser *p, const char *name, Type *type, Expr *init, Span span) {
    for (GlobalDef *g = p->globals; g != NULL; g = g->next) {
        if (strcmp(g->name, name) == 0) {
            /* Naming the file is the whole point of the diagnostic once a
             * project has more than one: a name declared twice in one file is a
             * mistake in a paragraph, and the same name in two files is a
             * disagreement about what it means. The span carries the path, so
             * there is nothing to keep in sync. */
            diag_error_code(span, "duplicate_global", "global '%s' is already declared at %s:%d",
                            name, g->file != NULL ? g->file : "?", g->decl_span.line);
            return NULL;
        }
    }

    GlobalDef *g = arena_alloc(p->arena, sizeof *g);
    g->name = arena_strdup(p->arena, name);
    g->type = type;
    g->init = init;
    g->decl_span = span;
    g->file = span.file;
    g->init_is_const = init_is_constant(init);
    /* Prepended, so the list runs newest-first; gidx counts the other way so the
     * emitted table is in declaration order. Two orders for one list would be a
     * mistake waiting to happen, so the index is what everything else uses. */
    g->gidx = p->nglobals++;
    g->next = p->globals;
    p->globals = g;

    Var *v = arena_alloc(p->arena, sizeof *v);
    memset(v, 0, sizeof *v);
    v->name = g->name;
    v->type = type;
    v->is_global = 1;
    v->offset = -1; /* not a frame slot; the symbol comes from the GlobalDef */
    v->decl_span = span;
    v->used = 1; /* a global is not a local, so -Wunused-local never applies */
    return v;
}

/* The global named `name`, or NULL. Kept separate from resolve_var so the
 * disambiguation forms and the ordinary lookup share one source of truth. */
static GlobalDef *find_global(Parser *p, const char *name) {
    for (GlobalDef *g = p->globals; g != NULL; g = g->next)
        if (strcmp(g->name, name) == 0)
            return g;
    return NULL;
}

/* The Var a GlobalDef is referred to by.
 *
 * One is built per GlobalDef and cached on it, because a use site has to be an
 * `E_VAR` to reach codegen and every other path -- register allocation, the drop
 * pass, the DWARF walk -- keys off `Var`. Handing each use site its own Var
 * would work and would also make `-Wunused-local` fire once per mention, so the
 * one binding is shared.
 *
 * The list is walked rather than the globals indexed, because there are no more
 * than a handful per program and an array of pointers would be a second thing to
 * keep in step. */
static Var *global_var_ref(Parser *p, GlobalDef *g) {
    if (g->ref != NULL)
        return g->ref;
    Var *v = arena_alloc(p->arena, sizeof *v);
    memset(v, 0, sizeof *v);
    v->name = g->name;
    v->type = g->type;
    v->is_global = 1;
    v->offset = -1;
    v->decl_span = g->decl_span;
    v->used = 1;
    g->ref = v;
    return v;
}

static Stmt *parse_var_decl(Parser *p);
static Stmt *parse_var_decl_kind(Parser *p, int prefix);

static Stmt *parse_var_decl(Parser *p) { return parse_var_decl_kind(p, 0); }

/* `prefix` is 0 for an ordinary `var`/`T name = ...` declaration, 'g' for a
 * global and 'l' for a function-local that outranks a plain `var`.
 *
 * The three share every rule below the type -- the assignability check, the
 * string ownership dance, the "needs an initializer" error -- because a global
 * that was checked less carefully than a local would be checked less carefully
 * than the only thing a local is for. Only the storage differs, and that is two
 * branches. */
static Stmt *parse_var_decl_kind(Parser *p, int prefix) {
    Span start = cur(p)->span;
    Type *declared = NULL;
    int infer = 0;
    if (prefix != 0) {
        const char *word = cur(p)->text + 1;
        advance(p);
        declared = prefixed_type(p, word, &infer);
        if (!infer && declared == NULL)
            diag_error(start, "unknown type '%s'", word);
    } else if (match(p, T_KW_VAR)) {
        infer = 1;
    } else {
        declared = parse_type(p);
    }
    /* `mut` is Rust-style explicit mutability: `var mut x = ...`,
     * `int mut x = ...`. Accepted and recorded; enforcement (immutable by
     * default) follows once the codebase is migrated. */
    if (at(p, T_KW_MUT))
        advance(p);

    if (at(p, T_IDENT)) {
        char *name = cur(p)->text;
        advance(p);
        if (!match(p, T_ASSIGN)) {
            diag_error(cur(p)->span, "declaration of '%s' needs an initializer", name);
            expect_semi(p);
            Stmt *s = new_stmt(p, S_VAR, start);
            s->name = name;
            s->type = declared;
            return s;
        }
        /* A declared type tells `Ok(...)`/`Err(...)` what to build. */
        Type *saved_expect = p->expect_result;
        p->expect_result = type_is_result(declared) ? declared : NULL;
        Expr *init = parse_expr(p);
        p->expect_result = saved_expect;
        Type *t = declared;
        if (infer)
            t = init->type;
        if (!infer && is_kind(t, TK_IFACE))
            init = to_iface(p, init, t, start);
        /* A written-out type is a claim about what the variable holds, and an
         * unchecked claim is worse than none: `int x = "hello";` compiled, and
         * reading `x` read the pointer's own bytes back as an integer.
         *
         * This is the same check an assignment makes, in the same order and with
         * the same two exceptions: an int widens to a float because that cannot
         * lose a value, and a value going to an interface boxes, which is the
         * conversion just above. `var` skips it, because there the type came from
         * the value and there is nothing to disagree with.
         *
         * An unknown type on either side means something upstream already failed,
         * and saying anything here would be a second complaint about one mistake. */
        if (!infer && !is_unk(t) && !is_unk(init->type)) {
            if (type_widens_to(t, init->type)) {
                init = make_cvt(p, t, init, start);
            } else if (!type_assignable(t, init->type)) {
                if (is_kind(t, TK_INT) && is_kind(init->type, TK_F64))
                    diag_error(start,
                               "cannot initialize 'int' with 'float' without losing precision; "
                               "write '(int)' if that is what you want");
                else {
                    note_missing_class_pointer(p, t, init->type);
                    diag_error_code(start, "type_mismatch", "cannot assign '%s' to '%s'",
                                    type_name(p->ty, init->type), type_name(p->ty, t));
                }
            }
        }
        init = own_string_copy(p, init);
        init = claim_string_value(init);
        if (t == NULL)
            /* A bare `null` carries no type, but `var p = null` is a pointer
             * declaration in every other language; default to int* so the
             * variable is assignable later. */
            t = init->kind == E_NULL ? type_ptr(p->ty, type_int(p->ty)) : type_int(p->ty);
        /* A global has no frame slot, so none of the frame machinery below it
         * applies. The initializer is kept on the GlobalDef instead and emitted
         * into the project's init function. */
        if (prefix == 'g') {
            declare_global(p, ns_join(p, ns_here(p), name), t, init, start);
            expect_semi(p);
            return new_stmt(p, S_BLOCK, start);
        }

        p->declaring_lvar = prefix == 'l';
        int slot = declare_var(p, name, t);
        p->declaring_lvar = 0;
        if (slot >= 0 && prefix == 'l') {
            /* Marked after the fact because declare_var is shared with `var`,
             * and the tier is a property of this one declaration. */
            Var *v = lookup_var_local(p, name);
            if (v != NULL)
                v->is_lvar = 1;
        }
        expect_semi(p);
        Stmt *s = new_stmt(p, S_VAR, start);
        s->name = name;
        s->type = t;
        s->slot = slot;
        s->init = init;
        return s;
    }

    diag_error(cur(p)->span, "expected variable name after declaration");
    expect_semi(p);
    return new_stmt(p, S_EXPR, start);
}

/* True when control cannot fall out of the bottom of `s`, so anything after it
 * in the same block is dead. An `if` counts only when it has an `else` and both
 * arms leave -- otherwise the untaken arm falls through. */
static int stmt_terminates(Stmt *s) {
    if (s == NULL)
        return 0;
    switch (s->kind) {
    case S_RETURN:
    case S_BREAK:
    case S_CONTINUE:
        return 1;
    case S_BLOCK:
        /* A block leaves only if its last statement does. */
        return s->nitems > 0 && stmt_terminates(s->items[s->nitems - 1]);
    case S_IF:
        return s->orelse != NULL && stmt_terminates(s->body) && stmt_terminates(s->orelse);
    default:
        return 0;
    }
}
