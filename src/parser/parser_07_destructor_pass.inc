/* ---- the destructor pass ----
 *
 * Runs after the whole program is parsed, because whether a value owns
 * something is not knowable while parsing: a class body is read in a later pass
 * than the functions that mention it, so deciding at the declaration made the
 * answer depend on the order the parser happened to walk the file.
 *
 * Four shapes are expanded:
 *
 *   - a block's own scope, at the end of the block
 *   - a `return`, for the function body's scope, immediately before the return
 *     (handed to the return as `ret_drops`, emitted after the value is copied)
 *   - a `?`, for the scopes it leaves, as `try_drops` on the expression
 *   - a `break`/`continue`, for the scopes the jump leaves up to its
 *     `jump_floor`, spliced in ahead of the jump
 *
 * A `return` inside a block already sits after that block's own drops, because
 * the block's are appended at its end and the return is inside it -- so the
 * order is inner block first, function body last, which is the order the frame
 * implies.
 */

/* The destructor calls for one scope, in reverse declaration order, or NULL when
 * the scope owns nothing.
 *
 * The receiver is the value itself for a class (a class variable already holds
 * the object pointer, which is what a method's `this` is) and the address of the
 * frame slot for a struct. Passing the address of a class's *pointer* variable
 * would make `this` point at the frame slot rather than the object, so every
 * field read inside a destructor came back as whatever the frame held. */
static Stmt *build_drops(Parser *p, Scope *sc, Span span, const Span *limit) {
    if (sc == NULL || sc->ndrops == 0)
        return NULL;
    int n = sc->ndrops;
    Stmt **items = arena_alloc_array(p->arena, (size_t)n, sizeof(Stmt *));
    int k = 0;
    for (int i = 0; i < n; i++) {
        Var *v = sc->drops[n - 1 - i];
        /* A jump (`break`/`continue`) or a `return` destroys only the locals
         * whose initializer has already run on the path that reaches it. A local
         * declared after the jump in the same block has not run yet, and its slot
         * still holds the *previous* iteration's value -- already released at the
         * end of that iteration -- so releasing it here is a double free. The
         * locals declared before the jump are exactly those written before it, so
         * the jump's own position is the cutoff. `limit` is NULL where there is
         * nothing to cut: the end of a block has no declaration after it. */
        if (limit != NULL && v->decl_span.start > limit->start &&
            (limit->file == NULL || v->decl_span.file == NULL ||
             strcmp(v->decl_span.file, limit->file) == 0))
            continue;
        /* A moved-from local is not owned by anything. `move` is how a value is
         * handed on rather than shared, and the handover empties the source, so
         * running a destructor on it afterwards destroys the caller's value.
         *
         * For a string that happened to be harmless, because `z_str_free` on an
         * empty pointer returns and the source was already emptied. For a class
         * it was not: `move` clears the pointer and the destructor call was
         * still emitted, so `return move out;` called `~Vec` on null and the
         * read of the vtable took the process down. */
        if (v->is_moved)
            continue;
        v->needs_drop = type_needs_drop(v->type);
        if (v->needs_drop)
            v->used = 1; /* existing is a use: see the unused-local warning */
        if (!v->needs_drop)
            continue;
        Type *st = v->type;
        if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
            st = st->base;
        /* A string has no user destructor and still owns its bytes, so its drop
         * is a call to the runtime rather than to a method the program wrote.
         * It goes in the same list and in the same order, which is the point of
         * computing the drop list from the type rather than from a flag. */
        if (is_kind(v->type, TK_STRING)) {
            Expr *slot = new_expr(p, E_VAR, v->decl_span);
            slot->name = arena_strdup(p->arena, v->name);
            slot->slot = v->offset;
            slot->type = v->type;
            /* A captured variable's slot holds a pointer to a heap cell, so the
             * reference has to carry `boxed` or codegen reads the slot and hands
             * `z_str_free` the box. That is not a leak: `z_str_free` subtracts the
             * header size to read the header, so a heap address given to it is a
             * read eight bytes before the allocation, which AddressSanitizer calls
             * a heap-buffer-overflow and the program calls a segfault. */
            slot->boxed = v->captured;
            Expr **sargs = arena_alloc_array(p->arena, 1, sizeof(Expr *));
            sargs[0] = slot;
            Expr *scall = new_expr(p, E_CALL, v->decl_span);
            /* The runtime symbol, called verbatim, which is the same shape a
             * call to the `str_free` builtin takes once the parser has resolved
             * it. The drop is generated after type checking has finished, so
             * there is no argument to check here: the argument is the variable's
             * own slot, which `type_needs_drop` has already decided is a
             * string. */
            scall->name = arena_strdup(p->arena, "z_str_free");
            scall->is_extern = 1;
            scall->args = sargs;
            scall->nargs = 1;
            scall->type = type_void(p->ty);
            Stmt *ss = new_stmt(p, S_EXPR, v->decl_span);
            ss->expr = scall;
            items[k++] = ss;
            continue;
        }
        if (!is_kind(st, TK_STRUCT) || st->sdef == NULL)
            continue;
        StructDef *sd = st->sdef;
        StructMethod *m = struct_find_dtor(sd);
        if (m == NULL)
            continue; /* a field was owning, not the value itself */
        char *mang = arena_alloc(p->arena, strlen(sd->name) + strlen(m->name) + 3);
        snprintf(mang, strlen(sd->name) + strlen(m->name) + 3, "%s__%s", sd->name, m->name);
        Expr *slot = new_expr(p, E_VAR, v->decl_span);
        slot->name = arena_strdup(p->arena, v->name);
        slot->slot = v->offset;
        slot->type = v->type;
        /* Deliberately *not* marked boxed, unlike the string above.
         *
         * For a struct it makes no difference: the receiver is `&slot`, and a
         * captured struct's cell holds a copy of the bytes, so the box pointer is
         * already the address the destructor wants.
         *
         * For a class it does, and marking it reaches the destructor that had
         * been silently skipped, which is right -- but it then runs on an object
         * whose string fields can be aliased out through a closure:
         *
         *     var b = new Boxed("one");
         *     var h = () => b.label;
         *     Console.WriteLog(h());   // the temporary is released afterwards
         *                              // and it is b's own field
         *
         * `~Boxed` then releases the same allocation a second time. Unmarked, the
         * destructor is never reached and the object leaks instead, which is the
         * smaller fault and the one this leaves in place. The aliasing is a
         * separate defect and fixing it is not a one-line change to this pass. */
        Expr *recv;
        if (sd->is_class) {
            recv = slot;
            recv->type = type_ptr(p->ty, st);
        } else {
            recv = new_expr(p, E_ADDR, v->decl_span);
            recv->lhs = slot;
            recv->type = type_ptr(p->ty, st);
        }
        Expr **args = arena_alloc_array(p->arena, 1, sizeof(Expr *));
        args[0] = recv;
        Expr *call = new_expr(p, E_CALL, v->decl_span);
        call->name = mang;
        call->args = args;
        call->nargs = 1;
        call->type = type_void(p->ty);
        Stmt *s = new_stmt(p, S_EXPR, v->decl_span);
        s->expr = call;
        items[k++] = s;
    }
    if (k == 0)
        return NULL;
    Stmt *b = new_stmt(p, S_BLOCK, span);
    b->items = items;
    b->nitems = k;
    return b;
}

/* The destructor calls for every scope a `return` is leaving, innermost scope
 * first and, within a scope, reverse declaration order -- or NULL when the
 * return leaves nothing that owns.
 *
 * The walk starts at the scope the return was written in and climbs to the
 * function's parameter scope, which it stops *at* rather than destroys: a
 * parameter belongs to the caller, and a callee that tore it down would be
 * freeing something it was only lent. Everything between is this function's
 * own, and a return unwinds all of it.
 *
 * This is a chain rather than a single scope because a return is not always in
 * the function's outermost block. A `return` inside a nested block has to
 * destroy that block's values as well as the function's, and it is the
 * *innermost* scope's values that are destroyed first. */
/* The drops for the scopes between `inner` and a floor, exclusive.
 *
 * `build_drops_chain` walks up to the function's parameter scope because a return
 * leaves the whole function. A `break` leaves less: the scopes between where it
 * was written and the loop body, and the parameter scope is nowhere near. So this
 * takes the floor as a count of open scopes rather than a Scope pointer, and stops
 * there. */
static Stmt *build_drops_to(Parser *p, Scope *inner, Scope *stop, Span span) {
    if (inner == NULL)
        return NULL;
    int cap = 0;
    for (Scope *sc = inner; sc != NULL && sc != stop; sc = sc->parent)
        cap += sc->ndrops;
    if (cap == 0)
        return NULL;
    Stmt **items = arena_alloc_array(p->arena, (size_t)cap, sizeof(Stmt *));
    int k = 0;
    for (Scope *sc = inner; sc != NULL && sc != stop; sc = sc->parent) {
        Stmt *d = build_drops(p, sc, span, &span);
        if (d == NULL)
            continue;
        for (int j = 0; j < d->nitems; j++)
            items[k++] = d->items[j];
    }
    if (k == 0)
        return NULL;
    Stmt *b = new_stmt(p, S_BLOCK, span);
    b->items = items;
    b->nitems = k;
    return b;
}

/* The `?` case, at the `?`. */
static Stmt *build_drops_chain(Parser *p, Scope *inner, Span span) {
    if (inner == NULL)
        return NULL;
    /* Count first so the list is allocated once. Upper bound: one destructor
     * call per recorded local across every scope on the chain. */
    int cap = 0;
    for (Scope *sc = inner; sc != NULL; sc = sc->parent)
        cap += sc->ndrops;
    if (cap == 0)
        return NULL;
    Stmt **items = arena_alloc_array(p->arena, (size_t)cap, sizeof(Stmt *));
    int k = 0;
    for (Scope *sc = inner; sc != NULL && !sc->is_fn_body; sc = sc->parent) {
        Stmt *d = build_drops(p, sc, span, &span);
        if (d == NULL)
            continue;
        for (int j = 0; j < d->nitems; j++)
            items[k++] = d->items[j];
    }
    if (k == 0)
        return NULL;
    Stmt *b = new_stmt(p, S_BLOCK, span);
    b->items = items;
    b->nitems = k;
    return b;
}

/* Appends `d`'s statements to a statement list, returning the new list and
 * count through the pointers so a caller's list can grow. */
static void list_append(Parser *p, Stmt ***items_io, int *n_io, Stmt *d) {
    if (d == NULL || d->nitems == 0)
        return;
    int n = *n_io;
    int m = n + d->nitems;
    Stmt **out = arena_alloc_array(p->arena, (size_t)m, sizeof(Stmt *));
    for (int i = 0; i < n; i++)
        out[i] = (*items_io)[i];
    for (int i = 0; i < d->nitems; i++)
        out[n + i] = d->items[i];
    *items_io = out;
    *n_io = m;
}

/* `return x;` of a local that owns a destructor is a use-after-free waiting to
 * happen. The scope's destructor runs on the way out, so the caller receives a
 * value that was already torn down, and then the caller's own scope tears it
 * down a second time. `return move x;` is how the value is handed on, and it is
 * the only form that is sound, so the plain form is rejected here with a note
 * saying what to write instead.
 *
 * Strings are exempt: a return copies a borrowed string, so the caller never
 * sees the local's bytes. Only the name is resolved, and only up to the
 * function's own body, the same rule `resolve_var` applies to reads. */
static void check_return_of_owned_local(Parser *p, Stmt *s) {
    (void)p;
    Expr *e = s->expr;
    if (e == NULL || e->kind != E_VAR || e->name == NULL)
        return;
    Var *found = NULL;
    for (Scope *sc = s->ret_scope; sc != NULL && found == NULL; sc = sc->parent) {
        for (Var *v = sc->vars; v != NULL; v = v->next) {
            if (strcmp(v->name, e->name) == 0) {
                found = v;
                break;
            }
        }
        if (sc->is_fn_body)
            break;
    }
    if (found == NULL || found->is_param || is_kind(found->type, TK_STRING))
        return;
    if (!type_needs_drop(found->type))
        return;
    diag_note_at(found->decl_span,
                 "'%s' is declared here; write 'return move %s;' to hand it to the caller",
                 found->name, found->name);
    diag_error_code(
        s->span, "return_owned_local",
        "cannot return '%s' directly: its destructor would run before the caller gets it",
        found->name);
}

/* A non-void function whose body can reach the closing brace hands the caller
 * whatever the return register happened to hold -- the leftover of the last
 * call the body made, usually -- with nothing at the source that says so. The
 * value then flows on as if it had been computed. `bare_return_nonvoid` covers
 * the same mistake written as an explicit `return;`; this is the silent form,
 * where the reader never wrote the word at all.
 *
 * `int main` is the one exemption, and it is C's: running off the end of main
 * returns 0. The parser marks the function so codegen can zero the return
 * register on the fall-through path; every other non-void function that can
 * fall through is rejected here, before codegen ever sees it. */

/* True when control can reach the end of this statement. A statement list is
 * scanned in full rather than only its last entry, so
 *     return x;
 *     <dead code>
 * counts as a body that cannot fall through: the last statement is unreachable,
 * but control still leaves at the return above it. `stmt_terminates` cannot
 * answer this -- it looks only at the last statement -- which is why this is a
 * separate walk rather than a change to it. */
static int stmt_falls_through(Stmt *s) {
    if (s == NULL)
        return 1;
    switch (s->kind) {
    case S_RETURN:
    case S_BREAK:
    case S_CONTINUE:
        return 0;
    case S_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            if (!stmt_falls_through(s->items[i]))
                return 0;
        return 1;
    case S_IF:
        /* Without an `else` the condition can be false and control passes on. */
        if (s->orelse == NULL)
            return 1;
        return stmt_falls_through(s->body) || stmt_falls_through(s->orelse);
    default:
        return 1;
    }
}

static void check_falls_off_end(Stmt *fn, Type *ret, int is_entry, Span span, const char *name) {
    if (fn->fbody == NULL || is_unk(ret) || is_kind(ret, TK_VOID))
        return;
    if (!stmt_falls_through(fn->fbody))
        return;
    fn->falls_off = 1;
    if (is_entry && is_kind(ret, TK_INT))
        return;
    diag_error_code(span, "missing_return",
                    "function '%s' can reach its closing brace without returning a value", name);
}

/* Expands every block scope and every return in a statement list. Inner first,
 * so a nested block's values are destroyed before the enclosing block's.
 *
 * Every statement is visited exactly once. A block is rewritten where it stands
 * -- its drops are appended to its own item list -- and the walk then moves on,
 * because what gets appended is a run of destructor calls and none of those is
 * itself a block or a return. Revisiting the block is not merely wasteful:
 * `build_drops` answers from the scope, not from the block, so a second call
 * returns the same drops again and appends them a second time, forever. */
static int emit_drops_in(Parser *p, Stmt **items, int n) {
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        Stmt *s = items[i];
        if (s == NULL)
            continue;
        if (s->kind == S_BLOCK) {
            /* A top-level block is reached twice: once as an item of the program
             * list and once inside the entry function synthesized for those same
             * statements. Both walks would append this scope's releases to the
             * block, so the second free of every string live at the end. The flag
             * makes the second visit a no-op. */
            if (s->drops_done)
                continue;
            s->drops_done = 1;
            /* Inside before outside, so the inner block's drops are already
             * sitting at the end of its own body when the enclosing ones land. */
            if (s->own_scope != NULL)
                emitted += emit_drops_in(p, s->items, s->nitems);
            Stmt *d = build_drops(p, s->own_scope, s->span, NULL);
            if (d != NULL) {
                list_append(p, &s->items, &s->nitems, d);
                emitted += d->nitems;
            }
            continue;
        }
        if (s->kind == S_RETURN) {
            check_return_of_owned_local(p, s);
            Stmt *d = build_drops_chain(p, s->ret_scope, s->span);
            if (d != NULL) {
                /* Handed to the return rather than spliced in ahead of it. A
                 * returned local is one of the values these destructors destroy,
                 * and the copy the return already wraps it in reads it -- so
                 * drops-then-return freed the bytes the copy was about to read,
                 * and the caller got a `str_dup` of freed memory. The code
                 * generator emits them after the value is computed and copied,
                 * which is the order `?` already uses for `try_drops`. */
                s->ret_drops = d;
                emitted += d->nitems;
            }
            continue;
        }
        if (s->kind == S_BREAK || s->kind == S_CONTINUE) {
            /* The scopes a jump leaves, innermost first, ending at the loop's own
             * body scope -- which is not left, because `break` jumps out of the
             * loop and `continue` jumps to its step, and in both cases the loop's
             * body scope is torn down when the loop itself ends.
             *
             * The drops go *before* the jump in the same item list, which is why
             * this needs nothing from the code generator: the jump is the last
             * statement of the block the pass builds, so it cannot run before the
             * drops above it. */
            Stmt *d = build_drops_to(p, s->ret_scope, s->jump_floor, s->span);
            if (d != NULL) {
                Stmt **out = arena_alloc_array(p->arena, (size_t)(d->nitems + 1), sizeof(Stmt *));
                for (int j = 0; j < d->nitems; j++)
                    out[j] = d->items[j];
                out[d->nitems] = s;
                Stmt *b = new_stmt(p, S_BLOCK, s->span);
                b->items = out;
                b->nitems = d->nitems + 1;
                items[i] = b;
                emitted += d->nitems;
            }
            continue;
        }
        if (s->kind == S_FUNC) {
            /* Guarded for the same reason as S_BLOCK: the entry function the
             * parser synthesizes for top-level statements is an item of the
             * program list while its body's statements are also reached through
             * that list. Revisiting would recompute `owns_drops` from a partial
             * walk and append the body's fall-off-the-end drops a second time. */
            if (s->drops_done)
                continue;
            s->drops_done = 1;
            /* A function is an item of the program list, and its body is where
             * every local it owns lives. Without this the pass never looks
             * inside a function at all. */
            if (s->fbody != NULL) {
                if (s->fbody->kind == S_BLOCK)
                    emitted += emit_drops_in(p, s->fbody->items, s->fbody->nitems);
                else {
                    Stmt *wrap[1];
                    wrap[0] = s->fbody;
                    emitted += emit_drops_in(p, wrap, 1);
                }
                /* The fall-off-the-end exit. Every `return` above destroys the
                 * body scope on its way out, but a function that ends without
                 * one -- every void function -- reaches the end of its body with
                 * the body's locals still live, and those are the values that
                 * leak. The body's own block is never visited by the S_BLOCK
                 * arm above (the walk is over its items), so this is the only
                 * place the body's own drop list can be emitted.
                 *
                 * When the last statement is a return these land after it and
                 * are unreachable, which costs a few bytes of assembly and
                 * nothing else: the return path already has its own copy. */
                if (s->fbody->kind == S_BLOCK) {
                    Stmt *d = build_drops(p, s->fbody->own_scope, s->fbody->span, NULL);
                    if (d != NULL) {
                        list_append(p, &s->fbody->items, &s->fbody->nitems, d);
                        emitted += d->nitems;
                    }
                }
            }
            s->owns_drops = emitted > 0;
            continue;
        }
        if (s->kind == S_STRUCT || s->kind == S_UNION) {
            /* Idempotent for the same reason: a top-level type declaration is an
             * item of the program list and its methods' bodies are walked here,
             * so a repeated visit would append each method body's drops twice. */
            if (s->drops_done)
                continue;
            s->drops_done = 1;
            /* A method body is where every local a method owns lives, and a
             * method is not an item of the program list: it hangs off the
             * StructDef, which is all `sdef` names. So the S_FUNC arm below never
             * sees one, and a local string a method returned was never released.
             * Walking the methods here is what puts them in the pass.
             *
             * The body is reached through `m->body`, which is the same S_FUNC
             * node code generation emits, so the drops land in the one function
             * that runs rather than in a copy. */
            StructDef *sd = s->sdef;
            if (sd == NULL)
                continue;
            for (int mi = 0; mi < sd->nmethods; mi++) {
                Stmt *mfn = sd->methods[mi]->body;
                if (mfn == NULL || mfn->fbody == NULL)
                    continue;
                if (mfn->fbody->kind == S_BLOCK) {
                    emitted += emit_drops_in(p, mfn->fbody->items, mfn->fbody->nitems);
                    Stmt *d = build_drops(p, mfn->fbody->own_scope, mfn->fbody->span, NULL);
                    if (d != NULL) {
                        list_append(p, &mfn->fbody->items, &mfn->fbody->nitems, d);
                        emitted += d->nitems;
                    }
                }
            }
            continue;
        }
        if (s->kind == S_IF || s->kind == S_WHILE || s->kind == S_FOR) {
            /* Each arm goes through the list walk as a single item rather than
             * being descended into directly. A block body is itself a scope, and
             * its own drops are appended by the S_BLOCK arm above; passing its
             * *items* instead walked straight past it, so a value declared in a
             * loop body or an `if` body was never destroyed -- and a loop body
             * is re-entered every iteration, so that is the one place a leak
             * accumulates rather than happening once. */
            if (s->body != NULL) {
                Stmt *wrap[1];
                wrap[0] = s->body;
                emitted += emit_drops_in(p, wrap, 1);
            }
            if (s->orelse != NULL) {
                Stmt *wrap[1];
                wrap[0] = s->orelse;
                emitted += emit_drops_in(p, wrap, 1);
            }
            if (s->for_init != NULL) {
                Stmt *wrap[1];
                wrap[0] = s->for_init;
                emitted += emit_drops_in(p, wrap, 1);
            }
        }
    }
    return emitted;
}

/* Runs the destructor pass over the whole program. */
static void emit_drops(Parser *p, Stmt *program) {
    if (program == NULL)
        return;
    emit_drops_in(p, program->items, program->nitems);
}

static Stmt *parse_block(Parser *p) {
    Span start = cur(p)->span;
    if (!at(p, T_LBRACE)) {
        diag_error(cur(p)->span, "expected '{' to open a block");
        return new_stmt(p, S_BLOCK, start);
    }
    /* A nested block is a nested scope, and the scope stack is a fixed array,
     * so the depth at which scopes stop being recorded is already bounded. The
     * recursion into parse_stmt is not, though: a block deep enough runs the C
     * stack out. The two limits are the same number, so the diagnostic that
     * says the nest is too deep is the one the reader sees.
     *
     * The check is before the '{' is consumed, because the body is skipped
     * rather than parsed, and the skip needs to know where the body starts. */
    if (!enter_depth(p)) {
        skip_braced_block(p);
        return new_stmt(p, S_BLOCK, start);
    }
    advance(p); /* the '{' */
    scope_push(p);
    int cap = 8, n = 0;
    Stmt **items = arena_alloc_array(p->arena, (size_t)cap, sizeof(Stmt *));
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        if (n == cap) {
            int ncap = cap * 2;
            Stmt **ni = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Stmt *));
            memcpy(ni, items, (size_t)n * sizeof(Stmt *));
            items = ni;
            cap = ncap;
        }
        /* A statement after one that always leaves the block can never run.
         * Say so once, at the first dead statement, and keep parsing: the rest
         * still has to type-check so the errors below are the real ones. */
        if (n > 0 && stmt_terminates(items[n - 1]))
            diag_warn(W_UNREACHABLE, cur(p)->span, "this statement can never run");
        items[n++] = parse_stmt(p);
    }
    leave_depth(p);
    if (!match(p, T_RBRACE)) {
        diag_error(cur(p)->span, "expected '}' to close block");
    }
    /* The destructor calls for this scope go after its last statement, so the
     * drops run on the way out rather than being a second entry into the
     * block. This is the *normal* exit; the early exits are handled where they
     * are parsed, because a `return` three blocks down still has to destroy
     * everything those three blocks own. */
    /* A function body's own scope is not dropped here. Its locals live until
     * the function returns, and every exit from the body is a `return` (or a
     * `?` propagating out of it), and each of those already emits the drops for
     * the scopes it is leaving -- the body scope among them. Emitting them a
     * second time at the end of the block put them after the return, where they
     * are dead code, and a destructor that runs only on the fallthrough path
     * runs on almost no path at all. */
    /* The scope this block introduced is recorded on the block; the destructor
     * calls are appended afterwards by emit_drops, once every type the program
     * mentions is complete. Deciding here made the answer depend on whether a
     * class had been read yet, which is a question about the order the parser
     * walks the file rather than about the program -- a `var t = new T()`
     * written above `class T` came out owning nothing. */
    Scope *own = p->scope;
    scope_pop(p);
    Stmt *s = new_stmt(p, S_BLOCK, start);
    s->items = items;
    s->nitems = n;
    s->own_scope = own;
    return s;
}
