static Stmt *parse_struct_decl(Parser *p, int is_class) {
    Span start = cur(p)->span;
    advance(p); /* 'struct' or 'class' */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected type name after '%s'", is_class ? "class" : "struct");
        return new_stmt(p, S_EXPR, start);
    }
    /* The name as written, which for a generic declaration is the template's.
     * Building an instance substitutes the mangled name, so the struct that comes
     * out is `Vec__int` rather than a second `Vec` -- two instances of one
     * template must not share a type, or a `Vec<int>` field could hold a string. */
    char *written = cur(p)->text;
    char *name = written;
    StructDef *sd;
    if (p->inst_name != NULL) {
        /* An instance's name and its StructDef were interned by instantiate_type
         * before it read a token of the body, so a method that names the type
         * again finds this one. Recomputing the name here from the *currently*
         * bound parameters was wrong for the same reason twice over: it counted
         * every enclosing instantiation's bindings, so a Vec built inside a Set
         * inside a Vec was named `Vec__int_int` and was a third distinct type. */
        name = (char *)p->inst_name;
        sd = p->inst_sdef;
    } else {
        /* Inside a namespace the type registers under "ns.Name". `written`
         * stays short for constructor/destructor naming and diagnostics. */
        name = ns_join(p, ns_here(p), written);
        sd = type_define_struct(p->ty, name);
        if (sd == NULL) {
            diag_error(start, "%s '%s' is already defined", is_class ? "class" : "struct", name);
            /* Step over the whole declaration rather than reading its members.
             *
             * There is no type for them to belong to, and every member read below
             * was written on the assumption that there is one. Fields were guarded
             * and methods were guarded, and both guards were added because something
             * had already crashed: a plain method died in `struct_add_method`, a
             * destructor died passing a null name to the mangler. Guarding each
             * site is how three of them get found. Nothing in a body that names a
             * rejected type can be diagnosed usefully either, since the names in it
             * resolve against the type that is already there.
             *
             * The body still has to be consumed so that what follows parses in the
             * right place. */
            advance(p); /* the name */
            p->pos = skip_type_param_list(p, p->pos);
            if (at(p, T_LBRACE))
                p->pos = skip_braced(p, p->pos);
            return new_stmt(p, S_EXPR, start);
        }
    }
    /* Step over the name. */
    advance(p);
    /* A generic declaration carries its type-parameter list between the name and
     * the brace. Building an instance steps over it: the parameters are bound to
     * concrete types already, and the list is not part of the layout. */
    {
        int after = skip_type_param_list(p, p->pos);
        if (after != p->pos)
            p->pos = after;
    }
    /* A pre-scan already gave this declaration its fields, so that code above it
     * in the file could use it. Start the field list over: the members are about
     * to be read again, and appending to the pre-scanned list would double every
     * field and make every offset wrong. */
    if (sd != NULL && sd->prescanned) {
        sd->prescanned = 0;
        sd->fields = NULL;
        sd->nfields = 0;
        sd->props = NULL;
        sd->nprops = 0;
        sd->prop_cap = 0;
        sd->size = 0;
        sd->align = 1;
    }
    if (sd != NULL && is_class) {
        sd->is_class = 1;
        /* Z has no inheritance. A class used to be written `class C : B`, and
         * what that bought -- a collection of related types -- is what an
         * `interface` is for, without a base type to upcast to, a base
         * constructor to remember, or a vtable per class in the hierarchy. A
         * `:` here is now a diagnostic rather than a silently ignored base. */
        if (at(p, T_COLON)) {
            diag_error_code(cur(p)->span, "no_inheritance",
                            "Z has no inheritance: '%s' cannot have a base class. Use an "
                            "interface to collect related types, and give each type the "
                            "methods it needs",
                            name);
            advance(p);
            if (at(p, T_IDENT))
                advance(p);
        }
        sd->align = 8;
        sd->size = 8; /* vptr */
    }
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open %s body", is_class ? "class" : "struct");
        return new_stmt(p, S_EXPR, start);
    }
    /* A generic instance gets no pass over the whole unit, so the signatures are
     * not yet on the type -- and a method's body may call a method declared after
     * it, as `push` calling `Grow` does. Registering them here is the same pass
     * the pre-scan does for an ordinary type, run at the point the instance
     * exists. Written under the *written* name, because that is what a
     * constructor is spelled with. */
    if (p->inst_name != NULL) {
        prescan_struct_members(p, sd, p->pos, written);
    }
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        /* `~Type() { ... }` -- a destructor.
         *
         * A destructor is a method named after the type with a `~` in front, and
         * it takes no arguments: the value being destroyed is the receiver, and
         * there is nothing else to say. The emitted symbol has the `~` spelled
         * `dtor` because `~` is not a character the assembler accepts in a
         * symbol name, and a mangling that does not assemble is not a
         * mangling.
         *
         * It is parsed before the member-type lookahead because `~` is not a
         * type token and would otherwise be reported as a bad member type. */
        if (at(p, T_TILDE)) {
            Span dspan = cur(p)->span;
            advance(p);
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected a type name after '~'");
                break;
            }
            char *dname = cur(p)->text;
            advance(p);
            /* A generic template is written with its own name, so `~Vec()` is
             * what the source says while `name` is the mangled instance the
             * method is registered on. Both spellings are therefore accepted,
             * and the note names the instance so the message is still the one
             * that identifies the type. */
            int name_mismatch = strcmp(dname, name) != 0 && strcmp(dname, written) != 0;
            if (sd == NULL || name_mismatch) {
                diag_error(dspan,
                           "a destructor is named after its type: expected '~%s' but got '~%s'",
                           name, dname);
            }
            if (!at(p, T_LPAREN)) {
                diag_error(cur(p)->span, "expected '(' after '~%s'", name);
                break;
            }
            /* One destructor per type. A second one would emit the same symbol
             * twice and fail in the assembler, which is a worse error than
             * saying so here. */
            if (sd != NULL && find_method(p, sd, dtor_name(p, sd)) != NULL) {
                diag_error_code(dspan, "duplicate_destructor", "'%s' already has a destructor",
                                name);
            }
            /* `sname` is the *type* the method is registered on, because that
             * is what the receiver is looked up from; `mname` is the method's
             * own name, which for a destructor is the mangled spelling. */
            parse_method(p, sd, name, type_void(p->ty), dtor_name(p, sd), dspan, 0);
            continue;
        }
        int mvirtual = 0;
        if (at(p, T_KW_VIRTUAL)) {
            mvirtual = 1;
            advance(p);
        } else if (at(p, T_KW_OVERRIDE)) {
            /* `override` went with inheritance, because with no base class there
             * is nothing to override. It is a diagnostic rather than a silent
             * unknown method, so a program carried over from v1 says what
             * happened and what to do instead. */
            diag_error_code(cur(p)->span, "no_override",
                            "'override' has no meaning without inheritance: there is no base "
                            "method to override. Declare the method 'virtual' if it goes in an "
                            "interface");
            mvirtual = 1;
            advance(p);
        }
        /* Rust-style method: `fn Name(params) [-> Ret]` (optionally `virtual fn`).
         * Only when `fn` is followed by a name: `fn(int)->int f;` is a field
         * of function-pointer type and falls through to the C-style path. */
        if (at(p, T_KW_FN) && p->pos + 1 < p->ntoks && p->toks[p->pos + 1].kind == T_IDENT) {
            advance(p); /* 'fn' */
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected method name after 'fn'");
                break;
            }
            char *mname = cur(p)->text;
            Span mspan = cur(p)->span;
            advance(p);
            if (!at(p, T_LPAREN)) {
                diag_error(cur(p)->span, "expected '(' after method '%s'", mname);
                break;
            }
            if (is_class && strcmp(mname, name) == 0) {
                if (find_method(p, sd, name) != NULL) {
                    diag_error_code(cur(p)->span, "duplicate_constructor",
                                    "%s '%s' already has a constructor; Z has no constructor "
                                    "overloading, so give the second one a different name",
                                    is_class ? "class" : "struct", name);
                }
                parse_method(p, sd, name, type_void(p->ty), name, mspan, 0);
                continue;
            }
            /* Placeholder ret; the real one follows `->` and is picked up
             * inside parse_method. */
            parse_method(p, sd, name, type_void(p->ty), mname, mspan, mvirtual);
            continue;
        }
        Type *mt = parse_type(p);
        if (mt == NULL) {
            diag_error(cur(p)->span, "expected member type but found %s",
                       token_kind_name(cur(p)->kind));
            break;
        }
        if (is_class && at(p, T_LPAREN) && is_kind(mt, TK_STRUCT) && mt->sdef == sd) {
            /* Constructor: `C(params) { ... }` (a method named after the class).
             * It returns void (not the class type), so no hidden sret param. */
            /* Two constructors are not two constructors: Z dispatches a call by
             * name, so a second `Vec(...)` would be a second method with the same
             * name and the first one would win everywhere. Saying so is better
             * than the alternative, which is a method that silently does not
             * exist -- or an `nmethods` with two entries emitting the same
             * symbol twice and failing in the assembler. */
            if (find_method(p, sd, name) != NULL) {
                diag_error_code(cur(p)->span, "duplicate_constructor",
                                "%s '%s' already has a constructor; Z has no constructor "
                                "overloading, so give the second one a different name",
                                is_class ? "class" : "struct", name);
            }
            parse_method(p, sd, name, type_void(p->ty), name, cur(p)->span, 0);
            continue;
        }
        if (!at(p, T_IDENT)) {
            diag_error(cur(p)->span, "expected member name");
            break;
        }
        char *mname = cur(p)->text;
        Span mspan = cur(p)->span;
        advance(p);
        if (at(p, T_LPAREN)) {
            /* method */
            parse_method(p, sd, name, mt, mname, mspan, mvirtual);
        } else if (at(p, T_SEMI) || at(p, T_RBRACE) || at(p, T_TILDE) || at(p, T_KW_VIRTUAL) ||
                   at(p, T_KW_OVERRIDE) || at(p, T_KW_FN) || is_type_token(cur(p)->kind) ||
                   at(p, T_IDENT)) {
            /* plain field; the ';' is optional, so a field before '}' or before
             * the next member needs no terminator. */
            match(p, T_SEMI);
            if (sd != NULL)
                struct_add_field(p->ty, sd, mname, mt);
        } else if (at(p, T_LBRACE)) {
            /* auto-property: `int X { get; set; }` -> backing field + accessor
             * methods named `X`. */
            advance(p); /* '{' */
            int has_get = 0;
            while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                if (at(p, T_IDENT) && strcmp(cur(p)->text, "get") == 0) {
                    has_get = 1;
                    advance(p);
                    match(p, T_SEMI);
                } else if (at(p, T_IDENT) && strcmp(cur(p)->text, "set") == 0) {
                    advance(p);
                    match(p, T_SEMI);
                } else {
                    diag_error(cur(p)->span, "expected 'get' or 'set' in property");
                    break;
                }
            }
            if (!match(p, T_RBRACE)) {
                diag_error(cur(p)->span, "expected '}' to close property");
            }
            if (!has_get) {
                diag_error(mspan, "property '%s' needs at least a getter", mname);
            }
            if (sd != NULL) {
                /* The backing field IS the property storage; `p.X` reads/writes
                 * it directly, equivalent to a trivial getter/setter. */
                struct_add_prop_field(p->ty, sd, mname, mt);
            }
        } else if (at(p, T_FATARROW)) {
            /* Expression-bodied property: get-only, backed by a `get` method. */
            advance(p);
            /* parse body in a method context */
            Scope *ss = p->scope;
            int so = p->next_offset;
            Type *sr = p->cur_ret;
            StructDef *sm = p->cur_msd;
            Expr *st = p->cur_this;
            scope_push(p);
            p->next_offset = 0;
            p->cur_ret = mt;
            Type *sty = type_find_struct(p->ty, name);
            Expr *tv = new_expr(p, E_VAR, mspan);
            tv->name = arena_strdup(p->arena, "this");
            tv->type = sty ? type_ptr(p->ty, sty) : NULL;
            tv->slot = declare_var(p, "this", tv->type);
            mark_synth(p, "this");
            p->cur_msd = sd;
            p->cur_this = tv;
            Expr *e = parse_expr(p);
            Stmt *ret_st = new_stmt(p, S_RETURN, mspan);
            ret_st->expr = e;
            Stmt **items = arena_alloc_array(p->arena, 1, sizeof(Stmt *));
            items[0] = ret_st;
            Stmt *body = new_stmt(p, S_BLOCK, mspan);
            body->items = items;
            body->nitems = 1;
            char *gname = arena_alloc(p->arena, strlen(name) + strlen(mname) + 8);
            snprintf(gname, strlen(name) + strlen(mname) + 8, "%s__get_%s", name, mname);
            Stmt *fn = new_stmt(p, S_FUNC, mspan);
            fn->fname = gname;
            fn->ret_type = mt;
            Expr **gparams = arena_alloc_array(p->arena, 1, sizeof(Expr *));
            gparams[0] = tv;
            fn->params = gparams;
            fn->nparams = 1;
            fn->fbody = body;
            fn->locals_bytes = p->next_offset;
            StructMethod *m = arena_alloc(p->arena, sizeof *m);
            m->name = arena_strdup(p->arena, mname);
            m->ret = mt;
            m->ptypes = NULL;
            m->nparams = 0;
            m->body = fn;
            struct_add_method(p->ty, sd, m);
            scope_pop(p);
            p->scope = ss;
            p->next_offset = so;
            p->cur_ret = sr;
            p->cur_msd = sm;
            p->cur_this = st;
            expect_semi(p);
        } else {
            diag_error(cur(p)->span, "expected '(', '=>' or a new member after member name");
            break;
        }
    }
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close struct body");
    }
    if (sd != NULL)
        struct_finish(sd);
    if (sd != NULL && is_class) {
        if (class_finish_vtable(p->ty, sd) != 0) {
            diag_error(start, "class '%s' has an 'override' with no matching base virtual method",
                       name);
        }
    }
    Stmt *s = new_stmt(p, S_STRUCT, start);
    s->sdef = sd;
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    Token *t = cur(p);

    /* A function declared inside another function, written where a statement
     * goes. `at_func_decl` requires a type, then a name, then '(', so this
     * cannot swallow `Console.WriteLog(x);` (no leading type), `var v = 1;` (`var` is not a
     * type token), or `P p = ...;` (an '=' where the '(' would be). It also
     * tolerates a leading `extern`/`export`, which parse_func then rejects. */
    /* A `g`/`l`-prefixed declaration, before at_func_decl: `gPoint p = new
     * Point(1, 2);` is a type-then-name-then-`=`, and at_func_decl wants a `(`
     * where the `=` is, so it declines this on its own -- but checking here
     * first means the error message is about the prefixed form rather than
     * whatever at_func_decl decides to say about something that is not one. */
    if (at_prefixed_decl(p))
        return parse_var_decl_kind(p, cur(p)->text[0]);

    if (at_func_decl(p)) {
        parse_func(p, 1);
        /* The declaration emits nothing where it stands; the function is emitted
         * as a hoisted one, under a symbol of its own. An empty block is the
         * honest shape for a statement that produces no code. */
        return new_stmt(p, S_BLOCK, t->span);
    }

    switch (t->kind) {
    case T_KW_IF: {
        Span start = t->span;
        advance(p);
        match(p, T_LPAREN);
        Expr *cond = parse_expr(p);
        if (!is_kind(cond->type, TK_BOOL) && !is_unk(cond->type)) {
            diag_error(start, "condition must be 'bool' but got '%s'",
                       type_name(p->ty, cond->type));
        }
        match(p, T_RPAREN);
        Stmt *s = new_stmt(p, S_IF, start);
        s->cond = cond;
        s->body = parse_stmt(p);
        if (match(p, T_KW_ELSE))
            s->orelse = parse_stmt(p);
        return s;
    }
    case T_KW_WHILE: {
        Span start = t->span;
        advance(p);
        match(p, T_LPAREN);
        Expr *cond = parse_expr(p);
        if (!is_kind(cond->type, TK_BOOL) && !is_unk(cond->type)) {
            diag_error(start, "condition must be 'bool' but got '%s'",
                       type_name(p->ty, cond->type));
        }
        match(p, T_RPAREN);
        Stmt *s = new_stmt(p, S_WHILE, start);
        s->cond = cond;
        p->loop_depth++;
        if (p->nloops < MAX_DROP_SCOPES)
            p->loop_top[p->nloops++] = p->nopen;
        s->body = parse_stmt(p);
        if (p->nloops > 0)
            p->nloops--;
        p->loop_depth--;
        return s;
    }
    case T_KW_FOR: {
        Span start = t->span;
        advance(p);
        match(p, T_LPAREN);
        /* `for (auto x : v)` is a range-for, not a three-part for, so it is
         * recognized before the `;` is expected. A C-style `for` cannot begin
         * with a colon, so looking for one here costs nothing. */
        {
            int save = p->pos;
            if (at_range_for(p)) {
                p->pos = save;
                return parse_range_for(p, start);
            }
            p->pos = save;
        }
        Stmt *init = NULL;
        if (!at(p, T_SEMI)) {
            if (at(p, T_KW_CONST)) {
                init = parse_const_decl(p); /* consumes its ';' */
            } else if (at(p, T_KW_VAR) || is_type_token(cur(p)->kind)) {
                init = parse_var_decl(p); /* consumes its ';' */
            } else {
                Expr *e = parse_expr(p);
                init = new_stmt(p, S_EXPR, start);
                init->expr = e;
                match(p, T_SEMI);
            }
        } else {
            advance(p);
        }
        Expr *cond = NULL;
        if (!at(p, T_SEMI))
            cond = parse_expr(p);
        match(p, T_SEMI);
        Expr *step = NULL;
        if (!at(p, T_RPAREN))
            step = parse_expr(p);
        match(p, T_RPAREN);
        Stmt *s = new_stmt(p, S_FOR, start);
        s->for_init = init;
        s->cond = cond;
        s->for_step = step;
        p->loop_depth++;
        if (p->nloops < MAX_DROP_SCOPES)
            p->loop_top[p->nloops++] = p->nopen;
        s->body = parse_stmt(p);
        if (p->nloops > 0)
            p->nloops--;
        p->loop_depth--;
        return s;
    }
    case T_KW_FOREACH:
        return parse_foreach(p, t->span);
    case T_KW_RETURN: {
        Span start = t->span;
        advance(p);
        Stmt *s = new_stmt(p, S_RETURN, start);
        if (at(p, T_SEMI)) {
            /* A bare `return` is an ordinary way out of a void function, and an
             * error in one that owes a value.
             *
             * It used to compile silently in both. There is no expression to copy
             * into the return slot, so the slot keeps whatever the frame held,
             * and the caller reads that as the answer: a function returning `int`
             * printed a number nobody computed, and one returning a struct returned
             * whatever was in the result buffer.
             *
             * The diagnostic names what is missing rather than only the return
             * type, because "cannot return without a value" on its own reads as a
             * complaint about the statement when what is wanted is a value. */
            if (!is_unk(p->cur_ret) && !is_kind(p->cur_ret, TK_VOID))
                diag_error(start, "cannot return without a value from a function returning '%s'",
                           type_name(p->ty, p->cur_ret));
            s->expr = NULL;
        } else {
            /* The function's return type tells `Ok(...)`/`Err(...)` what to
             * build, which is what makes `return Ok(3);` work unannotated. */
            Type *saved_expect = p->expect_result;
            p->expect_result = type_is_result(p->cur_ret) ? p->cur_ret : NULL;
            s->expr = parse_expr(p);
            p->expect_result = saved_expect;
            if (is_kind(p->cur_ret, TK_IFACE))
                s->expr = to_iface(p, s->expr, p->cur_ret, start);
            /* A returned string is handed to the caller, so it has to be a value
             * nothing else holds. Returning a local's bytes directly would alias
             * them, and the local's own teardown would then free what the caller
             * is reading. This is the one place a copy is required rather than
             * optional, and it is what lets the caller's side treat every call
             * result as fresh. */
            s->expr = own_string_copy(p, s->expr);
            s->expr = claim_string_value(s->expr);
            if (!is_unk(s->expr->type) && !is_unk(p->cur_ret) &&
                !type_equals(s->expr->type, p->cur_ret)) {
                diag_error(start, "cannot return '%s' from function returning '%s'",
                           type_name(p->ty, s->expr->type), type_name(p->ty, p->cur_ret));
            }
        }
        expect_semi(p);
        /* A `return` leaves every scope it is inside, and each of them has to
         * destroy what it owns first, innermost first. The innermost scope open
         * here is recorded on the return, and the pass walks up from it to the
         * function's own parameter scope -- which is where the walk stops,
         * because a parameter is the caller's value and tearing it down here
         * would destroy an object this function was only lent.
         *
         * The body's locals are in a *different* scope from the parameters, a
         * block scope pushed for the function body itself. Recording the
         * function's scope found only the parameters, so a return destroyed
         * nothing the function had actually declared. */
        s->ret_scope = p->scope;
        return s;
    }
    case T_LBRACE:
        return parse_block(p);
    case T_KW_VAR:
        return parse_var_decl(p);
    case T_KW_CONST:
        return parse_const_decl(p);
    /* A type declared inside a function. It is queued with the other hoisted
     * declarations rather than emitted where it stands: a class's vtable is
     * emitted by walking the top-level items, so a local class that stayed in
     * this body would have no vtable and a virtual call through it would jump
     * through a null slot. The declaration itself produces no code, so the
     * statement it parsed to is an empty block. */
    case T_KW_INTERFACE: {
        Stmt *s = parse_interface_decl(p);
        add_pending(p, s);
        return new_stmt(p, S_BLOCK, t->span);
    }
    case T_KW_CLASS:
    case T_KW_STRUCT:
    case T_KW_ENUM: {
        Stmt *s = at(p, T_KW_ENUM) ? parse_enum_decl(p) : parse_struct_decl(p, at(p, T_KW_CLASS));
        add_pending(p, s);
        return new_stmt(p, S_BLOCK, t->span);
    }
    case T_KW_BREAK: {
        Span bs = t->span;
        advance(p);
        expect_semi(p);
        if (p->loop_depth == 0)
            diag_error(bs, "'break' is only valid inside a loop");
        Stmt *b = new_stmt(p, S_BREAK, bs);
        b->ret_scope = p->scope;
        /* Resolved here, not in the destructor pass: the pass runs once the whole
         * unit is parsed, by which time every scope has been popped and
         * `p->open_scopes` is empty. A floor read then is always "none", the walk
         * runs to the top of the function, and the jump destroys the caller's
         * locals -- which is how `drop 202` came out followed by a second, wrong
         * `drop 7`. */
        int fl = p->nloops > 0 ? p->loop_top[p->nloops - 1] : 0;
        b->jump_floor = fl > 0 ? p->open_scopes[fl - 1] : NULL;
        return b;
    }
    case T_KW_CONTINUE: {
        Span cs = t->span;
        advance(p);
        expect_semi(p);
        if (p->loop_depth == 0)
            diag_error(cs, "'continue' is only valid inside a loop");
        Stmt *c = new_stmt(p, S_CONTINUE, cs);
        c->ret_scope = p->scope;
        int fl = p->nloops > 0 ? p->loop_top[p->nloops - 1] : 0;
        c->jump_floor = fl > 0 ? p->open_scopes[fl - 1] : NULL;
        return c;
    }
    default:
        break;
    }

    /* An explicitly typed local: `Type name = expr;`.
     *
     * This used to be reachable only for the keyword types. A named type -- the
     * spelling a struct is declared and used with -- never got here: the code
     * below treated any leading identifier as an unknown type and reported it
     * without a lookup, so `P r = new P();` could not be written at all, and
     * `P* q = null;` fell through to the expression parser and read as a
     * variable named `P`. Both are fixed by resolving the name first. */
    if ((at_typed_decl(p) && base_type_or_name(p, cur(p)) != NULL) ||
        (at_typed_decl(p) && at_ns_typed(p)))
        return parse_var_decl(p);

    if (is_type_token(cur(p)->kind)) {
        return parse_var_decl(p);
    }
    if (t->kind == T_IDENT && at_typed_decl(p)) {
        /* The name is in declaration position but is not a type. Say which one,
         * then skip the declaration so the rest of the block still parses. */
        {
            char dpath[256];
            int dend = 0;
            if (ns_scan_path(p, p->pos, dpath, sizeof dpath, &dend)) {
                diag_error_code(t->span, "unknown_type", "unknown type '%s'", dpath);
                int guard = 0;
                while (!at(p, T_SEMI) && !at(p, T_EOF) && !at(p, T_RBRACE) && guard++ < 256)
                    advance(p);
                match(p, T_SEMI);
                return new_stmt(p, S_EXPR, t->span);
            }
        }
        suggest_in_scope(p, SK_TYPE, "type", t->text);
        diag_error_code(t->span, "unknown_type", "unknown type '%s'", t->text);
        int guard = 0;
        while (!at(p, T_SEMI) && !at(p, T_EOF) && !at(p, T_RBRACE) && guard++ < 256)
            advance(p);
        match(p, T_SEMI);
        return new_stmt(p, S_EXPR, t->span);
    }

    Span start = t->span;
    Expr *e = parse_expr(p);
    expect_semi(p);
    Stmt *s = new_stmt(p, S_EXPR, start);
    s->expr = e;
    return s;
}

static void parse_top_item(Parser *p, Arena *arena, Stmt ***pitems, int *pn, int *pcap,
                           int *phas_main, Span *pmain_span, int *ptop_stmts);
static void parse_namespace(Parser *p, Arena *arena, Stmt ***pitems, int *pn, int *pcap,
                            int *phas_main, Span *pmain_span, int *ptop_stmts);
/* One top-level item: a function, a type, a const, a global, or a bare
 * statement. Shared by the program loop and by `namespace` bodies, so the two
 * accept exactly the same members. `has_main`/`top_stmts` are the program's
 * entry-point bookkeeping; a `fn main` only counts as the entry point when it
 * is declared outside any namespace (checked inside parse_func). */
static void parse_top_item(Parser *p, Arena *arena, Stmt ***pitems, int *pn, int *pcap,
                           int *phas_main, Span *pmain_span, int *ptop_stmts) {
    Stmt **items = *pitems;
    int n = *pn, cap = *pcap;
    if (n == cap) {
        int ncap = cap * 2;
        Stmt **ni = arena_alloc_array(arena, (size_t)ncap, sizeof(Stmt *));
        memcpy(ni, items, (size_t)n * sizeof(Stmt *));
        items = ni;
        cap = ncap;
    }
    if (at(p, T_KW_NAMESPACE)) {
        parse_namespace(p, arena, pitems, pn, pcap, phas_main, pmain_span, ptop_stmts);
        return;
    }
    if (at_func_decl(p)) {
        Stmt *fn = parse_func(p, 0);
        if (fn->is_entry) {
            if (*phas_main) {
                diag_error(fn->span,
                           "a second function named 'main' is declared here; the first is at "
                           "%s:%d, and a program has one entry point",
                           pmain_span->file != NULL ? pmain_span->file : "another file",
                           pmain_span->line);
            } else {
                *phas_main = 1;
                *pmain_span = fn->span;
            }
        }
        items[n++] = fn;
    } else if ((at(p, T_KW_CLASS) || at(p, T_KW_STRUCT)) && peek(p, 1)->kind == T_IDENT &&
               peek(p, 2)->kind == T_LT &&
               (find_gtype(p, peek(p, 1)->text) != NULL ||
                ns_lookup(p, peek(p, 1)->text, look_gtype) != NULL ||
                find_gtype(p, ns_join(p, ns_here(p), peek(p, 1)->text)) != NULL)) {
        /* A generic declaration is a template, and a template is not code.
         * It emits nothing and defines nothing until something asks for an
         * instance, which re-parses these very tokens. Stepping over the
         * whole thing is therefore not a shortcut: parsing it here would
         * define a `Vec` that the instances have nothing to do with. */
        p->pos = skip_generic_type_decl(p);
        *pitems = items;
        *pn = n;
        *pcap = cap;
        return;
    } else if (at(p, T_KW_CLASS)) {
        items[n++] = parse_struct_decl(p, 1); /* a class decl */
    } else if (at(p, T_KW_STRUCT)) {
        items[n++] = parse_struct_decl(p, 0); /* a type decl, not a statement */
    } else if (at(p, T_KW_INTERFACE)) {
        items[n++] = parse_interface_decl(p);
    } else if (at(p, T_KW_ENUM)) {
        items[n++] = parse_enum_decl(p); /* a type decl, not a statement */
    } else if (at(p, T_KW_CONST)) {
        /* A top-level const is a declaration, not a top-level statement, so
         * it may sit alongside an explicit main(). It folds to a literal
         * and emits no code. */
        items[n++] = parse_const_decl(p);
    } else if (at_prefixed_decl(p) && cur(p)->text[0] == 'g') {
        /* Same reasoning, and it matters for exactly the same reason: a
         * `gvar` is a declaration with storage of its own, emitted as data
         * and as an initializer, so counting it as a top-level statement
         * would put a project that declares one global beside an explicit
         * main() into the "cannot combine" error -- which is the right
         * error for a statement and the wrong one here.
         *
         * An `lvar` at the top level is the other thing, and it stays a
         * statement: at file scope an lvar is a plain var, so it belongs in
         * the synthesized entry exactly as a var does. */
        items[n++] = parse_var_decl_kind(p, 'g');
    } else {
        if (p->ns_prefix != NULL && p->ns_prefix[0] != '\0') {
            diag_error(cur(p)->span,
                       "only declarations may appear inside 'namespace %s': move this statement "
                       "into a function",
                       p->ns_prefix);
            advance(p);
            *pitems = items;
            *pn = n;
            *pcap = cap;
            return;
        }
        items[n++] = parse_stmt(p);
        (*ptop_stmts)++;
    }
    *pitems = items;
    *pn = n;
    *pcap = cap;
}

/* `namespace netlink { ... }`: everything declared inside registers under
 * "netlink.", so `class Lobby` becomes the type `netlink.Lobby` and
 * `fn connect` the function `netlink.connect`. A single identifier nests
 * relative to the enclosing block; a dotted header (`namespace a.b`) is
 * absolute, which is how two files merge into one namespace by writing the
 * same header -- and how `import "f.z" as netlink` (synthesized around the
 * file's tokens) shares this one implementation. */
static void parse_namespace(Parser *p, Arena *arena, Stmt ***pitems, int *pn, int *pcap,
                            int *phas_main, Span *pmain_span, int *ptop_stmts) {
    Span start = cur(p)->span;
    advance(p); /* 'namespace' */
    if (!at(p, T_IDENT)) {
        diag_error(cur(p)->span, "expected a namespace name after 'namespace'");
        return;
    }
    char name[256];
    size_t n = 0;
    int dotted = 0;
    while (at(p, T_IDENT)) {
        const char *t = cur(p)->text;
        if (n > 0)
            dotted = 1;
        size_t tl = strlen(t);
        if (n + tl + 1 >= sizeof name)
            break;
        if (n > 0)
            name[n++] = '.';
        memcpy(name + n, t, tl);
        n += tl;
        advance(p);
        if (at(p, T_DOT) && peek(p, 1)->kind == T_IDENT) {
            advance(p);
            continue;
        }
        break;
    }
    name[n] = '\0';
    if (!match(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open namespace body");
        return;
    }
    char full[256];
    if (dotted || p->ns_prefix == NULL || p->ns_prefix[0] == '\0') {
        size_t fl = strlen(name);
        if (fl >= sizeof full) {
            diag_error(start, "namespace name is too long");
            return;
        }
        memcpy(full, name, fl + 1);
    } else {
        int need = snprintf(full, sizeof full, "%s.%s", p->ns_prefix, name);
        if (need < 0 || (size_t)need >= sizeof full) {
            diag_error(start, "namespace name is too long");
            return;
        }
    }
    ns_declare(p, full);
    char *saved = p->ns_prefix;
    p->ns_prefix = arena_strdup(p->arena, full);
    while (!at(p, T_RBRACE) && !at(p, T_EOF))
        parse_top_item(p, arena, pitems, pn, pcap, phas_main, pmain_span, ptop_stmts);
    if (!match(p, T_RBRACE))
        diag_error(cur(p)->span, "expected '}' to close namespace '%s'", full);
    p->ns_prefix = saved;
}

Stmt *parse_program(Arena *arena, Token *toks, int ntoks, StringTable *strings, int opt_level,
                    Surface *surf) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.arena = arena;
    p.surf = surf;
    p.toks = toks;
    p.ntoks = ntoks;
    {
        /* On the arena, not the stack: the destructor pass runs after the whole
         * file is read and allocates types through this context, and a
         * `TypeCtx` on this frame would be a dangling pointer by then -- which
         * is a segfault in type_ptr with no diagnostic and no output. */
        TypeCtx *ctx = arena_alloc(arena, sizeof(TypeCtx));
        typectx_init(ctx, arena);
        p.ty = ctx;
    }
    p.strings = strings;
    {
        /* On the parser's own storage, not the stack: the list outlives this
         * frame in the sense that every shallow copy of the parser -- the
         * signature pre-scan, parse_type_at -- shares this pointer, and the
         * compile finishes with it. */
        PendingList *pending = arena_alloc(arena, sizeof(PendingList));
        pending->items = NULL;
        pending->count = 0;
        pending->cap = 0;
        p.pending = pending;
    }
    p.cur_ret = NULL;
    p.opt_level = opt_level;

    prescan_namespaces(&p);
    prescan_struct_names(&p);
    prescan_struct_fields(&p);
    prescan_signatures(&p);
    p.ns_prefix = NULL;

    Stmt *program = new_stmt(&p, S_BLOCK, cur(&p)->span);
    scope_push(&p);
    p.next_offset = 0;

    int cap = 8, n = 0;
    Stmt **items = arena_alloc_array(arena, (size_t)cap, sizeof(Stmt *));
    int has_main = 0;
    /* Where `main` was found. Two entry points in one program was only
     * unreachable because one program was one file; a project compiles every
     * file under its root as one unit, so two files that each define `main` now
     * reach codegen together and emit `z_main` twice. The assembler reports it as
     * "symbol is already defined", which names neither file. */
    Span main_span = {0};
    int top_stmts = 0;

    while (!at(&p, T_EOF)) {
        parse_top_item(&p, arena, &items, &n, &cap, &has_main, &main_span, &top_stmts);
    }

    /* A lambda marks the captured declarations in the body of the function that
     * encloses it, and it does that by walking an S_BLOCK. Top-level statements
     * are appended to `items` one at a time and only gathered into a block below,
     * after the whole file is read, so nothing walked them: a `var` a lambda
     * captured kept no box, and the closure's environment read the raw frame slot
     * that the initializer was supposed to fill. No parse_func runs for a
     * top-level program, so p->captured is still the whole file's list here. */
    for (int i = 0; i < n; i++)
        mark_boxed_stmt(&p, items[i]);

    /* Append monomorphized generic instances so codegen emits them. */
    for (int i = 0; i < p.pending->count; i++) {
        if (n == cap) {
            int ncap = cap * 2;
            Stmt **ni = arena_alloc_array(arena, (size_t)ncap, sizeof(Stmt *));
            memcpy(ni, items, (size_t)n * sizeof(Stmt *));
            items = ni;
            cap = ncap;
        }
        items[n++] = p.pending->items[i];
    }

    if (has_main && top_stmts > 0) {
        diag_error(items[0]->span, "cannot combine top-level statements with a 'main' function");
    }

    if (!has_main) {
        Stmt **body_items = arena_alloc_array(arena, (size_t)(n > 0 ? n : 1), sizeof(Stmt *));
        int bi = 0;
        for (int i = 0; i < n; i++) {
            if (items[i]->kind != S_FUNC)
                body_items[bi++] = items[i];
        }
        Stmt *body = new_stmt(&p, S_BLOCK, items[0] ? items[0]->span : cur(&p)->span);
        body->items = body_items;
        body->nitems = bi;

        Stmt *entry = new_stmt(&p, S_FUNC, body->span);
        entry->fname = arena_strdup(arena, "main");
        entry->ret_type = type_void(p.ty);
        entry->fbody = body;
        entry->is_entry = 1;
        entry->locals_bytes = p.next_offset;

        if (n == cap) {
            int ncap = cap * 2;
            Stmt **ni = arena_alloc_array(arena, (size_t)ncap, sizeof(Stmt *));
            memcpy(ni, items, (size_t)n * sizeof(Stmt *));
            items = ni;
            cap = ncap;
        }
        items[n++] = entry;
    }

    program->items = items;
    program->nitems = n;
    /* Destructors are expanded first, once every type is complete -- see the
     * comment on emit_drops. The order matters: the inliner has to be able to
     * see the destructor calls to know it must leave the body alone, so a pass
     * that ran afterwards would find bodies it had already spliced and would
     * have no way to tell what it had done. Running it second was not merely
     * late, it meant every destructor call the inliner had already inlined away
     * was simply gone, and a program with a destructor in an inlined function
     * ran its destructors never. */
    emit_drops(&p, program);
    /* Inlining runs over the finished program, so a call is inlinable no matter
     * which order the two functions were written in, and the synthesized entry is
     * a function like any other as far as it is concerned. */
    inline_program(&p, program);
    report_unused_locals(&p);
    scope_pop(&p);
    /* Handed to the driver through the program, because `p` is about to go out
     * of scope and the globals list would go with it. Reversed first: the list is
     * built by prepending, so it currently reads newest-first, and both the
     * emitted table and z_ginit want declaration order -- the order a reader can
     * predict, and the order initializers run in. */
    program->globals_head = NULL;
    {
        GlobalDef *reversed = NULL;
        for (GlobalDef *g = p.globals; g != NULL;) {
            GlobalDef *next = g->next;
            g->next = reversed;
            reversed = g;
            g = next;
        }
        program->globals_head = reversed;
        /* gidx was assigned as the list grew, so after the reversal the two
         * disagree. Re-stamped so index i names the i-th declaration, which is
         * what a use site recorded when it resolved. */
        int i = 0;
        for (GlobalDef *g = reversed; g != NULL; g = g->next)
            g->gidx = i++;
    }
    return program;
}
