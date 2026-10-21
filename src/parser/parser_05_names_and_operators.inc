/* ---- naming one tier on purpose: `gvar(x)`, `var(x)`, `lvar(x)` ----
 *
 * With three tiers under one name, the priority rule answers almost every read
 * and hides the other two. `var what = 2; lvar what = 1;` means 1 everywhere in
 * that body, and there is no spelling that reaches the 2 -- which is fine until
 * the 2 is what you meant.
 *
 * So the three tier keywords are also names of their own when they are followed
 * by a parenthesis: `gvar(why)` is the global and only the global, `var(why)`
 * is the plain one, `lvar(why)` the local. It reads like a cast and is not one:
 * it is a way of saying which declaration, of three with one name, a line means.
 *
 * `var` is a keyword and `gvar`/`lvar` are not, so both token kinds are accepted
 * here. Without the parenthesis these are declarations, which is why the `(` is
 * what tells the two apart -- the same test that separates a prefixed
 * declaration from an expression, and for the same reason.
 */
static int at_tier_ref(Parser *p, int *prefix) {
    const char *t;
    if (at(p, T_IDENT))
        t = cur(p)->text;
    else if (at(p, T_KW_VAR))
        t = "var";
    else
        return 0;
    if (peek(p, 1)->kind != T_LPAREN)
        return 0;
    if (strcmp(t, "var") == 0)
        *prefix = 0;
    else if (strcmp(t, "lvar") == 0)
        *prefix = 'l';
    else if (strcmp(t, "gvar") == 0)
        *prefix = 'g';
    else
        return 0;
    return 1;
}

/* The binding `name` denotes in exactly one tier. NULL when there is none.
 *
 * The scope walk is the ordinary one with the other tiers skipped, rather than a
 * separate search, so a name reached from a nested block is reached the same way
 * a bare one is -- `gvar(x)` inside three blocks of a function still finds the
 * gvar, which is the only thing that makes the form worth having. */
static Var *resolve_tier(Parser *p, const char *name, int prefix, int mark) {
    if (prefix == 'g') {
        GlobalDef *g = find_global(p, name);
        if (g == NULL)
            g = ns_lookup(p, name, look_global);
        if (g == NULL)
            return NULL;
        Var *v = global_var_ref(p, g);
        if (mark)
            v->used = 1;
        return v;
    }
    for (Scope *sc = p->scope; sc != NULL; sc = sc->parent) {
        for (Var *v = sc->vars; v != NULL; v = v->next) {
            if (strcmp(v->name, name) != 0)
                continue;
            /* is_lvar is exactly "is an lvar", so the plain tier is its negation
             * -- and is_global covers neither, though a global has no scope to
             * be found in anyway. `prefix` is a character rather than a flag, so
             * it is asked as the question it stands for. */
            if (v->is_global || (v->is_lvar != 0) != (prefix == 'l'))
                continue;
            if (mark)
                v->used = 1;
            return v;
        }
        if (sc->is_fn_body)
            break;
    }
    return NULL;
}

static Expr *parse_tier_ref(Parser *p, int prefix) {
    Span start = cur(p)->span;
    advance(p); /* the keyword */
    advance(p); /* '(' */
    Span nspan = cur(p)->span;
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected a variable name inside '%s(...)'",
                   prefix == 'g' ? "gvar" : (prefix == 'l' ? "lvar" : "var"));
        expect_semi(p);
        return new_expr(p, E_INT, start);
    }
    char *name = cur(p)->text;
    advance(p);
    if (!match(p, T_RPAREN)) {
        diag_error(cur(p)->span, "expected ')' after '%s(%s'", prefix == 'g' ? "gvar" : "var",
                   name);
        while (at(p, T_IDENT) || at(p, T_RPAREN))
            advance(p);
        match(p, T_RPAREN);
        return new_expr(p, E_INT, start);
    }

    Var *v = resolve_tier(p, name, prefix, 1);
    if (v == NULL) {
        const char *what = prefix == 'g' ? "global" : (prefix == 'l' ? "lvar" : "variable");
        diag_error_code(nspan, "undefined_variable", "no %s named '%s' is in scope here", what,
                        name);
        Expr *e = new_expr(p, E_VAR, nspan);
        e->name = name;
        return e;
    }

    Expr *e = new_expr(p, E_VAR, nspan);
    e->name = name;
    if (v->is_global) {
        GlobalDef *g = find_global(p, v->name);
        e->gidx = g != NULL ? g->gidx : -1;
        e->slot = -1;
    } else {
        e->slot = v->offset;
    }
    e->type = v->type;
    e->agg_param = v->agg_param;
    return parse_postfix(p, e);
}

static Expr *parse_primary(Parser *p) {
    {
        int prefix = 0;
        if (at_tier_ref(p, &prefix))
            return parse_tier_ref(p, prefix);
    }
    Token *t = cur(p);
    switch (t->kind) {
    case T_INT: {
        advance(p);
        Expr *e = new_expr(p, E_INT, t->span);
        e->ival = t->ival;
        e->type = type_int(p->ty);
        return parse_postfix(p, e);
    }
    case T_F64: {
        advance(p);
        Expr *e = new_expr(p, E_F64, t->span);
        e->dval = t->dval;
        e->type = type_f64(p->ty);
        return parse_postfix(p, e);
    }
    case T_STRING: {
        advance(p);
        Expr *e = new_expr(p, E_STRING, t->span);
        e->str_id = t->str_id;
        e->type = type_string(p->ty);
        return parse_postfix(p, e);
    }
    case T_INTERP:
        advance(p);
        return parse_postfix(p, parse_interp(p, t));
    case T_KW_MATCH: {
        Expr *m = parse_match(p, t->span);
        return parse_postfix(p, m);
    }
    case T_KW_THIS: {
        /* `this` inside a class/struct method body refers to the receiver. */
        advance(p);
        if (p->cur_this == NULL) {
            diag_error(t->span, "'this' is only valid inside a method");
            return new_expr(p, E_INT, t->span);
        }
        Expr *th = new_expr(p, E_VAR, t->span);
        th->name = arena_strdup(p->arena, "this");
        th->slot = p->cur_this->slot;
        th->type = p->cur_this->type;
        return parse_postfix(p, th);
    }
    case T_KW_NULL: {
        advance(p);
        /* Type is deliberately unknown: the checker lets an unknown flow into
         * any pointer slot, and codegen materializes it as a literal 0. */
        return new_expr(p, E_NULL, t->span);
    }
    case T_KW_TRUE:
    case T_KW_FALSE: {
        advance(p);
        Expr *e = new_expr(p, E_BOOL, t->span);
        e->ival = t->kind == T_KW_TRUE;
        e->type = type_bool(p->ty);
        return e;
    }
    case T_IDENT: {
        char *name = t->text;
        Span span = t->span;
        /* A qualified surface name: `Console.WriteLog(...)`.
         *
         * Z has no namespaces, so `A.B(...)` is otherwise a member call on
         * something named `A`, and a name that is not a variable is an error
         * before the dot is ever reached. The check lives here, at the one point
         * where a name becomes a value, because that is the only place that can
         * tell an ordinary identifier from the first half of a qualified one. */
        {
            Expr *nq = parse_ns_qualified(p, name, span);
            if (nq != NULL)
                return nq;
            Expr *q = parse_qualified(p, name, span);
            if (q != NULL)
                return q;
        }
        /* A local holding a function pointer is called through the pointer, so
         * this is decided before the ordinary function lookup, which would
         * report the name as undefined. */
        /* lookup_var, not lookup_var_local: a function pointer held in an
         * enclosing scope -- a parameter, most often -- is just as callable. */
        Var *fplocal = lookup_var(p, name);
        /* Inside a lambda, a name that resolves to a variable belonging to an
         * enclosing function is a capture. Done here, at the single point where
         * a name becomes a variable, so no other path can produce one. */
        capture_for(p, fplocal, span);
        /* The identifier is still the current token here, so the '(' that
         * follows it is one token ahead and whatever comes after that is two. */
        /* A local holding a function pointer, a bound method or a closure is
         * called through the value. type_is_callable covers all three: a closure
         * and a function pointer of the same signature are interchangeable, and
         * a call site should not have to know which one it was handed. */
        int is_fp_call = peek(p, 1)->kind == T_LPAREN && peek(p, 2)->kind != T_KW_THIS &&
                         fplocal != NULL && type_is_callable(fplocal->type);
        advance(p);
        if (is_fp_call) {
            Var *lv = fplocal;
            {
                advance(p); /* '(' */
                Expr **args = NULL;
                int n = 0, cap = 0;
                if (!at(p, T_RPAREN)) {
                    for (;;) {
                        if (n == cap) {
                            int ncap = cap == 0 ? 4 : cap * 2;
                            Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                            if (args != NULL)
                                memcpy(na, args, (size_t)n * sizeof(Expr *));
                            args = na;
                            cap = ncap;
                        }
                        args[n++] = parse_expr(p);
                        if (!match(p, T_COMMA))
                            break;
                    }
                }
                if (!match(p, T_RPAREN))
                    diag_error(cur(p)->span, "expected ')' after arguments");
                Type *ft = lv->type;
                if (n != ft->nparams) {
                    diag_error(span, "'%s' expects %d argument%s but got %d", name, ft->nparams,
                               ft->nparams == 1 ? "" : "s", n);
                } else {
                    for (int i = 0; i < n; i++) {
                        if (!is_unk(args[i]->type) && !type_equals(args[i]->type, ft->ptypes[i]))
                            diag_error(args[i]->span,
                                       "argument %d of '%s' expects '%s' but got '%s'", i + 1, name,
                                       type_name(p->ty, ft->ptypes[i]),
                                       type_name(p->ty, args[i]->type));
                    }
                }
                Expr *callee = new_expr(p, E_VAR, span);
                callee->name = name;
                callee->slot = lv->offset;
                callee->type = lv->type;
                if (is_kind(ft, TK_MPTR) &&
                    (is_kind(ft->ret, TK_STRUCT) || is_kind(ft->ret, TK_UNION)))
                    diag_error(span, "a method pointer cannot return a struct");
                Expr *ic = new_expr(p, E_ICALL, span);
                ic->lhs = callee;
                ic->args = args;
                ic->nargs = n;
                ic->type = ft->ret;
                return parse_postfix(p, ic);
            }
        }
        if (at(p, T_LPAREN)) {
            /* A bare variant name is a union constructor: `Circle(5)`. */
            UnionDef *owner = NULL;
            VariantDef *vd = type_find_variant(p->ty, name, &owner);
            if (vd != NULL)
                return parse_variant_call(p, vd, owner, name, span);
            return parse_postfix(p, parse_call(p, name, span, 0));
        }
        Var *v = lookup_var(p, name);
        if (v == NULL) {
            /* A field-less variant can be written bare: `Empty`. */
            UnionDef *owner0 = NULL;
            VariantDef *vd0 = type_find_variant(p->ty, name, &owner0);
            if (vd0 != NULL && vd0->nfields == 0) {
                Expr *eu = new_expr(p, E_UNIONLIT, span);
                eu->variant = vd0;
                eu->type = type_find_union(p->ty, owner0->name);
                return parse_postfix(p, eu);
            }
        }
        if (v == NULL) {
            /* Inside a struct method, an unqualified field name resolves
             * to `this.<name>`. */
            if (p->cur_msd != NULL && p->cur_this != NULL) {
                Field *fld = struct_find_field(p->cur_msd, name);
                if (fld != NULL) {
                    Expr *fe = new_expr(p, E_FIELD, span);
                    fe->lhs = p->cur_this;
                    fe->name = name;
                    fe->field_off = fld->offset;
                    fe->type = fld->type;
                    return parse_postfix(p, fe);
                }
                /* A get-only property behaves like a field read. */
                StructMethod *pm = struct_find_method(p->cur_msd, name);
                if (pm != NULL && pm->nparams == 0) {
                    char *mang = arena_alloc(p->arena, strlen(p->cur_msd->name) + strlen(name) + 3);
                    snprintf(mang, strlen(p->cur_msd->name) + strlen(name) + 3, "%s__%s",
                             p->cur_msd->name, name);
                    Expr *call = new_expr(p, E_CALL, span);
                    call->name = mang;
                    Expr **a = arena_alloc_array(p->arena, 1, sizeof(Expr *));
                    a[0] = p->cur_this;
                    call->args = a;
                    call->nargs = 1;
                    call->type = pm->ret;
                    return call;
                }
            }
            {
                ConstDef *c = const_find(p, name);
                if (c == NULL)
                    c = ns_lookup(p, name, look_const);
                if (c != NULL)
                    return parse_postfix(p, const_expr(p, c, span));
            }
            suggest_in_scope(p, SK_VALUE, "variable", name);
            diag_error_code(span, "undefined_variable", "undefined variable '%s'", name);
            Expr *e = new_expr(p, E_VAR, span);
            e->name = name;
            e->type = NULL;
            return e;
        }
        Expr *e = new_expr(p, E_VAR, span);
        e->name = name;
        /* A global has no frame offset, so the index into the emitted table
         * travels instead and codegen reads storage by symbol. The offset stays
         * -1, which every local-only path already treats as "not a local". */
        if (v->is_global) {
            /* v->name is the definition's own spelling -- qualified when the
             * declaration lives in a namespace -- which is what the table is
             * keyed by. Looking the written name up instead read globals[-1]
             * for every unqualified use beside its namespace. */
            GlobalDef *g = find_global(p, v->name);
            e->gidx = g != NULL ? g->gidx : -1;
            e->slot = -1;
        } else {
            e->slot = v->offset;
        }
        e->type = v->type;
        e->agg_param = v->agg_param;
        /* Reading a value that `move` already took. The slot itself is null by
         * now, so this would otherwise be a read of null that surfaces as a
         * crash somewhere else entirely. */
        if (v->is_moved) {
            /* Notes are queued and claimed by the next error, so they go first. */
            diag_note_at(v->move_span, "'%s' was moved here", name);
            diag_note_at(span, "assign into '%s' instead, or write 'move %s' to hand it on again",
                         name, name);
            diag_error_code(span, "use_after_move", "'%s' has been moved from", name);
        }
        return parse_postfix(p, e);
    }
    case T_KW_NEW: {
        return parse_postfix(p, parse_new(p));
    }
    case T_LPAREN: {
        /* A lambda: a parameter list followed by `=>`. Checked before the cast
         * form, which also starts with a type keyword, and before the plain
         * grouped-expression form, which this is a refinement of. */
        if (lambda_ahead(p, p->pos) > 0) {
            Expr *lam = parse_lambda(p, t->span);
            return parse_postfix(p, lam);
        }
        /* `(float)x` and `(int)f`: a cast, which is how a value crosses between
         * the two numeric types in the direction that is never implicit. A '('
         * followed by anything that is not a type keyword is just a grouped
         * expression, so the two are told apart by looking at the next token. */
        if (is_kind(base_type_from_token(p, peek(p, 1)->kind), TK_INT) ||
            is_kind(base_type_from_token(p, peek(p, 1)->kind), TK_F64)) {
            Span cspan = cur(p)->span;
            Type *to = base_type_from_token(p, peek(p, 1)->kind);
            advance(p); /* '(' */
            advance(p); /* the type */
            if (!match(p, T_RPAREN))
                diag_error(cur(p)->span, "expected ')' after a cast type");
            /* parse_unary, not parse_expr: a cast binds like a unary operator, so
             * `(float)-x` and `-(float)x` both mean what they look like. */
            Expr *inner = parse_unary(p);
            Expr *c = make_cvt(p, to, inner, cspan);
            if (c == inner && !is_unk(inner->type) && !type_equals(inner->type, to))
                diag_error(cspan, "cannot cast '%s' to '%s'", type_name(p->ty, inner->type),
                           type_name(p->ty, to));
            return parse_postfix(p, c);
        }
        advance(p);
        /* A parenthesized expression is the deepest way to nest: every level
         * costs a parenthesis, a call and a scope, and the whole nest is
         * parsed before the first one is finished. It is bounded like every
         * other descent. */
        if (!enter_depth(p)) {
            /* The rest of the nest is skipped rather than parsed, so the
             * closing parentheses are consumed and the enclosing levels find
             * their own ')' where they expect it. */
            skip_past_bracket(p);
            Expr *e = new_expr(p, E_INT, t->span);
            e->ival = 0;
            e->type = type_int(p->ty);
            return parse_postfix(p, e);
        }
        Expr *inner = parse_expr(p);
        leave_depth(p);
        if (!match(p, T_RPAREN)) {
            diag_error(cur(p)->span, "expected ')' after expression");
        }
        /* Allow postfix (method calls, indexing) on a parenthesized value. */
        return parse_postfix(p, inner);
    }
    default:
        diag_error(t->span, "expected expression but found %s", token_kind_name(t->kind));
        advance(p);
        return new_expr(p, E_INT, t->span);
    }
}

static Expr *parse_unary(Parser *p) {
    Token *t = cur(p);
    /* `move x` hands the value on rather than sharing it. It takes a unary
     * operand, like `!x` and `-x` do, so `move a + b` moves `a` and then adds,
     * and `f(move x)` moves into the argument. */
    if (t->kind == T_KW_MOVE) {
        advance(p);
        /* A chain of `move`s is as deep as the expression it wraps, and it
         * recurses to get there, so it is bounded like every other descent. */
        if (!enter_depth(p)) {
            Expr *e = new_expr(p, E_MOVE, t->span);
            e->type = type_int(p->ty);
            return e;
        }
        Expr *operand = parse_unary(p);
        leave_depth(p);
        Expr *e = new_expr(p, E_MOVE, t->span);
        e->lhs = operand;
        e->type = operand->type;
        /* Only a storage location can be given up. `move f()` has nothing to
         * give up, because the call already produced a value nothing else holds,
         * and `move 3` is not a location at all. Both are diagnosed rather than
         * quietly accepted, because the reader wrote `move` expecting it to mean
         * something.
         *
         * A field or an element counts, and clearing it is what makes that
         * sound: `StringBuilder.take()` moves its buffer out of the object, so
         * the object has to stop holding it rather than keep a claim the caller
         * is now acting on. */
        if (operand->kind == E_VAR && operand->name != NULL) {
            Var *v = lookup_var(p, operand->name);
            if (v != NULL) {
                v->is_moved = 1;
                v->move_span = t->span;
            }
        } else if (operand->kind != E_FIELD && operand->kind != E_INDEX && !is_unk(operand->type)) {
            diag_note_at(
                operand->span,
                "'move' takes the value *out of* a variable, so it cannot be applied "
                "to a call or a literal: those already produce a value nothing else holds");
            diag_error_code(t->span, "move_non_value",
                            "'move' needs a variable to take the value from");
        }
        return e;
    }
    if (t->kind == T_MINUS || t->kind == T_NOT || t->kind == T_TILDE) {
        advance(p);
        /* A chain of unary operators recurses to its end, so it is bounded
         * like every other descent. */
        if (!enter_depth(p)) {
            Expr *e = new_expr(p, E_UNARY, t->span);
            e->op = t->kind;
            e->type = type_int(p->ty);
            return e;
        }
        Expr *operand = parse_unary(p);
        leave_depth(p);
        /* Fold unary minus / logical-not / bitwise-not on literals. */
        if (t->kind == T_TILDE && operand->kind == E_INT) {
            Expr *f = new_expr(p, E_INT, t->span);
            f->ival = ~operand->ival;
            f->type = type_int(p->ty);
            return f;
        }
        if (t->kind == T_MINUS && operand->kind == E_INT) {
            Expr *f = new_expr(p, E_INT, t->span);
            f->ival = -operand->ival;
            f->type = type_int(p->ty);
            return f;
        }
        if (t->kind == T_NOT && operand->kind == E_BOOL) {
            Expr *f = new_expr(p, E_BOOL, t->span);
            f->ival = !operand->ival;
            f->type = type_bool(p->ty);
            return f;
        }
        Expr *e = new_expr(p, E_UNARY, t->span);
        e->op = t->kind;
        e->lhs = operand;
        if (t->kind == T_MINUS || t->kind == T_TILDE) {
            const char *what = t->kind == T_MINUS ? "negate" : "apply '~' to";
            /* Negation works on a float -- it flips the sign bit, so -0.0 stays
             * distinct from 0.0 -- but bitwise not has no meaning for one. */
            int ok = t->kind == T_MINUS
                         ? (is_kind(operand->type, TK_INT) || is_kind(operand->type, TK_F64))
                         : is_kind(operand->type, TK_INT);
            if (!is_unk(operand->type) && !ok) {
                diag_error(t->span, "cannot %s '%s'", what, type_name(p->ty, operand->type));
                e->type = NULL;
            } else if (is_kind(operand->type, TK_F64)) {
                e->type = type_f64(p->ty);
            } else {
                e->type = type_int(p->ty);
            }
        } else {
            if (!is_unk(operand->type) && !is_kind(operand->type, TK_BOOL)) {
                diag_error(t->span, "cannot apply '!' to '%s'", type_name(p->ty, operand->type));
                e->type = NULL;
            } else {
                e->type = type_bool(p->ty);
            }
        }
        return e;
    }
    if (t->kind == T_AMP) { /* &expr */
        advance(p);
        /* `&f` where f names a function is that function's address, typed by
         * its signature. This has to be caught before the operand is parsed as
         * an expression, because a bare function name is not a variable. */
        if (at(p, T_IDENT) && peek(p, 1)->kind != T_LPAREN && peek(p, 1)->kind != T_DOT) {
            char *fn_src = cur(p)->text;
            Sig *fs = find_sig(p, resolve_fn_sym(p, fn_src));
            if (fs == NULL) {
                char *cand = ns_resolve(p, fn_src, look_sig);
                if (cand != NULL && cand != fn_src)
                    fs = find_sig(p, resolve_fn_sym(p, cand));
                if (fs != NULL)
                    fn_src = cand;
            }
            if (fs != NULL) {
                Span fspan = cur(p)->span;
                char *fname = arena_strdup(
                    p->arena, (fs->is_extern || fs->is_export) ? ns_short_name(fn_src) : fn_src);
                /* resolve_fn_sym maps nested functions to their mangled symbol;
                 * extern/export keep the C spelling either way. */
                if (!fs->is_extern && !fs->is_export)
                    fname = arena_strdup(p->arena, resolve_fn_sym(p, fn_src));
                advance(p);
                Expr *fe = new_expr(p, E_FNPTR, fspan);
                fe->name = fname;
                fe->is_extern = fs->is_extern || fs->is_export;
                Type **pts = NULL;
                if (fs->nparams > 0) {
                    pts = arena_alloc_array(p->arena, (size_t)fs->nparams, sizeof(Type *));
                    for (int i = 0; i < fs->nparams; i++)
                        pts[i] = fs->ptypes[i];
                }
                fe->type = type_fnptr(p->ty, pts, fs->nparams, fs->ret);
                return fe;
            }
        }
        int saved_take = p->take_method_addr;
        p->take_method_addr = 1;
        Expr *operand = parse_unary(p);
        p->take_method_addr = saved_take;
        /* `&obj.M` is already the pointer; taking its address again would be
         * nonsense, and it is not an lvalue. A qualified `&N.foo` arrives the
         * same way, as an `E_FNPTR` from the namespace path below. */
        if (operand->kind == E_MPTR || operand->kind == E_FNPTR)
            return operand;
        Expr *e = new_expr(p, E_ADDR, t->span);
        e->lhs = operand;
        if (!is_lvalue(operand)) {
            if (!is_unk(operand->type)) {
                diag_error(t->span, "cannot take address of a temporary");
            }
            e->type = NULL;
        } else {
            e->type = type_ptr(p->ty, operand->type);
        }
        return e;
    }
    if (t->kind == T_STAR) { /* *expr */
        advance(p);
        Expr *operand = parse_unary(p);
        Expr *e = new_expr(p, E_DEREF, t->span);
        e->lhs = operand;
        if (!is_kind(operand->type, TK_PTR)) {
            if (!is_unk(operand->type)) {
                diag_error(t->span, "cannot dereference %s", type_name(p->ty, operand->type));
            }
            e->type = NULL;
        } else {
            e->type = operand->type->base;
        }
        return e;
    }
    return parse_primary(p);
}

static Expr *parse_binary(Parser *p, int min_prec) {
    Expr *lhs = parse_unary(p);
    for (;;) {
        TokenKind op = cur(p)->kind;
        int prec = binop_prec(op);
        if (prec == 0 || prec < min_prec)
            break;
        Span span = cur(p)->span;
        advance(p);
        Expr *rhs = parse_binary(p, prec + 1);
        lhs = make_binary(p, op, lhs, rhs, span);
    }
    return lhs;
}

static int at_assign_op(TokenKind k) {
    return k == T_ASSIGN || k == T_PLUS_EQ || k == T_MINUS_EQ || k == T_STAR_EQ ||
           k == T_SLASH_EQ || k == T_PERCENT_EQ || k == T_AMP_EQ || k == T_PIPE_EQ ||
           k == T_CARET_EQ || k == T_SHL_EQ || k == T_SHR_EQ;
}

static TokenKind base_op(TokenKind k) {
    switch (k) {
    case T_PLUS_EQ:
        return T_PLUS;
    case T_MINUS_EQ:
        return T_MINUS;
    case T_STAR_EQ:
        return T_STAR;
    case T_SLASH_EQ:
        return T_SLASH;
    case T_PERCENT_EQ:
        return T_PERCENT;
    case T_AMP_EQ:
        return T_AMP;
    case T_PIPE_EQ:
        return T_PIPE;
    case T_CARET_EQ:
        return T_CARET;
    case T_SHL_EQ:
        return T_SHL;
    case T_SHR_EQ:
        return T_SHR;
    default:
        return T_ASSIGN;
    }
}

/* Does this expression name bytes that something else owns?
 *
 * The answer decides whether storing the value copies it. A string is the one
 * owning type that can be made sound without `move` everywhere, because a copy
 * of a string is a copy of the bytes rather than a second name for them, so
 * every store of a borrowed string has to become a `str_dup`. Get this wrong in
 * one direction and two values name one allocation, which frees twice; get it
 * wrong in the other and an allocation is copied and the original is dropped on
 * the floor. Only the second is safe, so anything this does not recognize counts
 * as fresh.
 *
 * Fresh means: a literal, which is static and owned by nobody; a call, because
 * every `return` in the language produces a value nothing else holds (see the
 * copy on return below, which is what makes that true); a string concatenation
 * or slice, both of which build new bytes; and a `move`, which hands the value
 * over on purpose. */
int expr_is_borrowed_string(const Expr *e) {
    if (e == NULL)
        return 0;
    switch (e->kind) {
    case E_VAR:   /* a local, a parameter, or a captured cell */
    case E_FIELD: /* a field of a struct or class */
    case E_INDEX: /* an element */
    case E_DEREF: /* through a pointer */
    case E_TRY:   /* the Ok payload, which is a field of the Result union */
    case E_MATCH: /* a bound payload, same shape */
    case E_IFACE:
        return 1;
    case E_UNARY:
        return expr_is_borrowed_string(e->lhs);
    case E_TERNARY:
        return expr_is_borrowed_string(e->rhs) ||
               (e->args != NULL && e->nargs > 0 ? expr_is_borrowed_string(e->args[0]) : 0);
    case E_BINARY:
        /* `+` on two strings builds new bytes. Anything else that type-checks to
         * a string would be the concatenation anyway. */
        return e->op != T_PLUS;
    default:
        return 0;
    }
}

/* The value to store into a `string` slot: `str_dup(e)` when `e` names bytes
 * someone else owns, and `e` itself when it does not.
 *
 * This is the whole of string ownership, and it is a rewrite rather than a
 * change to code generation because it can be expressed in the language: a dup
 * is an ordinary call to an ordinary builtin, so it is type-checked, it inlines,
 * and it needs no path through the register allocator. */
static Expr *own_string_copy(Parser *p, Expr *e) {
    if (!is_kind(e->type, TK_STRING) || !expr_is_borrowed_string(e))
        return e;
    Expr **args = arena_alloc_array(p->arena, 1, sizeof(Expr *));
    args[0] = e;
    Expr *d = new_expr(p, E_CALL, e->span);
    d->name = arena_strdup(p->arena, "z_str_dup");
    d->is_extern = 1;
    d->args = args;
    d->nargs = 1;
    d->type = type_string(p->ty);
    return d;
}

/* Records that a store or a `return` has taken over a fresh string value, so the
 * code generator releases it once rather than treating it as a temporary as well.
 * Two owners for one value is the bug this whole stage is about, so the claim is
 * made in one place next to the copy rather than in each of the three sites. */
static Expr *claim_string_value(Expr *e) {
    if (e != NULL && is_kind(e->type, TK_STRING) && !expr_is_borrowed_string(e))
        e->str_result_owned = 1;
    return e;
}

static Expr *parse_expr(Parser *p) {
    Expr *lhs = parse_binary(p, 1);
    TokenKind k = cur(p)->kind;
    if (at_assign_op(k)) {
        Span span = cur(p)->span;
        int compound = k != T_ASSIGN;
        TokenKind bop = base_op(k);
        advance(p);
        /* An assignment chain is right-associative, so `a = b = c` parses as
         * `a = (b = c)` and the recursion is as deep as the chain. It is
         * bounded like every other descent. */
        if (!enter_depth(p)) {
            /* The rest of the chain is stepped over rather than parsed, so the
             * statements after it are still statements and not the tail of this
             * one. */
            skip_to_statement_end(p);
            Expr *e = new_expr(p, E_ASSIGN, span);
            e->op = bop;
            e->compound = compound;
            e->lhs = lhs;
            e->rhs = new_expr(p, E_INT, cur(p)->span);
            e->rhs->ival = 0;
            e->rhs->type = type_int(p->ty);
            e->type = NULL;
            return e;
        }
        Expr *rhs = parse_expr(p);
        leave_depth(p);
        if (!is_lvalue(lhs)) {
            if (!is_unk(lhs->type)) {
                diag_error(span, "left side of assignment must be a variable");
            }
            lhs = new_expr(p, E_INT, lhs->span);
            lhs->type = type_int(p->ty);
        }
        Expr *e = new_expr(p, E_ASSIGN, span);
        e->op = bop;
        e->compound = compound;
        e->lhs = lhs;
        e->rhs = rhs;
        /* Assigning a variable gives it a value again, so a `move` or a
         * `str_buf_append` that emptied it no longer applies to it.
         *
         * The flag was write-once, which made a moved variable unusable for the
         * rest of its scope: `buf = str_buf_append(buf, s)` empties `buf` at the
         * call, and without this the second append was "'buf' has been moved
         * from". `StringBuilder.take()` has always written `buf = str_buf_new(0)`
         * after moving out of it and only worked because a field was never
         * flagged.
         *
         * Only the plain form clears it. A compound assignment reads the
         * destination before it writes it, so `buf += s` on a moved `buf` is
         * still a use of a moved value. */
        if (!compound && lhs->kind == E_VAR && lhs->name != NULL) {
            Var *dst = lookup_var(p, lhs->name);
            if (dst != NULL)
                dst->is_moved = 0;
        }
        /* Storing into a string slot copies borrowed bytes. Covers every
         * assignment target there is, because a local, a field and an element
         * store are all one E_ASSIGN whose left side carries the type. A
         * compound assignment is not one: `s += x` runs the operator and the
         * result is a new string, which the plain case already treats as
         * fresh. */
        if (!compound) {
            e->rhs = own_string_copy(p, rhs);
            e->rhs = claim_string_value(e->rhs);
            /* Releasing the destination's old value is only right when the new
             * value is a different allocation. `str_buf_append` is the one
             * builtin that hands back its first argument unchanged whenever the
             * buffer had room, so this is a transfer and not a replacement: it
             * frees the very buffer the new value names, and the next append
             * reads freed memory. That is the in-place growth the runtime's own
             * comment describes, so the fact belongs to the call rather than to
             * every caller. Both spellings, because the call has already been
             * resolved to its runtime symbol by the time the assignment is
             * built: the arguments had to be type-checked first. */
            int transfers = rhs->kind == E_CALL && rhs->name != NULL &&
                            (strcmp(rhs->name, "str_buf_append") == 0 ||
                             strcmp(rhs->name, "z_str_buf_append") == 0);
            if (is_kind(lhs->type, TK_STRING) && !transfers)
                e->frees_old = 1;
        }
        if (compound) {
            /* A compound assignment runs the operator and stores back into the
             * left operand, so the right side is promoted to the left's type
             * rather than the whole expression being retyped. `f += 1` is
             * therefore float arithmetic, and `i += 1.5` is an error rather
             * than a silent truncation. */
            if (is_kind(lhs->type, TK_F64) &&
                (bop == T_PLUS || bop == T_MINUS || bop == T_STAR || bop == T_SLASH)) {
                if (!is_kind(rhs->type, TK_F64) && !is_unk(rhs->type) &&
                    !is_kind(rhs->type, TK_INT))
                    diag_error(span, "operator %s= requires 'float' or 'int' operands",
                               token_kind_name(bop));
                else
                    rhs = make_cvt(p, type_f64(p->ty), rhs, span);
                e->rhs = rhs;
                e->type = lhs->type;
                return e;
            }
            if (!is_kind(lhs->type, TK_INT) || !is_kind(rhs->type, TK_INT)) {
                if (!is_unk(lhs->type) && !is_unk(rhs->type)) {
                    diag_error(span, "operator %s= requires 'int' operands", token_kind_name(bop));
                    e->type = NULL;
                    return e;
                }
            }
            e->type = lhs->type;
        } else {
            /* The one implicit conversion: an int widens to a float, which
             * cannot lose a value. Everything else has to match exactly -- except
             * a concrete value going to an interface, which boxes it. */
            if (is_kind(lhs->type, TK_IFACE) && !is_kind(rhs->type, TK_IFACE) &&
                !is_unk(rhs->type)) {
                rhs = to_iface(p, rhs, lhs->type, span);
                e->rhs = rhs;
                e->type = lhs->type;
                return e;
            }
            if (type_widens_to(lhs->type, rhs->type) && !is_unk(lhs->type) && !is_unk(rhs->type)) {
                rhs = make_cvt(p, lhs->type, rhs, span);
                e->rhs = rhs;
                e->type = lhs->type;
                return e;
            }
            if (!type_assignable(lhs->type, rhs->type) && !is_unk(lhs->type) &&
                !is_unk(rhs->type)) {
                /* Name the cast that would work, when there is one: "cannot
                 * assign float to int" is a true sentence but not much help. */
                if (is_kind(lhs->type, TK_INT) && is_kind(rhs->type, TK_F64))
                    diag_error(span,
                               "cannot assign 'float' to 'int' without losing precision; write "
                               "'(int)' if that is what you want",
                               type_name(p->ty, rhs->type), type_name(p->ty, lhs->type));
                else {
                    note_missing_class_pointer(p, lhs->type, rhs->type);
                    diag_error_code(span, "type_mismatch", "cannot assign '%s' to '%s'",
                                    type_name(p->ty, rhs->type), type_name(p->ty, lhs->type));
                }
                e->type = NULL;
                return e;
            }
            e->type = lhs->type;
        }
        return e;
    }
    /* Conditional (ternary): `cond ? a : b`. */
    if (at(p, T_QUESTION)) {
        Span span = cur(p)->span;
        advance(p);
        Expr *cond = lhs;
        Expr *then_e = parse_expr(p);
        if (!match(p, T_COLON)) {
            diag_error(cur(p)->span, "expected ':' in conditional expression");
        }
        Expr *else_e = parse_expr(p);
        Expr *e = new_expr(p, E_TERNARY, span);
        e->lhs = cond;
        e->rhs = then_e;
        e->args = arena_alloc_array(p->arena, 1, sizeof(Expr *));
        e->args[0] = else_e;
        e->nargs = 1;
        if (!is_unk(cond->type) && !is_kind(cond->type, TK_BOOL)) {
            diag_error(span, "condition of '?:' must be 'bool' but got '%s'",
                       type_name(p->ty, cond->type));
            e->type = NULL;
        } else if (!type_equals(then_e->type, else_e->type)) {
            if (!is_unk(then_e->type) && !is_unk(else_e->type)) {
                diag_error(span, "branches of '?:' have different types '%s' and '%s'",
                           type_name(p->ty, then_e->type), type_name(p->ty, else_e->type));
                e->type = NULL;
            } else {
                e->type = then_e->type;
            }
        } else {
            e->type = then_e->type;
        }
        return e;
    }
    return lhs;
}
