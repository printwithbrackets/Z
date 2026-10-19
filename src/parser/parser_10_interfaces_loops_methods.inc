static Stmt *parse_interface_decl(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* 'interface' */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected interface name after 'interface'");
        return new_stmt(p, S_EXPR, start);
    }
    char *name = ns_join(p, ns_here(p), cur(p)->text);
    advance(p);
    IfaceDef *id = type_define_iface(p->ty, name);
    /* A pre-scan already built the method list, so that a signature naming this
     * interface -- and a call resolving one of its methods -- could work above the
     * declaration. The members are about to be read again, so start over rather
     * than append. */
    if (id != NULL && id->prescanned) {
        id->prescanned = 0;
        id->methods = NULL;
        id->nmethods = 0;
    }
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open interface body");
        return new_stmt(p, S_EXPR, start);
    }
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        if (at(p, T_KW_FN)) {
            advance(p);
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected a method name after 'fn'");
                break;
            }
            char *mname = cur(p)->text;
            advance(p);
            if (!match(p, T_LPAREN)) {
                diag_error(cur(p)->span, "expected '(' after method name");
                break;
            }
            Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
            int np = 0;
            if (!at(p, T_RPAREN)) {
                for (;;) {
                    if (np >= Z_MAX_ARGS) {
                        diag_error(cur(p)->span, "'%s' takes too many parameters", mname);
                        break;
                    }
                    if (at(p, T_KW_MUT))
                        advance(p);
                    Type *pt = parse_type(p);
                    if (pt == NULL || !at(p, T_IDENT)) {
                        diag_error(cur(p)->span, "expected a parameter type and name");
                        break;
                    }
                    advance(p);
                    ptypes[np++] = pt;
                    if (!match(p, T_COMMA))
                        break;
                }
            }
            match(p, T_RPAREN);
            Type *ret = type_void(p->ty);
            if (at(p, T_ARROW)) {
                advance(p);
                ret = parse_type(p);
                if (ret == NULL) {
                    diag_error(cur(p)->span, "expected return type after '->'");
                    break;
                }
            }
            if (iface_find_method(id, mname) != NULL) {
                diag_error(start, "interface '%s' already requires '%s'", name, mname);
            } else {
                iface_add_method(p->ty, id, mname, type_fnptr(p->ty, ptypes, np, ret));
            }
            /* Method signatures end with an optional ';'. */
            match(p, T_SEMI);
            continue;
        }
        Type *ret = parse_type(p);
        if (ret == NULL) {
            diag_error(cur(p)->span, "expected a return type in interface body");
            break;
        }
        if (!at(p, T_IDENT)) {
            diag_error(cur(p)->span, "expected a method name in interface body");
            break;
        }
        char *mname = cur(p)->text;
        advance(p);
        if (!match(p, T_LPAREN)) {
            diag_error(cur(p)->span, "expected '(' after method name");
            break;
        }
        Type **ptypes = arena_alloc_array(p->arena, Z_MAX_ARGS, sizeof(Type *));
        int np = 0;
        if (!at(p, T_RPAREN)) {
            for (;;) {
                if (np >= Z_MAX_ARGS) {
                    diag_error(cur(p)->span, "'%s' takes too many parameters", mname);
                    break;
                }
                Type *pt = parse_type(p);
                if (pt == NULL || !at(p, T_IDENT)) {
                    diag_error(cur(p)->span, "expected a parameter type and name");
                    break;
                }
                advance(p); /* parameter name; an interface has no body to read */
                ptypes[np++] = pt;
                if (!match(p, T_COMMA))
                    break;
            }
        }
        match(p, T_RPAREN);
        if (iface_find_method(id, mname) != NULL) {
            diag_error(start, "interface '%s' already requires '%s'", name, mname);
        } else {
            iface_add_method(p->ty, id, mname, type_fnptr(p->ty, ptypes, np, ret));
        }
        /* Method signatures end with an optional ';'. */
        match(p, T_SEMI);
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close interface body");
    }
    return new_stmt(p, S_BLOCK, start);
}

/* foreach (T x in coll) body  ->  a hidden index-based while loop. */
/* True when a `for (` is the head of a range-for, i.e. a colon turns up before
 * the closing paren. A C-style `for` has semicolons where the colon would be,
 * so this cannot mistake one for the other, and it never parses: it only looks,
 * so the caller can rewind and take the ordinary path. */
static int at_range_for(Parser *p) {
    int i = p->pos;
    /* `for` '(' has been consumed. Step over the loop variable, which is
     * `var`/`auto` optionally followed by a type and '&', then a name. */
    while (i < p->ntoks && i - p->pos < 8) {
        TokenKind k = p->toks[i].kind;
        if (k == T_COLON)
            return 1;
        if (k == T_SEMI || k == T_RPAREN || k == T_LBRACE || k == T_EOF)
            return 0;
        i++;
    }
    return 0;
}

/* Builds a call to a no-argument or one-argument method on a class receiver,
 * by the mangled symbol the method is emitted under. This is how a container is
 * reached from a desugaring, where there is no source expression to resolve a
 * name against. */
static Expr *make_method_call(Parser *p, Expr *recv, const char *sdef, const char *mname, Type *ret,
                              Expr *arg, Span span) {
    char *mang = arena_alloc(p->arena, strlen(sdef) + strlen(mname) + 3);
    snprintf(mang, strlen(sdef) + strlen(mname) + 3, "%s__%s", sdef, mname);
    Expr *c = new_expr(p, E_CALL, span);
    c->name = mang;
    if (arg == NULL) {
        Expr **args = arena_alloc_array(p->arena, 1, sizeof(Expr *));
        args[0] = recv;
        c->args = args;
        c->nargs = 1;
    } else {
        Expr **args = arena_alloc_array(p->arena, 2, sizeof(Expr *));
        args[0] = recv;
        args[1] = arg;
        c->args = args;
        c->nargs = 2;
    }
    c->type = ret;
    return c;
}

/* `for (auto x : coll)` and `for (auto& x : coll)`.
 *
 * A desugaring rather than a new statement kind, for the same reason `foreach`
 * is: the loop that comes out is an ordinary index loop, so every optimizer
 * that understands a `for` already understands this, and `break`/`continue`
 * need no special case. What changes with the receiver is only *how* the
 * element is read.
 *
 * A `Vec<T>` is a class holding a pointer, so it is indexed with `at(i)` and
 * counted with `size()` rather than with `[i]` and `.length` -- the container
 * knows its own representation and a desugaring that indexed it directly would
 * have to know the field offsets too.
 *
 * `&` is the difference between a copy and a reference: without it each
 * element is read into a fresh local, and with it the loop writes back into the
 * container. There is no C++ overload of `operator[]` to lean on here, so this
 * is spelled by binding the element by reference, which the index expression
 * already produces. */
static Stmt *parse_range_for(Parser *p, Span start) {
    match(p, T_LPAREN);
    int by_ref = 0;
    Type *written = NULL;
    if (at(p, T_KW_VAR)) {
        advance(p);
        if (at(p, T_KW_MUT))
            advance(p); /* `var mut x`: accepted like every other `mut` */
        /* `auto&` is a reference binding, and it is the spelling C++ uses, so
         * it has to mean something the same way `var` plus a separate `&`
         * does. */
        by_ref = match(p, T_AMP);
    } else if (at(p, T_AMP)) {
        /* `for (&x : v)` is a bare-reference binding, and the element type is
         * taken from the container either way. */
        advance(p);
        by_ref = 1;
    } else if (is_type_token(cur(p)->kind) || at_typed_decl(p)) {
        written = parse_type(p);
        if (at(p, T_KW_MUT))
            advance(p);
        by_ref = match(p, T_AMP);
    }
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected loop variable name");
        return new_stmt(p, S_BLOCK, start);
    }
    char *name = cur(p)->text;
    advance(p);
    if (!match(p, T_COLON)) {
        diag_error(cur(p)->span, "expected ':' in a range-based for");
        match(p, T_SEMI);
        skip_braced_block(p);
        return new_stmt(p, S_BLOCK, start);
    }
    Expr *coll = parse_expr(p);
    match(p, T_RPAREN);

    /* A string is iterable as its bytes, like the fixed array it replaces. */
    int is_str = is_kind(coll->type, TK_STRING);
    int is_arr = is_kind(coll->type, TK_ARRAY);
    Type *cbase = NULL;
    char *cstruct = NULL;
    int cdata_off = -1;
    if (is_str) {
        cbase = type_int(p->ty);
    } else if (is_arr) {
        cbase = coll->type->base;
    } else if (!is_unk(coll->type) && is_kind(coll->type, TK_PTR) &&
               is_kind(coll->type->base, TK_STRUCT) && coll->type->base->sdef->is_class) {
        cstruct = coll->type->base->sdef->name;
        /* The element type comes from the container's own `at`, which is the
         * only place it is written down: a `Vec<T>` stores its type parameter
         * nowhere a generic instantiation can be read back out of a field. */
        StructDef *sd = coll->type->base->sdef;
        StructMethod *m = struct_find_method(sd, "at");
        cbase = m ? m->ret : NULL;
        /* The offset of the element buffer, which a by-reference element binds
         * to. A container without one cannot be iterated by reference, because
         * `at` hands back a value and there is then no place to write. */
        Field *df = struct_find_field(sd, "data");
        cdata_off = df ? df->offset : -1;
        if (cbase == NULL) {
            diag_error(start, "cannot range over '%s': it has no 'at(i)' to read elements with",
                       coll->type->base->sdef->name);
            match(p, T_SEMI);
            skip_braced_block(p);
            return new_stmt(p, S_BLOCK, start);
        }
    }
    if (cbase == NULL) {
        if (!is_unk(coll->type)) {
            if (is_kind(coll->type, TK_PTR))
                diag_error(start,
                           "cannot range over '%s': a pointer has no length, so there is no "
                           "bound to iterate to; use an index loop over the array instead",
                           type_name(p->ty, coll->type));
            else
                diag_error(start, "a range-based for needs an array, a string or a class, got %s",
                           type_name(p->ty, coll->type));
        }
        match(p, T_SEMI);
        skip_braced_block(p);
        return new_stmt(p, S_BLOCK, start);
    }

    scope_push(p);
    /* A written element type is checked against the container's, because a
     * loop variable declared `int` and bound to a `string` would otherwise be a
     * silent wrong answer rather than a mistake at the point of writing it. */
    if (written != NULL && !type_equals(written, cbase)) {
        diag_error(start, "cannot bind '%s' to an element of type '%s'", type_name(p->ty, written),
                   type_name(p->ty, cbase));
    }
    char hname[32];
    int loop_id = p->foreach_counter++;
    snprintf(hname, sizeof hname, "__rng%d", loop_id);
    int coll_slot = declare_var(p, hname, coll->type);
    mark_synth(p, hname);
    snprintf(hname, sizeof hname, "__ri%d", loop_id);
    int idx_slot = declare_var(p, hname, type_int(p->ty));
    mark_synth(p, hname);
    /* The element is a local whether or not it is a reference: `auto& x` is a
     * local bound to the container's storage, so a write through it lands in the
     * container. Which is why `by_ref` changes the initializer and not the
     * variable's shape. */
    /* A by-reference element is a *pointer* to the element, so the variable is
     * declared as one. Declaring it as the element type and storing an address
     * in it type-checks and then reads the address as if it were the value --
     * `Console.WriteLog(x)` printed the element's location rather than the element, and
     * `x = 5` overwrote the location. The body then works through `*`, exactly
     * as a hand-written `var p = &v.data[i]` does. */
    int elem_slot = declare_var(p, name, by_ref ? type_ptr(p->ty, cbase) : cbase);

    Type *int_ty = type_int(p->ty);

    /* One node per position, never shared. The allocator walks the condition
     * and the body as separate liveness positions, and a node reached from
     * both is walked twice: the second walk re-decides where it lives and the
     * two positions then disagree about the same variable. `parse_foreach`
     * builds a fresh reference at each use for the same reason. */
    Expr *idx_ref = new_expr(p, E_VAR, start);
    idx_ref->name = "idx";
    idx_ref->slot = idx_slot;
    idx_ref->type = int_ty;

    Stmt *coll_init = new_stmt(p, S_VAR, start);
    coll_init->name = "coll";
    coll_init->type = coll->type;
    coll_init->slot = coll_slot;
    coll_init->init = coll;

    Expr *zero = new_expr(p, E_INT, start);
    zero->ival = 0;
    zero->type = int_ty;
    Stmt *idx_init = new_stmt(p, S_VAR, start);
    idx_init->name = "idx";
    idx_init->type = int_ty;
    idx_init->slot = idx_slot;
    idx_init->init = zero;

    /* cond: idx < <length of coll> */
    Expr *len;
    {
        Expr *cr = new_expr(p, E_VAR, start);
        cr->name = "coll";
        cr->slot = coll_slot;
        cr->type = coll->type;
        if (cstruct != NULL) {
            len = make_method_call(p, cr, cstruct, "size", int_ty, NULL, start);
        } else if (is_str) {
            len = new_expr(p, E_STRLEN, start);
            len->lhs = cr;
            len->type = int_ty;
        } else {
            len = new_expr(p, E_FIELD, start);
            len->lhs = cr;
            len->name = "length";
            len->type = int_ty;
        }
    }
    Expr *cond = make_binary(p, T_LT, idx_ref, len, start);

    /* element = <coll[idx]> */
    Expr *elem;
    {
        Expr *cr = new_expr(p, E_VAR, start);
        cr->name = "coll";
        cr->slot = coll_slot;
        cr->type = coll->type;
        Expr *ir = new_expr(p, E_VAR, start);
        ir->slot = idx_slot;
        ir->type = int_ty;
        if (cstruct != NULL) {
            elem = make_method_call(p, cr, cstruct, "at", cbase, ir, start);
        } else if (is_str) {
            elem = new_expr(p, E_INDEX, start);
            elem->lhs = cr;
            elem->rhs = ir;
            elem->type = int_ty;
        } else {
            elem = new_expr(p, E_INDEX, start);
            elem->lhs = cr;
            elem->rhs = ir;
            elem->type = cbase;
        }
    }
    Stmt *elem_init = new_stmt(p, S_VAR, start);
    elem_init->name = name;
    elem_init->type = cbase;
    elem_init->slot = elem_slot;
    if (by_ref && cstruct != NULL) {
        /* A by-reference element has to keep writing to the container, so the
         * variable is a *pointer* at `data[i]` rather than a copy of `at(i)`.
         * `at` returns by value, so there is no lvalue behind it to read and a
         * copy in the initializer would make `&` a lie: the loop would read the
         * element and discard the write. The field is what makes the element
         * addressable, which is the same reason `p->` and `p.` are the same
         * thing -- a container that could not hand out a place to its own
         * elements could not be iterated by reference at all. */
        Expr *dataf = new_expr(p, E_FIELD, start);
        Expr *cr1 = new_expr(p, E_VAR, start);
        cr1->name = "coll";
        cr1->slot = coll_slot;
        cr1->type = coll->type;
        dataf->lhs = cr1;
        dataf->name = "data";
        dataf->field_off = cdata_off;
        dataf->type = type_array(p->ty, cbase, -1);
        Expr *place = new_expr(p, E_INDEX, start);
        Expr *ir1 = new_expr(p, E_VAR, start);
        ir1->slot = idx_slot;
        ir1->type = int_ty;
        place->lhs = dataf;
        place->rhs = ir1;
        place->type = cbase;
        Expr *addr = new_expr(p, E_ADDR, start);
        addr->lhs = place;
        addr->type = type_ptr(p->ty, cbase);
        elem_init->type = type_ptr(p->ty, cbase);
        elem_init->init = addr;
    } else if (by_ref) {
        /* A string's or an array's element is already an lvalue, so the
         * initializer is the index expression itself and the variable is a
         * pointer to it. A `char *`-shaped loop over a string, which is what
         * C++'s `for (char& c : s)` means. */
        Expr *place = new_expr(p, E_INDEX, start);
        Expr *cr2 = new_expr(p, E_VAR, start);
        cr2->name = "coll";
        cr2->slot = coll_slot;
        cr2->type = coll->type;
        Expr *ir2 = new_expr(p, E_VAR, start);
        ir2->slot = idx_slot;
        ir2->type = int_ty;
        place->lhs = cr2;
        place->rhs = ir2;
        place->type = is_str ? int_ty : cbase;
        Expr *addr = new_expr(p, E_ADDR, start);
        addr->lhs = place;
        addr->type = type_ptr(p->ty, is_str ? int_ty : cbase);
        elem_init->type = type_ptr(p->ty, is_str ? int_ty : cbase);
        elem_init->init = addr;
    } else {
        elem_init->init = elem;
    }

    /* idx = idx + 1 */
    Expr *one = new_expr(p, E_INT, start);
    one->ival = 1;
    one->type = int_ty;
    Expr *idx_ref3 = new_expr(p, E_VAR, start);
    idx_ref3->slot = idx_slot;
    idx_ref3->type = int_ty;
    Expr *asg = new_expr(p, E_ASSIGN, start);
    asg->op = T_PLUS;
    asg->compound = 1;
    asg->lhs = idx_ref3;
    asg->rhs = one;
    asg->type = int_ty;
    Stmt *step = new_stmt(p, S_EXPR, start);
    step->expr = asg;

    p->loop_depth++;
    Stmt *body = parse_stmt(p);
    p->loop_depth--;
    scope_pop(p);

    Stmt **inner = arena_alloc_array(p->arena, 3, sizeof(Stmt *));
    inner[0] = elem_init;
    inner[1] = step;
    inner[2] = body;
    Stmt *inner_block = new_stmt(p, S_BLOCK, start);
    inner_block->items = inner;
    inner_block->nitems = 3;

    Stmt *loop = new_stmt(p, S_WHILE, start);
    loop->cond = cond;
    loop->body = inner_block;

    Stmt **outer = arena_alloc_array(p->arena, 3, sizeof(Stmt *));
    outer[0] = coll_init;
    outer[1] = idx_init;
    outer[2] = loop;
    Stmt *wrap = new_stmt(p, S_BLOCK, start);
    wrap->items = outer;
    wrap->nitems = 3;
    return wrap;
}

static Stmt *parse_foreach(Parser *p, Span start) {
    advance(p); /* foreach */
    match(p, T_LPAREN);
    Type *et = NULL;
    int infer = 0;
    if (match(p, T_KW_VAR)) {
        infer = 1;
    } else {
        et = parse_type(p);
        /* `foreach (v in xs)` with a bare identifier: parse_type found no type
         * (it did not consume `v`), so treat the variable as inferred rather
         * than leaving a NULL element type. */
        if (et == NULL)
            infer = 1;
    }
    if (at(p, T_KW_MUT))
        advance(p); /* `foreach (var mut x in xs)`, like every other `mut` */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected loop variable name");
        return new_stmt(p, S_BLOCK, start);
    }
    char *name = cur(p)->text;
    advance(p);
    if (!match(p, T_KW_IN)) {
        diag_error(cur(p)->span, "expected 'in' in foreach");
    }
    Expr *coll = parse_expr(p);
    match(p, T_RPAREN);

    /* Only an array. A pointer has no length -- there is nothing for the
     * loop's bound to compare against -- and reading `ptr.length` off one
     * yields the pointer's own address, which used to turn a `foreach` over a
     * `T*` into an unbounded walk into unrelated memory. To walk part of an
     * array, use an explicit index loop. */
    if (!is_kind(coll->type, TK_ARRAY)) {
        if (!is_unk(coll->type)) {
            if (is_kind(coll->type, TK_PTR))
                diag_error(start,
                           "cannot foreach over '%s': a pointer has no length, so there is no "
                           "bound to iterate to; use an index loop over the array instead",
                           type_name(p->ty, coll->type));
            else
                diag_error(start, "foreach requires an array, got %s",
                           type_name(p->ty, coll->type));
        }
        /* The body is skipped rather than parsed: nothing in it can be
         * meaningful once the collection is rejected, and parsing it would bury
         * the one real diagnostic under a pile of undefined-name errors. */
        match(p, T_SEMI);
        skip_braced_block(p);
        return new_stmt(p, S_BLOCK, start);
    }

    /* Loop variables live in their own scope. */
    scope_push(p);
    char hname[32];
    int loop_id = p->foreach_counter++;
    /* __arr/__fi are this desugaring's own bookkeeping and are never named in
     * source, so they are exempt from -Wunused-local. */
    snprintf(hname, sizeof hname, "__arr%d", loop_id);
    int arr_slot = declare_var(p, hname, coll->type);
    mark_synth(p, hname);
    snprintf(hname, sizeof hname, "__fi%d", loop_id);
    int idx_slot = declare_var(p, hname, type_int(p->ty));
    mark_synth(p, hname);
    int elem_slot = declare_var(p, name, infer ? coll->type->base : et);

    /* Build the desugared loop by hand (normal parser helpers would redeclare
     * names in the wrong scope). The loop variables stay in scope while the
     * body is parsed so the element name resolves. */
    Type *int_ty = type_int(p->ty);

    Expr *arr_ref = new_expr(p, E_VAR, start);
    arr_ref->name = "coll";
    arr_ref->slot = arr_slot;
    arr_ref->type = coll->type;

    Expr *idx_ref = new_expr(p, E_VAR, start);
    idx_ref->name = "idx";
    idx_ref->slot = idx_slot;
    idx_ref->type = int_ty;

    /* coll = <collection> */
    Stmt *arr_init = new_stmt(p, S_VAR, start);
    arr_init->name = "coll";
    arr_init->type = coll->type;
    arr_init->slot = arr_slot;
    arr_init->init = coll;

    /* init index = 0 */
    Expr *zero = new_expr(p, E_INT, start);
    zero->ival = 0;
    zero->type = int_ty;
    Stmt *idx_init = new_stmt(p, S_VAR, start);
    idx_init->name = "idx";
    idx_init->type = int_ty;
    idx_init->slot = idx_slot;
    idx_init->init = zero;

    /* cond: idx < coll.length */
    Expr *len = new_expr(p, E_FIELD, start);
    len->lhs = arr_ref;
    len->name = "length";
    len->type = int_ty;
    Expr *cond = make_binary(p, T_LT, idx_ref, len, start);

    /* element = coll[idx] */
    Expr *idx_ref2 = new_expr(p, E_VAR, start);
    idx_ref2->slot = idx_slot;
    idx_ref2->type = int_ty;
    Expr *elem = new_expr(p, E_INDEX, start);
    elem->lhs = arr_ref;
    elem->rhs = idx_ref2;
    elem->type = coll->type->base;
    Stmt *elem_init = new_stmt(p, S_VAR, start);
    elem_init->name = name;
    elem_init->type = elem->type;
    elem_init->slot = elem_slot;
    elem_init->init = elem;

    /* idx = idx + 1 */
    Expr *one = new_expr(p, E_INT, start);
    one->ival = 1;
    one->type = int_ty;
    Expr *idx_ref3 = new_expr(p, E_VAR, start);
    idx_ref3->slot = idx_slot;
    idx_ref3->type = int_ty;
    Expr *asg = new_expr(p, E_ASSIGN, start);
    asg->op = T_PLUS;
    asg->compound = 1;
    asg->lhs = idx_ref3;
    asg->rhs = one; /* idx = idx + 1 */
    asg->type = int_ty;
    Stmt *step = new_stmt(p, S_EXPR, start);
    step->expr = asg;

    p->loop_depth++;
    Stmt *body = parse_stmt(p);
    p->loop_depth--;
    scope_pop(p);

    Stmt **inner = arena_alloc_array(p->arena, 3, sizeof(Stmt *));
    inner[0] = elem_init;
    inner[1] = step;
    inner[2] = body;
    Stmt *inner_block = new_stmt(p, S_BLOCK, start);
    inner_block->items = inner;
    inner_block->nitems = 3;

    Stmt *loop = new_stmt(p, S_WHILE, start);
    loop->cond = cond;
    loop->body = inner_block;

    Stmt **outer = arena_alloc_array(p->arena, 3, sizeof(Stmt *));
    outer[0] = arr_init;
    outer[1] = idx_init;
    outer[2] = loop;
    Stmt *wrap = new_stmt(p, S_BLOCK, start);
    wrap->items = outer;
    wrap->nitems = 3;
    return wrap;
}

/* Parses a method declared inside a struct body. The receiver `this` is a
 * hidden first parameter (a pointer to the struct); the emitted function is
 * mangled as `Struct__method` and registered on the struct. Returns the S_FUNC
 * AST node for later code generation. */
static Stmt *parse_method(Parser *p, StructDef *sd, const char *sname, Type *ret, const char *mname,
                          Span start, int is_virtual) {
    advance(p); /* '(' */
    Type *stype = type_find_struct(p->ty, sname);
    Type *this_ty = stype ? type_ptr(p->ty, stype) : NULL;

    /* A method whose return type is its *own* class returns the reference.
     *
     * `new C()` yields a `C*` and every field of a `C` is reached through one, so
     * a `Node next()` on a class has nothing sensible to return but the pointer --
     * and writing `Node*` for it would be saying the same thing twice, since the
     * name already means the reference type. The receiver is spelled the same
     * way: `this` is a `C*` and nobody writes the star.
     *
     * **Its own** class, and that is the whole condition. A method returning some
     * *other* class is left alone, because there the star is real information:
     * `Vec<T> first()` on a `Vec<T>` that happens to be instantiated with a class
     * element type would otherwise silently become `Vec<T>*` and stop agreeing
     * with the type the caller wrote. A struct is untouched for the same reason
     * the receiver's rule does not apply to a field: a struct is already a value,
     * and `Node next()` there returns one by copy. */
    if (ret != NULL && is_kind(ret, TK_STRUCT) && ret->sdef == sd && sd->is_class)
        ret = type_ptr(p->ty, ret);

    Scope *saved_scope = p->scope;
    int saved_off = p->next_offset;
    Type *saved_ret = p->cur_ret;
    StructDef *saved_msd = p->cur_msd;
    Expr *saved_this = p->cur_this;
    scope_push(p);
    /* A method body is a frame of its own, exactly as a function body is, and
     * the flag is what says so. Without it two things go wrong, and they are the
     * same thing seen twice: name lookup walks past the method into whatever
     * enclosed it, and the destructor pass builds its chain to the same
     * boundary, so a local string a method returns was never released. */
    p->scope->is_fn_body = 1;
    p->next_offset = 0;
    p->cur_ret = ret;

    /* Qualified with the receiver, because a bare method name in a message
     * ("has no method 'lenght'") does not say which type's method list was
     * searched, and there may be ten. Sized dynamically so long type/method
     * names are not truncated in the diagnostic context. */
    int ctxneed = snprintf(NULL, 0, "%s.%s", sname, mname);
    if (ctxneed < 0) {
        diag_error(start, "could not name method '%s.%s'", sname, mname);
    }
    char *ctxlabel = arena_alloc(p->arena, (size_t)(ctxneed >= 0 ? ctxneed + 1 : 1));
    if (ctxlabel == NULL) {
        diag_error(start, "could not name method '%s.%s'", sname, mname);
    } else if (ctxneed >= 0) {
        snprintf(ctxlabel, (size_t)ctxneed + 1, "%s.%s", sname, mname);
    } else {
        ctxlabel[0] = '\0';
    }
    diag_push_ctx(DIAG_CTX_METHOD, ctxlabel, start);

    int pcap = 4, pn = 0;
    Expr **params = arena_alloc_array(p->arena, (size_t)pcap, sizeof(Expr *));
    Expr *this_var = new_expr(p, E_VAR, start);
    this_var->name = arena_strdup(p->arena, "this");
    this_var->type = this_ty;
    this_var->slot = declare_var(p, "this", this_ty);
    mark_param(p, "this");
    /* The receiver is reached through every unqualified field and method name,
     * so a method that never writes `this` is still using it. */
    mark_synth(p, "this");
    params[pn++] = this_var;
    p->cur_msd = sd;
    p->cur_this = this_var;

    Type **ptypes = NULL;
    int nexplicit = 0;
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (at(p, T_KW_MUT))
                advance(p);
            Type *pt = parse_type(p);
            if (pt == NULL || !at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected parameter type and name");
                break;
            }
            char *pname = cur(p)->text;
            Span ps = cur(p)->span;
            advance(p);
            if (pn == pcap) {
                int ncap = pcap * 2;
                Expr **npar = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                memcpy(npar, params, (size_t)pn * sizeof(Expr *));
                params = npar;
                pcap = ncap;
            }
            Expr *pe = new_expr(p, E_VAR, ps);
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
            nexplicit++;
            if (!match(p, T_COMMA))
                break;
        }
    }
    match(p, T_RPAREN);
    /* Rust-style `fn m(...) -> R` inside a struct: the return type follows. */
    if (at(p, T_ARROW)) {
        advance(p);
        Type *rt = parse_type(p);
        if (rt == NULL) {
            diag_error(cur(p)->span, "expected return type after '->'");
        } else {
            ret = rt;
            p->cur_ret = ret;
            if (is_kind(ret, TK_STRUCT) && ret->sdef == sd && sd->is_class)
                ret = type_ptr(p->ty, ret);
        }
    }

    {
        int agg = is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION);
        /* A method's receiver is a parameter in the frame like any other, and an
         * aggregate result adds the hidden result pointer on top, so both come
         * out of the same budget the argument-setup arrays are sized by. */
        int max_params = Z_MAX_ARGS - 1 - (agg ? 1 : 0);
        if (nexplicit > max_params) {
            diag_error(start, "method '%s' takes %d parameters but the limit is %d%s", mname,
                       nexplicit, max_params,
                       agg ? " (a struct-returning method spends one on the result buffer)" : "");
        }
    }

    /* Struct-returning methods also use the hidden-pointer convention; the
     * hidden buffer pointer precedes `this` as the first argument. */
    if (is_kind(ret, TK_STRUCT) || is_kind(ret, TK_UNION)) {
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

    char *mangled = arena_alloc(p->arena, strlen(sname) + strlen(mname) + 3);
    snprintf(mangled, strlen(sname) + strlen(mname) + 3, "%s__%s", sname, mname);

    Stmt *fn = new_stmt(p, S_FUNC, start);
    fn->fname = mangled;
    fn->src_fname = arena_strdup(p->arena, mname);
    fn->ret_type = ret;
    fn->params = params;
    fn->nparams = pn;
    /* A method's parameter 0 is the receiver, and a struct-returning one pushes
     * the hidden result buffer in front of that, so neither is a parameter a
     * reader can match to the source. */
    fn->vis_start = params[0] != NULL && strcmp(params[0]->name, "$ret") == 0 ? 2 : 1;
    if (match(p, T_FATARROW)) {
        Expr *e = parse_expr(p);
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

    check_falls_off_end(fn, ret, 0, start, ctxlabel);

    if (nexplicit > 0) {
        ptypes = arena_alloc_array(p->arena, (size_t)nexplicit, sizeof(Type *));
        /* params[0] is `this` (or the hidden $ret buffer if sret), so the
         * i-th explicit parameter is at params[i + 1 + regoff].
         *
         * The offset comes from whether the hidden buffer is actually there,
         * which `vis_start` already worked out, rather than from asking whether
         * the return type is a struct. Anything returned through memory has that
         * buffer, and `Result` is a two-variant *union*, so asking about structs
         * put the offset one short for every Result-returning method: ptypes[0]
         * read the receiver instead of the first declared parameter, and the call
         * site then reported "argument 1 expects 'F*' but got 'int'" for a method
         * taking an int. A Result-returning method with no parameters was fine,
         * because with nothing to misalign there was nothing to report. */
        int regoff = fn->vis_start - 1;
        for (int i = 0; i < nexplicit; i++)
            ptypes[i] = params[i + 1 + regoff]->type;
    }
    /* A pre-scan may already have registered this method's signature so that
     * code above the declaration could see the type has the method -- which is
     * how a struct declared later can still satisfy an interface. Fill that entry
     * in rather than adding a second one, or the method would be emitted twice
     * and found twice. */
    StructMethod *m = struct_find_method(sd, mname);
    if (m == NULL || m->body != NULL) {
        m = arena_alloc(p->arena, sizeof *m);
        m->name = arena_strdup(p->arena, mname);
        m->vtable_index = -1;
        struct_add_method(p->ty, sd, m);
    }
    m->ret = ret;
    m->ptypes = ptypes;
    m->nparams = nexplicit;
    m->body = fn;
    m->is_virtual = is_virtual;
    m->vtable_index = -1;

    scope_pop(p);
    p->scope = saved_scope;
    p->next_offset = saved_off;
    p->cur_ret = saved_ret;
    p->cur_msd = saved_msd;
    p->cur_this = saved_this;
    diag_pop_ctx();
    return fn;
}

/* struct Name { field; ret method(params) {..} ret Prop { get; set; } } */
/* enum Name { Variant(fields), Variant2, ... } */
static Stmt *parse_enum_decl(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* 'enum' */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected enum name after 'enum'");
        return new_stmt(p, S_EXPR, start);
    }
    char *name = ns_join(p, ns_here(p), cur(p)->text);
    advance(p);
    UnionDef *ud = type_define_union(p->ty, name);
    if (ud == NULL) {
        diag_error(start, "enum '%s' is already defined", name);
    }
    /* A pre-scan already built the variant list, so that a `match` above this
     * declaration could check exhaustiveness. Rebuild it rather than appending:
     * the variants are about to be read again. */
    if (ud != NULL && ud->prescanned) {
        ud->prescanned = 0;
        ud->variants = NULL;
        ud->nvariants = 0;
        ud->size = 0;
        ud->align = 0;
    }
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open enum body");
        return new_stmt(p, S_EXPR, start);
    }
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        if (!at(p, T_IDENT)) {
            diag_error(cur(p)->span, "expected variant name");
            break;
        }
        char *vname = cur(p)->text;
        advance(p);
        VariantFields vf = {NULL, NULL, 0, 0};
        if (match(p, T_LPAREN)) {
            while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
                Type *ft = parse_type(p);
                if (ft == NULL || !at(p, T_IDENT)) {
                    diag_error(cur(p)->span, "expected variant field type and name");
                    break;
                }
                const char *fname = cur(p)->text;
                advance(p);
                variant_fields_add(p, &vf, ft, fname);
                if (!match(p, T_COMMA))
                    break;
            }
            match(p, T_RPAREN);
        }
        if (ud != NULL)
            union_add_variant(p->ty, ud, vname, vf.ftypes, vf.fnames, vf.n);
        if (!match(p, T_COMMA))
            break;
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close enum body");
    }
    if (ud != NULL)
        union_finish(ud);
    Stmt *s = new_stmt(p, S_UNION, start);
    s->udef = ud;
    return s;
}

/* match subject { Variant(a, b) => body, _ => body } (exhaustive). */
static Expr *parse_match(Parser *p, Span start) {
    advance(p); /* 'match' */
    Expr *subject = parse_expr(p);
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' after match subject");
        return new_expr(p, E_INT, start);
    }
    Type *ut = subject->type;
    UnionDef *ud = is_kind(ut, TK_UNION) ? ut->udef : NULL;
    if (ud == NULL && !is_unk(ut)) {
        diag_error(start, "match requires a union value but got '%s'", type_name(p->ty, ut));
    }

    int cap = 4, n = 0;
    MatchArm *arms = arena_alloc_array(p->arena, (size_t)cap, sizeof(MatchArm));
    int covered = 0;
    int wildcard = 0;
    /* Which variants an arm named, so the exhaustiveness diagnostic can list the
     * ones still missing. "not exhaustive: 4 variants but 2 covered" makes the
     * reader diff two lists themselves; naming the two tells them what to add. */
    int nslots = (ud != NULL && ud->nvariants > 0) ? ud->nvariants : 1;
    char *hit = arena_alloc_array(p->arena, (size_t)nslots, 1);
    memset(hit, 0, (size_t)nslots);
    Type *restype = NULL;
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        VariantDef *v = NULL;
        if (at(p, T_IDENT) && strcmp(cur(p)->text, "_") == 0) {
            advance(p);
            wildcard = 1;
        } else if (at(p, T_IDENT)) {
            char *vname = cur(p)->text;
            advance(p);
            if (ud != NULL) {
                for (int i = 0; i < ud->nvariants; i++) {
                    if (strcmp(ud->variants[i].name, vname) == 0) {
                        v = &ud->variants[i];
                        break;
                    }
                }
            }
            if (v == NULL) {
                if (ud != NULL) {
                    const char *vc[64];
                    int nvc = ud->nvariants < 64 ? ud->nvariants : 64;
                    for (int i = 0; i < nvc; i++)
                        vc[i] = ud->variants[i].name;
                    diag_suggest_note("variant", vname, vc, nvc);
                }
                diag_error_code(start, "no_such_variant", "'%s' is not a variant of '%s'", vname,
                                ud ? ud->name : "this match");
                v = NULL;
            } else {
                covered++;
                hit[v - ud->variants] = 1;
            }
        } else {
            diag_error(cur(p)->span, "expected a variant name or '_' in match arm");
            break;
        }

        /* Each arm gets its own scope for its payload bindings. They are only
         * frame slots as far as codegen is concerned, so the scope is purely a
         * name-resolution device -- and without it two arms binding `v` collide,
         * which matters for Result because matching one is two arms whose
         * bindings naturally want the same names. */
        scope_push(p);
        int bcap = 4, bn = 0;
        int *slots = NULL;
        if (match(p, T_LPAREN)) {
            slots = arena_alloc_array(p->arena, (size_t)bcap, sizeof(int));
            while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
                if (!at(p, T_IDENT)) {
                    diag_error(cur(p)->span, "expected a binding name");
                    break;
                }
                /* bound name: the payload field at the same index */
                int fidx = bn;
                Type *ft = (v != NULL && fidx < v->nfields) ? v->fields[fidx].type : NULL;
                char *bname = cur(p)->text;
                advance(p);
                if (bn == bcap) {
                    int ncap = bcap * 2;
                    int *ns = arena_alloc_array(p->arena, (size_t)ncap, sizeof(int));
                    memcpy(ns, slots, (size_t)bn * sizeof(int));
                    slots = ns;
                    bcap = ncap;
                }
                if (ft == NULL)
                    ft = type_int(p->ty);
                slots[bn] = declare_var(p, bname, ft);
                /* `_` is a binding that is deliberately not read. It still needs
                 * a slot -- the arm's payload is copied into one either way -- but
                 * it is not a name, so it is not a use and must not be warned
                 * about. This is how an arm discards the payload it does not
                 * need, which with a per-arm scope is now the common case. */
                if (strcmp(bname, "_") == 0)
                    mark_synth(p, bname);
                bn++;
                if (!match(p, T_COMMA))
                    break;
            }
            match(p, T_RPAREN);
            if (v != NULL && bn != v->nfields && !is_unk(ut)) {
                diag_error(start, "variant '%s' expects %d binding%s but got %d", v->name,
                           v->nfields, v->nfields == 1 ? "" : "s", bn);
            }
        } else if (v != NULL && v->nfields > 0 && !is_unk(ut)) {
            diag_error(start, "variant '%s' expects %d binding%s", v->name, v->nfields,
                       v->nfields == 1 ? "" : "s");
        }

        if (!match(p, T_FATARROW)) {
            diag_error(cur(p)->span, "expected '=>' in match arm");
        }
        Expr *body = parse_expr(p);
        scope_pop(p);
        if (restype == NULL)
            restype = body->type;
        else if (!is_unk(restype) && !is_unk(body->type) && !type_equals(restype, body->type)) {
            diag_error(start, "match arms have different types '%s' and '%s'",
                       type_name(p->ty, restype), type_name(p->ty, body->type));
        }
        if (n == cap) {
            int ncap = cap * 2;
            MatchArm *na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(MatchArm));
            memcpy(na, arms, (size_t)n * sizeof(MatchArm));
            arms = na;
            cap = ncap;
        }
        arms[n].variant = v;
        arms[n].bind_slots = slots;
        arms[n].nbind = bn;
        arms[n].body = body;
        n++;
        if (!match(p, T_COMMA))
            break;
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close match");
    }
    /* Exhaustiveness: every variant must be covered (or a `_` wildcard). */
    if (ud != NULL && !wildcard && covered < ud->nvariants) {
        /* One note naming each missing variant, so the list is where the reader
         * is already looking rather than in a second diagnostic they have to
         * match up by hand. */
        for (int i = 0; i < ud->nvariants; i++) {
            if (!hit[i])
                diag_note("'%s' is not handled; add '%s%s' or a '_' arm", ud->variants[i].name,
                          ud->variants[i].name, ud->variants[i].nfields > 0 ? "(...)" : "");
        }
        diag_error_code(start, "non_exhaustive_match",
                        "match does not handle every variant of '%s': %d of %d covered", ud->name,
                        covered, ud->nvariants);
    }

    Expr *e = new_expr(p, E_MATCH, start);
    e->lhs = subject;
    e->arms = arms;
    e->narms = n;
    e->type = restype;
    return e;
}

/* The method name a type's destructor is registered under, and the symbol it is
 * emitted as. The two differ: `~` cannot appear in an assembler symbol, so the
 * emitted name spells it `dtor`. Keeping one function responsible means the
 * parse side and the emit side cannot disagree about which method *is* the
 * destructor. */
static char *dtor_name(Parser *p, StructDef *sd) {
    if (sd == NULL)
        return NULL;
    char *n = arena_alloc(p->arena, strlen(sd->name) + 6);
    snprintf(n, strlen(sd->name) + 6, "dtor%s", sd->name);
    return n;
}

/* Steps over a braced block, counting braces so that a body containing one does
 * not end the skip early. Returns the index just past the closing brace. */
static int skip_braced(Parser *p, int i) {
    int depth = 0;
    while (i < p->ntoks && p->toks[i].kind != T_EOF) {
        if (p->toks[i].kind == T_LBRACE)
            depth++;
        else if (p->toks[i].kind == T_RBRACE && --depth == 0)
            return i + 1;
        i++;
    }
    return i;
}
