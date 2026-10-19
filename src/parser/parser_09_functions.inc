static Stmt *parse_func(Parser *p, int nested) {
    int fn_tok_start = p->pos;
    Span start = cur(p)->span;
    /* Each function owns an id, so a lambda inside it can tell its own locals
     * from the ones it captured. */
    int fn_owner = ++p->func_counter;
    int saved_owner = p->cur_owner;
    p->cur_owner = fn_owner;

    /* A function starts a fresh capture list, and the enclosing one is put back
     * on the way out. A top-level program is the enclosing context for a
     * function declared after some top-level statements, and a plain
     * `p->ncaptured = 0` on the way out discarded the closures that had already
     * captured those statements -- so their declarations were never boxed, and
     * the first call that allocated anything walked a frame slot nobody filled.
     * The list is a fixed-size array, so copying it is cheap. */
    Var saved_captured[MAX_CAPTURES];
    int saved_ncaptured = p->ncaptured;
    if (saved_ncaptured > 0)
        memcpy(saved_captured, p->captured, (size_t)saved_ncaptured * sizeof(Var));
    p->ncaptured = 0;

    /* Look ahead for a generic parameter list `name<T,...>(` and pre-bind the
     * type parameters so the return type and parameter types can reference them
     * even though the list is written after the name. */
    int ntparams = 0;
    char **tparams = NULL;
    int saved_ntbind = p->ntbind;

    /* An `extern` declaration names a symbol implemented in C. The body is
     * optional (and forbidden for extern): the declaration exists so calls
     * type-check and so codegen emits the symbol unmangled. */
    int is_extern = 0;
    if (at(p, T_KW_EXTERN)) {
        is_extern = 1;
        advance(p);
    }
    /* `export` is the mirror of `extern`: the body is here, but the symbol
     * keeps the name as written and is made .globl so C can call it. */
    int is_export = 0;
    if (at(p, T_KW_EXPORT)) {
        is_export = 1;
        advance(p);
    }
    /* Rust-style `fn name(params) [-> ret]`. When present, the header is
     * parsed here and the C-style header below is skipped via fn_style. */
    /* `fn` opens a Rust-style header only when a name follows it; a C-style
     * function returning a function-pointer type (`fn(int) -> int f(...)`)
     * leads with the same keyword and takes the type-first path below. */
    int fn_style = at(p, T_KW_FN) && peek(p, 1)->kind == T_IDENT;
    Type *ret = NULL;
    char *name = NULL;
    char *src_name = NULL;
    if (!fn_style) {
        {
            int j = skip_type_tokens(p, p->pos);
            if (j < p->ntoks && p->toks[j].kind == T_IDENT) {
                int k = j + 1;
                if (k < p->ntoks && p->toks[k].kind == T_LT && k + 1 < p->ntoks &&
                    p->toks[k + 1].kind == T_IDENT) {
                    int e = skip_generic_params(p, k);
                    int cap = 4;
                    tparams = arena_alloc_array(p->arena, (size_t)cap, sizeof(char *));
                    int m = k + 1;
                    while (m < e) {
                        if (p->toks[m].kind == T_IDENT) {
                            if (ntparams == cap) {
                                cap *= 2;
                                char **g = arena_alloc_array(p->arena, (size_t)cap, sizeof(char *));
                                memcpy(g, tparams, (size_t)ntparams * sizeof(char *));
                                tparams = g;
                            }
                            tparams[ntparams] = p->toks[m].text;
                            if (!is_bound(p, p->toks[m].text))
                                bind_type(p, p->toks[m].text,
                                          type_new_param(p->ty, p->toks[m].text));
                            ntparams++;
                        }
                        m++;
                    }
                }
            }
        }

        ret = parse_type(p);
        if (ret == NULL) {
            if (cur(p)->kind == T_IDENT)
                suggest_in_scope(p, SK_TYPE, "type", cur(p)->text);
            diag_error(cur(p)->span, "unknown type '%s'",
                       cur(p)->kind == T_IDENT ? cur(p)->text : token_kind_name(cur(p)->kind));
        }
        name = cur(p)->text;
        advance(p); /* name */

        /* Consume the generic parameter list if present (already bound above). */
        if (ntparams > 0 && at(p, T_LT))
            p->pos = skip_generic_params(p, p->pos);
        match(p, T_LPAREN);
        src_name = name;
    } else {
        advance(p); /* 'fn' */
        if (!at(p, T_IDENT)) {
            diag_error(cur(p)->span, "expected function name after 'fn'");
            name = arena_strdup(p->arena, "<bad>");
        } else {
            name = cur(p)->text;
            advance(p);
        }
        src_name = name;
        if (at(p, T_LT) && p->pos + 1 < p->ntoks && p->toks[p->pos + 1].kind == T_IDENT) {
            int e = skip_generic_params(p, p->pos);
            int cap = 4;
            tparams = arena_alloc_array(p->arena, (size_t)cap, sizeof(char *));
            int m = p->pos + 1;
            while (m < e) {
                if (p->toks[m].kind == T_IDENT) {
                    if (ntparams == cap) {
                        cap *= 2;
                        char **g = arena_alloc_array(p->arena, (size_t)cap, sizeof(char *));
                        memcpy(g, tparams, (size_t)ntparams * sizeof(char *));
                        tparams = g;
                    }
                    tparams[ntparams] = p->toks[m].text;
                    if (!is_bound(p, p->toks[m].text))
                        bind_type(p, p->toks[m].text, type_new_param(p->ty, p->toks[m].text));
                    ntparams++;
                }
                m++;
            }
        }
        if (ntparams > 0 && at(p, T_LT))
            p->pos = skip_generic_params(p, p->pos);
        match(p, T_LPAREN);
        ret = type_void(p->ty);
    }

    /* A function written inside another one is emitted under a symbol built from
     * the enclosing function's id, so two `helper` declarations in two different
     * bodies do not collide. Everything the reader sees -- diagnostics, the
     * mapping from the written name -- keeps using the name as written. */
    if (nested) {
        /* Sized dynamically: a fixed buffer here truncated long function names
         * so two distinct nested functions shared one symbol. */
        int need = snprintf(NULL, 0, "$fn%d_%s", saved_owner, src_name);
        if (need < 0) {
            diag_error(start, "could not name a nested function '$fn%d_%s'", saved_owner, src_name);
        } else {
            char *sym = arena_alloc(p->arena, (size_t)need + 1);
            if (sym == NULL) {
                diag_error(start, "could not name a nested function '$fn%d_%s'", saved_owner,
                           src_name);
            } else {
                snprintf(sym, (size_t)need + 1, "$fn%d_%s", saved_owner, src_name);
                name = sym;
            }
        }
    }
    /* `extern` and `export` name a symbol deliberately, and a local has no
     * outside to be visible to. */
    if (nested && (is_extern || is_export))
        diag_error(start, "a function declared inside another function cannot be '%s'",
                   is_extern ? "extern" : "export");
    /* `export` publishes the name as written for C to call, but a name declared
     * in a namespace is registered qualified (`n.f`), which C cannot spell.
     * Define it at the top level instead; the declaration still parses so one
     * mistake does not cascade. */
    if (!nested && is_export && p->ns_prefix != NULL && p->ns_prefix[0] != '\0')
        diag_error(start,
                   "'export' in namespace '%s' would publish '%s', which C cannot name; move it "
                   "to the top level",
                   p->ns_prefix, src_name);
    if (nested && ntparams > 0)
        diag_error(start, "'%s' cannot be a generic function here", src_name);
    /* A function declared in a namespace registers under "ns.name", so
     * `fn connect` in `namespace netlink` is the function `netlink.connect`.
     * Diagnostics keep the written name via src_name. */
    if (!nested && p->ns_prefix != NULL && p->ns_prefix[0] != '\0')
        name = ns_join(p, p->ns_prefix, src_name);

    Scope *saved_scope = p->scope;
    int saved_slot = p->next_offset;
    Type *saved_ret = p->cur_ret;
    LocalFn *saved_local_fns = p->local_fns;
    scope_push(p);
    p->scope->is_fn_body = 1;
    p->next_offset = 0;
    p->cur_ret = ret;

    int pcap = 4, pn = 0;
    Expr **params = arena_alloc_array(p->arena, (size_t)pcap, sizeof(Expr *));
    int is_ext = 0;
    if (at(p, T_KW_THIS)) {
        is_ext = 1; /* extension method: the first param is the receiver */
        advance(p);
    }
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (at(p, T_KW_MUT))
                advance(p); /* `mut` in params: accepted, params are mutable */
            Type *pt = parse_type(p);
            if (pt == NULL) {
                diag_error(cur(p)->span, "expected parameter type but found %s",
                           token_kind_name(cur(p)->kind));
                break;
            }
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected parameter name");
                break;
            }
            char *pname = cur(p)->text;
            Span pspan = cur(p)->span;
            advance(p);
            if (pn == pcap) {
                int ncap = pcap * 2;
                Expr **npar = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                memcpy(npar, params, (size_t)pn * sizeof(Expr *));
                params = npar;
                pcap = ncap;
            }
            Expr *pe = new_expr(p, E_VAR, pspan);
            pe->name = pname;
            pe->type = pt;
            pe->slot = declare_var(p, pname, pt);
            mark_param(p, pname);
            if (is_kind(pt, TK_STRUCT) || is_kind(pt, TK_UNION)) {
                pe->agg_param = 1;
                Var *pv = lookup_var_local(p, pname);
                if (pv)
                    pv->agg_param = 1;
            }
            params[pn++] = pe;
            if (!match(p, T_COMMA))
                break;
        }
    }
    match(p, T_RPAREN);
    int fn_has_arrow = 0;
    if (fn_style && at(p, T_ARROW)) {
        advance(p);
        Type *rt = parse_type(p);
        if (rt == NULL) {
            diag_error(cur(p)->span, "expected return type after '->'");
            rt = type_void(p->ty);
        }
        ret = rt;
        p->cur_ret = ret;
        fn_has_arrow = 1;
    }
    if (fn_style && !fn_has_arrow && at(p, T_FATARROW)) {
        diag_error(cur(p)->span,
                   "fn-style '=>' needs a return type: write 'fn %s(...) -> T => ...'", src_name);
    }

    /* A struct-returning function uses the SysV hidden-pointer convention: a
     * hidden first parameter holds the address of the caller's result buffer. */
    int ret_is_aggregate = is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION);
    /* The hidden result pointer is a parameter as far as the frame is concerned,
     * and the argument-setup arrays hold one entry per parameter. Accepting 16
     * written parameters here would make the 17th spill out of them, so a
     * struct-returning function is one shorter. */
    int max_params = ret_is_aggregate ? Z_MAX_ARGS - 1 : Z_MAX_ARGS;
    if (pn > max_params) {
        diag_error(start, "'%s' takes %d parameters but the limit is %d%s", src_name, pn,
                   max_params,
                   ret_is_aggregate ? " (a struct-returning function spends one on the result"
                                      " buffer)"
                                    : "");
    }
    if (ret_is_aggregate) {
        Expr **np = arena_alloc_array(p->arena, (size_t)(pn + 1), sizeof(Expr *));
        memcpy(np + 1, params, (size_t)pn * sizeof(Expr *));
        Expr *hidden = new_expr(p, E_VAR, start);
        hidden->name = arena_strdup(p->arena, "$ret");
        hidden->type = type_ptr(p->ty, ret);
        hidden->slot = declare_var(p, "$ret", hidden->type);
        /* The hidden result buffer is written by the return sequence, not named
         * in source, so it is never "used" by name. */
        mark_synth(p, "$ret");
        np[0] = hidden;
        params = np;
        pn++;
    }

    /* A nested function gets no pre-scan -- that only walks top-level tokens --
     * so its signature is filed here, under the mangled symbol, before the body
     * is read. Doing it first is what lets the body call itself. */
    if (nested) {
        int ivis = ret_is_aggregate ? 1 : 0;
        int invis = pn - ivis;
        Type **cpt = arena_alloc_array(p->arena, (size_t)(invis > 0 ? invis : 1), sizeof(Type *));
        for (int i = 0; i < invis; i++)
            cpt[i] = params[ivis + i]->type;
        add_sig(p, name, ret, cpt, invis, is_ext);
        /* Pushed onto the list the enclosing function is using, not a fresh one:
         * the body has to resolve its own name, and a fresh list would discard
         * the mapping on the way out of parse_func. The mapping outlives this
         * parse_func call so calls that follow the declaration still find it. */
        LocalFn *lf = arena_alloc(p->arena, sizeof *lf);
        lf->name = src_name;
        lf->sym = name;
        lf->next = p->local_fns;
        p->local_fns = lf;
    }

    Stmt *fn = new_stmt(p, S_FUNC, start);
    fn->fname = name;
    fn->src_fname = src_name;
    fn->ret_type = ret;
    fn->params = params;
    fn->nparams = pn;
    /* Anything diagnosed from here on is inside this function, and says so. A
     * body-less declaration raises nothing, so the frame is pushed after the
     * declaration-only case and popped on every way out. */
    diag_push_ctx(DIAG_CTX_FUNC, src_name, start);
    /* A struct-returning function's parameter 0 is the hidden result buffer, so
     * the first one a reader can match to the source is 1. */
    fn->vis_start = ret_is_aggregate ? 1 : 0;
    fn->is_ext = is_ext;
    fn->is_extern = is_extern;
    fn->is_export = is_export;
    /* `export` publishes the name so C can call it, which needs the body to
     * come from. With a `;` the declaration-only path below handles it, but
     * with neither `;` nor a body opener the body read falls through to
     * parse_block, whose generic "expected '{'" blames the token that comes
     * next and then reads that next declaration as this body's first
     * statement. Naming the export here and taking the declaration-only
     * path keeps the recovery where a ';' would have put it. */
    int export_bodyless = is_export && !at(p, T_FATARROW) && !at(p, T_LBRACE);
    if (is_export && (at(p, T_SEMI) || export_bodyless))
        diag_error(start,
                   "'export' function '%s' needs a body; use 'extern' to declare a C "
                   "function",
                   src_name);
    if (at(p, T_SEMI) || is_extern || (export_bodyless)) {
        /* Declaration only. Nothing is emitted; the pre-scan already recorded
         * the signature. */
        if (nested && !is_extern)
            diag_error(start, "a function declared inside another function needs a body; "
                              "a body-less declaration only makes sense at the top level");
        if (is_extern && at(p, T_LBRACE))
            diag_error(start, "'extern' function '%s' cannot have a body", src_name);
        else
            expect_semi(p);
        /* There is no body, so nothing in this scope can ever be referenced.
         * Claiming the parameters are unused would be reporting on C code we
         * have not seen. */
        for (Var *v = p->scope->vars; v != NULL; v = v->next)
            v->is_synth = 1;
        fn->fbody = NULL;
        fn->is_extern = is_extern;
        fn->locals_bytes = 0;
        p->ntbind = saved_ntbind;
        scope_pop(p);
        diag_pop_ctx();
        return fn;
    }
    if (ntparams > 0 && !p->in_instantiate) {
        /* A generic function's body is not parsed here at all.
         *
         * It used to be, and that is a real restriction rather than an
         * implementation detail: the body was type-checked with `T` as an opaque
         * placeholder, so a generic function could only use operations valid for
         * *every* type. `"" + x` on a `T` is an error, and so is `if (x)`, and so
         * is anything else that depends on what T turned out to be -- which is
         * most of what a collection needs to do to its elements.
         *
         * Checking per instantiation is the other model, and the one every
         * monomorphizing language uses: the body is checked once per concrete
         * type argument, with the parameter bound, so a `Vec<int>` really does
         * get an `int`-typed body. An error in a template therefore surfaces at
         * the use site, with a note saying which instance and where it was
         * asked for -- which is the same span the reader has to look at anyway,
         * since they never wrote the template *at* that call.
         *
         * The declaration is stepped over here and its body is parsed later, from
         * the same tokens, by instantiate(). Signatures for everything the body
         * might call were registered by the pre-scan, so nothing is lost by not
         * reading the body yet. */
        int fn_tok_end = p->pos;
        if (at(p, T_FATARROW)) {
            advance(p);
            parse_expr(p);
            expect_semi(p);
        } else if (at(p, T_LBRACE)) {
            /* A braced body needs no semicolon after the closing brace, so
             * expect_semi here would report one missing and blame whatever
             * declaration came next. */
            skip_braced_block(p);
        }
        Generic g;
        memset(&g, 0, sizeof g);
        g.name = name;
        g.ns_prefix = arena_strdup(p->arena, ns_here(p));
        g.ntparams = ntparams;
        g.tparams = tparams;
        g.tok_start = fn_tok_start;
        g.tok_end = fn_tok_end;
        g.is_ext = is_ext;
        g.nparams = fn->nparams;
        g.vis_start = (is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION)) ? 1 : 0;
        g.nvisible = fn->nparams - g.vis_start;
        g.ret_type = ret;
        Type **pt = arena_alloc_array(p->arena, (size_t)fn->nparams, sizeof(Type *));
        for (int i = 0; i < fn->nparams; i++)
            pt[i] = fn->params[i]->type;
        g.ptypes = pt;
        add_generic(p, g);
        fn->is_generic_template = 1;
        /* The parameters were registered in a scope that is about to be popped;
         * nothing in the body has been read, so there is nothing to warn about. */
        for (Var *v = p->scope->vars; v != NULL; v = v->next)
            v->is_synth = 1;
        fn->fbody = NULL;
        fn->locals_bytes = 0;
        p->ntbind = saved_ntbind;
        scope_pop(p);
        diag_pop_ctx();
        return fn;
    }
    if (match(p, T_FATARROW)) {
        /* Expression-bodied function: `int f(int a) => expr;` desugars to
         * `{ return expr; }`. For `fn f(...) => expr;` with no `->`, the
         * return type is inferred from the expression. */
        Expr *e = parse_expr(p);
        if (fn_style && is_kind(ret, TK_VOID) && !is_kind(e->type, TK_VOID) && !is_unk(e->type)) {
            ret = e->type;
            fn->ret_type = ret;
            p->cur_ret = ret;
        }
        if (!is_unk(e->type) && !is_unk(ret) && !type_equals(e->type, ret)) {
            diag_error(start, "cannot return '%s' from function returning '%s'",
                       type_name(p->ty, e->type), type_name(p->ty, ret));
        }
        Stmt *ret_st = new_stmt(p, S_RETURN, start);
        ret_st->expr = e;
        Stmt **items = arena_alloc_array(p->arena, 1, sizeof(Stmt *));
        items[0] = ret_st;
        Stmt *body = new_stmt(p, S_BLOCK, start);
        body->items = items;
        body->nitems = 1;
        fn->fbody = body;
        expect_semi(p);
    } else {
        fn->fbody = parse_block(p);
    }
    fn->locals_bytes = p->next_offset;

    p->ntbind = saved_ntbind;
    scope_pop(p);
    p->scope = saved_scope;
    p->next_offset = saved_slot;
    p->cur_ret = saved_ret;
    /* The mapping a nested function pushed belongs to the *enclosing* function:
     * its scope runs to the end of that function, so calls after the declaration
     * still find it. Only leaving the scope that owns it clears the list, and
     * that is this function's own `saved_local_fns` when the function being left
     * is a top-level one. A nested function's exit must not clear it. */
    if (!nested)
        p->local_fns = saved_local_fns;

    /* Every variable a lambda in this body captured is now known, so the slots
     * that hold boxes can be marked before codegen ever sees the tree. Done
     * after the body is complete and the scope is gone, so the marks are the
     * final word on which slots are indirect. */
    /* A function that returns a lambda has a closure, not a bare code address,
     * and the two are called differently: a closure carries an environment in the
     * second word of its cell and the call site has to pass it. The static type
     * has to say so, or a caller cannot generate the right call.
     *
     * A `fn` and a `closure` are different types and are called differently, so
     * a function that hands one back has to say which it is. Declaring `fn` and
     * returning a lambda is an error naming the right spelling. */
    if (is_kind(fn->ret_type, TK_FNPTR) && body_returns_closure(fn->fbody)) {
        diag_error(fn->span,
                   "'%s' returns a lambda, so its return type must be "
                   "'closure(...) -> ...' rather than 'fn(...) -> ...'",
                   fn->fname);
    }

    if (p->ncaptured > 0) {
        mark_boxed_stmt(p, fn->fbody);
        /* A captured parameter is a capture like any other: its slot holds a box,
         * so the prologue has to box the incoming value rather than store it
         * straight into the frame. mark_boxed_stmt does not see parameters --
         * they are not in the body -- so they are marked here. */
        for (int i = fn->vis_start; i < fn->nparams; i++) {
            for (int k = 0; k < p->ncaptured; k++) {
                if (p->captured[k]->offset == fn->params[i]->slot) {
                    fn->params[i]->boxed = 1;
                    break;
                }
            }
        }
    }
    /* Closed-form loops, once the body is complete: the pass reads variables, so
     * it cannot run before the whole function has been parsed. */
    if (fn->fbody != NULL)
        close_form_stmts(p, &fn->fbody, 1);

    check_falls_off_end(fn, fn->ret_type,
                        strcmp(src_name, "main") == 0 &&
                            (p->ns_prefix == NULL || p->ns_prefix[0] == '\0'),
                        start, src_name);

    if (saved_ncaptured > 0)
        memcpy(p->captured, saved_captured, (size_t)saved_ncaptured * sizeof(Var));
    p->ncaptured = saved_ncaptured;
    p->cur_owner = saved_owner;
    diag_pop_ctx();

    if (strcmp(src_name, "main") == 0 && (p->ns_prefix == NULL || p->ns_prefix[0] == '\0'))
        fn->is_entry = 1;

    /* A nested function is not emitted where it was written: it is queued with
     * the other hoisted functions and emitted alongside them, under its mangled
     * symbol. Its declaration site produces no code of its own. */
    if (nested)
        add_pending(p, fn);
    return fn;
}

/* interface Name { ret m(params); ... }
 *
 * A declaration only: the body is semicolon-terminated signatures, and which
 * types satisfy it is decided per use rather than declared. That is what lets a
 * struct implement an interface without naming it -- a struct has no vtable of
 * its own, so a declared `struct S : I` would have nowhere to put the methods.
 *
 * The order of the methods is the contract. A call resolves a name to an index
 * here, once, and every implementing type lays its itab out in this same order.
 */
