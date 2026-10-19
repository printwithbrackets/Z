/* ---- generic helpers ---- */

/* Saves the active type-parameter bindings, so an instantiation that runs inside
 * another one -- `Vec<Vec<int>>` builds the inner `Vec<int>` while the outer
 * `Vec` is still being instantiated -- puts the outer ones back afterwards.
 *
 * Only the *count* used to be saved, which left the entries themselves
 * overwritten: the inner instantiation bound T to `int` where the outer had bound
 * it to `Vec<int>`, and when the outer body was read T was `int` again. The
 * result was an outer vector whose `T[]` field held an inner vector's element
 * type, and a `return data[i]` that reported two identical-looking types as
 * mismatched. */
typedef struct {
    TypeBind *binds;
    int n;
} SavedBinds;

static SavedBinds save_binds(Parser *p) {
    SavedBinds s;
    s.n = p->ntbind;
    s.binds = s.n > 0 ? arena_alloc_array(p->arena, (size_t)s.n, sizeof(TypeBind)) : NULL;
    for (int i = 0; i < s.n; i++)
        s.binds[i] = p->tbinds[i];
    return s;
}

static void restore_binds(Parser *p, SavedBinds s) {
    for (int i = 0; i < s.n && i < p->tbind_cap; i++)
        p->tbinds[i] = s.binds[i];
    p->ntbind = s.n;
}

static void bind_type(Parser *p, char *name, Type *ty) {
    if (p->ntbind == p->tbind_cap) {
        p->tbind_cap = p->tbind_cap ? p->tbind_cap * 2 : 8;
        p->tbinds = arena_alloc_array(p->arena, (size_t)p->tbind_cap, sizeof(TypeBind));
    }
    p->tbinds[p->ntbind].name = name;
    p->tbinds[p->ntbind].ty = ty;
    p->ntbind++;
}

/* True if `name` already has an active type-parameter binding. During
 * instantiation the concrete binding is pre-established, and re-parsing the
 * template must not replace it with a fresh placeholder. */
static int is_bound(Parser *p, const char *name) {
    for (int i = p->ntbind - 1; i >= 0; i--)
        if (strcmp(p->tbinds[i].name, name) == 0)
            return 1;
    return 0;
}

static Generic *find_generic(Parser *p, const char *name) {
    for (int i = 0; i < p->ngenerics; i++)
        if (strcmp(p->generics[i].name, name) == 0)
            return &p->generics[i];
    return NULL;
}

static void add_generic(Parser *p, Generic g) {
    if (p->ngenerics == p->generic_cap) {
        p->generic_cap = p->generic_cap ? p->generic_cap * 2 : 8;
        Generic *bigger = arena_alloc_array(p->arena, (size_t)p->generic_cap, sizeof(Generic));
        /* Copy what is already registered. Growing into a fresh block and leaving
         * the old contents behind leaves every entry a garbage pointer, and the
         * first `find_generic` then walks off into nothing: the ninth generic
         * function in a program crashed the compiler rather than failing in a way
         * that pointed here. Same mistake, same fix, as add_pending below. */
        if (p->ngenerics > 0)
            memcpy(bigger, p->generics, (size_t)p->ngenerics * sizeof(Generic));
        p->generics = bigger;
    }
    p->generics[p->ngenerics++] = g;
}

static void add_pending(Parser *p, Stmt *fn) {
    PendingList *L = p->pending;
    if (L->count == L->cap) {
        int ncap = L->cap ? L->cap * 2 : 8;
        Stmt **bigger = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Stmt *));
        /* Copy what is already queued. Growing into a fresh block and leaving
         * the old contents behind loses every entry added so far, so the ninth
         * queued function replaced the first eight with garbage -- which is why
         * a program with eight closures worked and one with nine crashed the
         * compiler rather than failing in a way that pointed here. */
        if (L->count > 0)
            memcpy(bigger, L->items, (size_t)L->count * sizeof(Stmt *));
        L->items = bigger;
        L->cap = ncap;
    }
    L->items[L->count++] = fn;
}

/* Builds a mangled instance name like `max__int` or `foo__int_bool`.
 *
 * Every non-alphanumeric character in a rendered type name becomes `_`, which is
 * what makes the result a legal assembler symbol. That mapping is lossy on its
 * own: `A<B,C>` and `A_B_C` render the same and so would mangle the same, and no
 * sizing can fix it. It is left as it is because the names Z can render for a
 * type parameter do not collide that way in practice -- `int[]` and `int**` do,
 * but Z has no way to spell an `int**` as a type argument.
 *
 * The buffer used to be sized by `strlen(name) + 8 * n + 8`, a guess, and the
 * loop filling it stopped at `off + 2 < cap` without reporting that it had. So a
 * type name got about twelve bytes, and two types agreeing on their first twelve
 * characters became one symbol:
 *
 *     f<ConfigurationSet>  ->  f__Configuratio
 *     f<ConfigurationMap>  ->  f__Configuratio
 *
 * The parser then found the first instance's cached signature, could not unify
 * the second argument against it, and rejected a correct program -- quoting a
 * mangled name the reader never wrote.
 *
 * So the size is computed from the rendered names rather than guessed, which
 * leaves no room to truncate and nothing to guard. */
static char *mangle_instance(Parser *p, const char *name, Type **types, int n) {
    const char **rendered = arena_alloc_array(p->arena, (size_t)(n > 0 ? n : 1), sizeof(char *));
    size_t cap = strlen(name) + 2 + 1; /* "name__" and the terminator */
    for (int i = 0; i < n; i++) {
        rendered[i] = type_name(p->ty, types[i]);
        /* The name itself, plus a separator between this type and the next. */
        cap += strlen(rendered[i]) + 1;
    }
    char *buf = arena_alloc(p->arena, cap);
    size_t off = (size_t)snprintf(buf, cap, "%s__", name);
    for (int i = 0; i < n; i++) {
        for (const char *c = rendered[i]; *c; c++) {
            char ch = *c;
            buf[off++] =
                ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
                    ? ch
                    : '_';
        }
        if (i + 1 < n)
            buf[off++] = '_';
    }
    buf[off] = '\0';
    return buf;
}

/* Re-parses a generic template `g` with its type parameters bound to the given
 * concrete types, producing a specialized function appended to the pending
 * list. Returns the mangled instance name. */
/* Structurally unifies a template parameter type against a concrete argument
 * type, binding any type parameters it contains (handles `T`, `T[]`, `T*`).
 * Returns 1 on success, 0 on a binding conflict. */
static int unify_type(Parser *p, Generic *g, Type *tmpl, Type *arg, Type **concrete, Span span) {
    if (tmpl == NULL || is_unk(tmpl) || is_unk(arg))
        return 1;
    if (is_kind(tmpl, TK_TYPEPARAM)) {
        int ti = -1;
        for (int j = 0; j < g->ntparams; j++)
            if (strcmp(g->tparams[j], tmpl->pname) == 0)
                ti = j;
        if (ti < 0)
            return 1;
        if (concrete[ti] == NULL) {
            concrete[ti] = arg;
        } else if (!type_equals(concrete[ti], arg)) {
            diag_error(span, "type parameter '%s' is bound to both '%s' and '%s'", g->tparams[ti],
                       type_name(p->ty, concrete[ti]), type_name(p->ty, arg));
            return 0;
        }
        return 1;
    }
    if (is_kind(tmpl, TK_PTR) && is_kind(arg, TK_PTR))
        return unify_type(p, g, tmpl->base, arg->base, concrete, span);
    if (is_kind(tmpl, TK_ARRAY) && is_kind(arg, TK_ARRAY))
        return unify_type(p, g, tmpl->base, arg->base, concrete, span);
    return 1;
}

static char *instantiate(Parser *p, Generic *g, Type **concrete, Span span) {
    char *mangled = mangle_instance(p, g->name, concrete, g->ntparams);
    (void)span;

    /* Reuse a previously-created instance for the same type arguments. */
    for (int i = 0; i < g->ninstances; i++) {
        int same = 1;
        for (int j = 0; j < g->ntparams; j++)
            if (!type_equals(g->inst_types[i][j], concrete[j])) {
                same = 0;
                break;
            }
        if (same)
            return g->mangled[i];
    }

    /* Save parser state so re-parsing the template is invisible to the caller. */
    int saved_pos = p->pos;
    Scope *saved_scope = p->scope;
    int saved_off = p->next_offset;
    Type *saved_ret = p->cur_ret;
    StructDef *saved_msd = p->cur_msd;
    Expr *saved_this = p->cur_this;
    SavedBinds saved_binds_v = save_binds(p);
    char *saved_ns = p->ns_prefix;

    for (int i = 0; i < g->ntparams; i++)
        bind_type(p, g->tparams[i], concrete[i]);

    p->pos = g->tok_start;
    p->ns_prefix = g->ns_prefix;
    p->in_instantiate = 1;
    /* A diagnostic raised while re-parsing the template points at a span in the
     * template, which is a different place from the call that caused it. Saying
     * which instance this is, and where it was asked for, is the difference
     * between a message the reader can act on and one they have to guess at. */
    diag_push_ctx(DIAG_CTX_INSTANCE, mangled, span);
    Stmt *inst = parse_func(p, 0);
    diag_pop_ctx();
    p->in_instantiate = 0;
    inst->fname = mangled;
    inst->is_generic_template = 0;
    if (inst->is_ext) {
        /* keep ext flag; receiver param is already typed concretely */
    }
    add_pending(p, inst);

    /* Register the concrete signature so the call type-checks and codegen
     * resolves the mangled symbol. Only user-visible params belong in the sig
     * (the hidden sret pointer, if any, is handled by codegen via the call's
     * result type). */
    int ivis = (is_kind(inst->ret_type, TK_STRUCT) || is_kind(inst->ret_type, TK_UNION)) ? 1 : 0;
    int invis = inst->nparams - ivis;
    Type **cpt = arena_alloc_array(p->arena, (size_t)(invis > 0 ? invis : 1), sizeof(Type *));
    for (int i = 0; i < invis; i++)
        cpt[i] = inst->params[ivis + i]->type;
    add_sig(p, mangled, inst->ret_type, cpt, invis, inst->is_ext);

    /* Cache the instance (grow, preserving prior entries) so a second call with
     * the same type arguments finds it. This is the whole reason the loop above
     * works, and it is not optional: a template called with the same type twice
     * -- `max(3, 7)` and `max(10, 4)` are both `max<int>` -- would otherwise be
     * re-parsed and emitted twice, and the assembler fails on the second copy of
     * a symbol rather than on anything that points here. */
    int old_n = g->ninstances;
    char **old_m = old_n ? arena_alloc_array(p->arena, (size_t)old_n, sizeof(char *)) : NULL;
    Type ***old_t = old_n ? arena_alloc_array(p->arena, (size_t)old_n, sizeof(Type **)) : NULL;
    for (int i = 0; i < old_n; i++) {
        old_m[i] = g->mangled[i];
        old_t[i] = g->inst_types[i];
    }
    g->mangled = arena_alloc_array(p->arena, (size_t)(old_n + 1), sizeof(char *));
    g->inst_types = arena_alloc_array(p->arena, (size_t)(old_n + 1), sizeof(Type **));
    for (int i = 0; i < old_n; i++) {
        g->mangled[i] = old_m[i];
        g->inst_types[i] = old_t[i];
    }
    g->mangled[old_n] = mangled;
    g->inst_types[old_n] = concrete;
    g->ninstances = old_n + 1;

    /* Restore parser state. */
    p->ns_prefix = saved_ns;
    p->pos = saved_pos;
    p->scope = saved_scope;
    p->next_offset = saved_off;
    p->cur_ret = saved_ret;
    p->cur_msd = saved_msd;
    p->cur_this = saved_this;
    restore_binds(p, saved_binds_v);
    return mangled;
}

/* Builds the instance of a generic type for the given concrete type arguments,
 * re-parsing the declaration with its parameters bound. Returns the instance's
 * StructDef, or NULL if the arguments did not fit the template.
 *
 * The mirror image of instantiate() for a generic function: same binding, same
 * caching by type-argument tuple, same mangling, same "this is an instance"
 * context for diagnostics. What is different is the product -- a struct with
 * fields laid out and methods parsed, rather than one function -- and that the
 * instance's methods have to be emitted, which happens because the re-parse
 * produces an S_STRUCT carrying them and the declaration is added to the pending
 * list exactly as a function instance is. */
/* Records a built (or being built) instance against its type arguments, so a
 * second request for the same combination finds it. */
static void cache_gtype_instance(Parser *p, GenericType *g, const char *mangled, Type **concrete) {
    int old_n = g->ninstances;
    char **old_m = old_n ? arena_alloc_array(p->arena, (size_t)old_n, sizeof(char *)) : NULL;
    Type ***old_t = old_n ? arena_alloc_array(p->arena, (size_t)old_n, sizeof(Type **)) : NULL;
    for (int i = 0; i < old_n; i++) {
        old_m[i] = g->inst_names[i];
        old_t[i] = g->inst_types[i];
    }
    g->inst_names = arena_alloc_array(p->arena, (size_t)(old_n + 1), sizeof(char *));
    g->inst_types = arena_alloc_array(p->arena, (size_t)(old_n + 1), sizeof(Type **));
    for (int i = 0; i < old_n; i++) {
        g->inst_names[i] = old_m[i];
        g->inst_types[i] = old_t[i];
    }
    g->inst_names[old_n] = arena_strdup(p->arena, mangled);
    g->inst_types[old_n] = concrete;
    g->ninstances = old_n + 1;
}

static StructDef *instantiate_type(Parser *p, GenericType *g, Type **concrete, Span span) {
    char *mangled = mangle_instance(p, g->name, concrete, g->ntparams);

    /* Already built, or already being built. A method of this very declaration
     * may name its own type -- `new Vec<T>()` inside Vec's own `reversed()` --
     * and a type that only enters the cache once its body is finished has
     * nothing for that request to find, so it re-enters, and again, until the
     * stack ran out. The instance is therefore interned before a single token of
     * the body is read, and a nested request gets the half-built struct. That is
     * safe: the layout is complete before code generation reads a size, and a
     * `new Vec<T>()` in a method body is an expression that runs later. */
    {
        Type *have = type_find_struct(p->ty, mangled);
        if (have != NULL && have->sdef != NULL)
            return have->sdef;
    }

    /* Save parser state so re-parsing the declaration is invisible. */
    int saved_pos = p->pos;
    Scope *saved_scope = p->scope;
    int saved_off = p->next_offset;
    Type *saved_ret = p->cur_ret;
    StructDef *saved_msd = p->cur_msd;
    Expr *saved_this = p->cur_this;
    SavedBinds saved_binds_v = save_binds(p);
    int saved_loop_depth = p->loop_depth;
    int saved_foreach = p->foreach_counter;
    int saved_lambda = p->lambda_counter;
    int saved_func = p->func_counter;
    int saved_owner = p->cur_owner;
    StructDef *saved_captured[MAX_CAPTURES];
    int saved_ncaptured = p->ncaptured;
    if (saved_ncaptured > 0)
        memcpy(saved_captured, p->captured, (size_t)saved_ncaptured * sizeof(Var));
    /* The instance-in-progress fields, because a declaration may instantiate
     * another generic type -- `Set` holds a `Map<T,bool>` and constructs one in
     * its own constructor -- and the nested call overwrites them. Without putting
     * them back, every member after that point in the *outer* declaration was
     * resolved with the inner instance's name: `Set` stopped meaning `Set`, and
     * the constructor was no longer recognised as one. */
    const char *saved_inst_name = p->inst_name;
    StructDef *saved_inst_sdef = p->inst_sdef;
    const char *saved_inst_srcname = p->inst_srcname;
    char *saved_ns = p->ns_prefix;
    p->ns_prefix = g->ns_prefix;
    /* The declaration is parsed at the top level, not inside whatever use site
     * triggered it, so a stray `break` or a return type left over from the
     * expression being parsed cannot be attributed to the container. */
    p->loop_depth = 0;
    p->cur_ret = NULL;
    p->cur_msd = NULL;
    p->cur_this = NULL;
    p->ncaptured = 0;

    for (int i = 0; i < g->ntparams; i++)
        bind_type(p, g->tparams[i], concrete[i]);

    cache_gtype_instance(p, g, mangled, concrete);
    StructDef *place = type_define_struct(p->ty, mangled);
    if (place == NULL) {
        diag_error_code(span, "duplicate_instance", "'%s' has already been built", g->name);
        restore_binds(p, saved_binds_v);
        return NULL;
    }

    p->pos = g->tok_start;
    p->inst_name = arena_strdup(p->arena, mangled);
    p->inst_sdef = place;
    p->inst_srcname = g->src_name != NULL ? g->src_name : g->name;
    /* A fresh root scope, because the declaration is being parsed in the middle
     * of an expression somewhere. Chaining to the use site's scope would make a
     * parameter of the template shadow-check against a local that happens to have
     * the same name at the place it was asked for -- warning about two variables
     * that have never met, once per instance, for a template that is fine. The
     * declaration is lexically at the top level of a file, so this is what its
     * scope actually is. */
    Scope *use_site_scope = p->scope;
    p->scope = NULL;
    scope_push(p);
    diag_push_ctx(DIAG_CTX_INSTANCE, mangled, span);
    Stmt *decl = parse_struct_decl(p, g->is_class);
    diag_pop_ctx();
    scope_pop(p);
    p->scope = use_site_scope;
    StructDef *sd = decl->sdef;
    p->inst_name = NULL;
    p->inst_sdef = NULL;
    p->inst_srcname = NULL;
    if (sd != NULL) {
        /* The instance is a real struct under a real (mangled) name, so a
         * signature written above the use site resolves, its fields are laid out
         * and `new` finds it -- nothing downstream of here knows generics exist.
         *
         * A generic declaration produces no code of its own. Only its instances
         * do, and only the ones actually asked for: a program that never writes
         * `Vec<int>` never has one built, let alone emitted. */
        add_pending(p, decl);
    }

    /* Restore parser state. */
    p->ns_prefix = saved_ns;
    p->pos = saved_pos;
    p->scope = saved_scope;
    p->next_offset = saved_off;
    p->cur_ret = saved_ret;
    p->cur_msd = saved_msd;
    p->cur_this = saved_this;
    restore_binds(p, saved_binds_v);
    p->loop_depth = saved_loop_depth;
    p->foreach_counter = saved_foreach;
    p->lambda_counter = saved_lambda;
    p->func_counter = saved_func;
    p->cur_owner = saved_owner;
    if (saved_ncaptured > 0)
        memcpy(p->captured, saved_captured, (size_t)saved_ncaptured * sizeof(Var));
    p->ncaptured = saved_ncaptured;
    p->inst_name = saved_inst_name;
    p->inst_sdef = saved_inst_sdef;
    p->inst_srcname = saved_inst_srcname;
    return sd;
}

/* Binds a generic type's parameters to `nargs` already-parsed type arguments,
 * instantiating the type if this is the first request for that combination.
 * Returns the instance's StructDef, or NULL after reporting why not. */
static StructDef *resolve_gtype(Parser *p, GenericType *g, Type **args, int nargs, Span span) {
    /* The arguments are copied into the arena before anything keeps them. The
     * caller builds them in a stack array, and the instance cache holds the
     * array for as long as the template lives -- so a cached instance compared
     * its type arguments against whatever the stack held afterwards, missed
     * every time, and built a fresh copy of the class for every single use. The
     * symptom was a set of `Vec__int`, `Vec__int_int`, `Vec__int_int_int`
     * instances, one per method that mentioned the receiver, and a pile of
     * duplicate definitions. */
    Type **kept = arena_alloc_array(p->arena, (size_t)(nargs > 0 ? nargs : 1), sizeof(Type *));
    for (int i = 0; i < nargs; i++)
        kept[i] = args[i];
    args = kept;
    if (nargs != g->ntparams) {
        diag_error_code(span, "wrong_type_arity", "'%s' takes %d type argument%s but got %d",
                        g->name, g->ntparams, g->ntparams == 1 ? "" : "s", nargs);
        return NULL;
    }
    for (int i = 0; i < nargs; i++) {
        if (args[i] == NULL || is_unk(args[i])) {
            diag_error_code(span, "unbound_type_argument", "cannot infer type argument %d of '%s'",
                            i + 1, g->name);
            return NULL;
        }
    }
    return instantiate_type(p, g, args, span);
}

/* `via_surface` says the name reached this call through the qualified surface
 * table rather than off a token. It is what keeps `print` reachable: the internal
 * name is still `print`, but a program writes `Console.WriteLog`, so without this
 * a bare `Console.WriteLog(...)` would find the same builtin and quietly keep working. */
static Expr *parse_call(Parser *p, char *name, Span span, int via_surface) {
    /* An unqualified call beside its namespace resolves there first: `connect`
     * inside `namespace netlink` is `netlink.connect`. A nested function of
     * the same name still wins, since it is the more local declaration. */
    if (!via_surface && resolve_fn_sym(p, name) == name) {
        char *r = ns_resolve(p, name, look_sig);
        if (r == NULL)
            r = ns_resolve(p, name, look_generic);
        if (r != NULL)
            name = r;
    }

    /* A plain-identifier surface name: `say(...)` for the `print` this compiler
     * knows, or `magnitude(...)` for `abs`.
     *
     * Done by rewriting `name` in place rather than by re-entering this function.
     * Re-entering would be the shorter line and it is wrong: by here the argument
     * list has been consumed, so a second pass would read the first argument's
     * first token as its opening paren. Rewriting also means everything below --
     * the argument types, the intrinsic table, the user-function shadowing check
     * -- sees the internal name, which is what it is written in terms of.
     *
     * A declared function or generic of the same spelling is the user's and wins,
     * so those are asked first. The whole rule is "surface names are a fallback":
     * ordinary resolution always gets the first turn. */
    if (!via_surface) {
        const char *mapped = surface_lookup(p->surf, name, strlen(name));
        if (mapped != NULL && find_sig(p, name) == NULL && find_generic(p, name) == NULL) {
            name = (char *)mapped;
            via_surface = 1;
        }
    }
    /* `base(args)` called a base class constructor, and went with inheritance.
     * It is a diagnostic rather than a silent undefined function, because a
     * program carried over from v1 will have one and the reader needs to know
     * it is not a name that failed to resolve. A constructor initialises its own
     * fields, so what `base(l)` did is now an assignment in the constructor. */
    if (strcmp(name, "base") == 0 && at(p, T_LPAREN) && p->cur_msd != NULL) {
        diag_error_code(span, "no_inheritance",
                        "'base(...)' has no meaning without inheritance: there is no base "
                        "constructor to call. Assign the field in this constructor instead");
        while (!at(p, T_RPAREN) && !at(p, T_EOF))
            advance(p);
        match(p, T_RPAREN);
        Expr *err = new_expr(p, E_INT, span);
        err->type = NULL;
        return err;
    }
    advance(p); /* '(' */
    Expr *e = new_expr(p, E_CALL, span);
    e->name = name;

    int cap = 4, n = 0;
    Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
    /* When the callee's signature is already known, a parameter's type can tell
     * an `Ok(...)`/`Err(...)` in argument position what to build. Resolved before
     * the arguments because the arguments are parsed first; a callee that is not
     * yet known (a generic, a builtin) simply gives no expectation, and `Ok` says
     * so rather than guessing. */
    Sig *pre = find_sig(p, name);
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (n == cap) {
                int ncap = cap * 2;
                Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                memcpy(na, args, (size_t)n * sizeof(Expr *));
                args = na;
                cap = ncap;
            }
            Type *saved_expect = p->expect_result;
            p->expect_result = pre != NULL && n < pre->nparams && type_is_result(pre->ptypes[n])
                                   ? pre->ptypes[n]
                                   : NULL;
            args[n++] = parse_expr(p);
            p->expect_result = saved_expect;
            if (!match(p, T_COMMA))
                break;
        }
    }
    e->args = args;
    e->nargs = n;
    if (!match(p, T_RPAREN)) {
        diag_error(cur(p)->span, "expected ')' after arguments");
    }

    /* `typeof(e)`: a small integer naming the *static* type of `e`, from the
     * table of TYPE_* constants the standard library declares.
     *
     * It is a special form rather than a built-in because the answer is a
     * property of the type, not of the value, and a built-in's signature cannot
     * say that. It is also the only way a generic function can ask what it was
     * handed, which is what makes a `Vec<T>.join` or a `Map<K,V>` hash work
     * without every element type implementing an interface: monomorphization
     * re-parses the template with the parameter bound, so inside the `Vec<int>`
     * instance the static type really is `int` and `typeof` really does answer
     * 1. Inside the template before instantiation it would answer TYPE_TYPEPARAM,
     * which is why a generic function has to be written for the instance and not
     * for the template.
     *
     * A user's own `typeof` shadows it, the same as a user's own `print`. */
    if (strcmp(name, "typeof") == 0 && find_sig(p, name) == NULL) {
        if (n != 1) {
            diag_error_code(span, "wrong_arity", "typeof takes 1 argument but got %d", n);
            e->type = type_int(p->ty);
            return e;
        }
        Expr *r = new_expr(p, E_INT, span);
        r->type = type_int(p->ty);
        if (is_unk(args[0]->type))
            r->ival = TK_VOID;
        else
            r->ival = type_kind_code(args[0]->type);
        /* The answer is a property of the type, so it is already known here and
         * the node to emit is an integer literal. This used to attach the
         * argument to `e` and then clear it again, which left a call to a
         * function named `typeof` with no arguments -- so every program that
         * asked its type anything failed in the linker with an undefined
         * reference to `z$typeof`, and nothing in the tree did, which is why it
         * went unnoticed. */
        return r;
    }

    /* Built-in print, reached only through a surface name.
     *
     * There is deliberately no bare `print` any more: `Console.WriteLog` is how a
     * program says it, and a name the table defines is one place to change if the
     * spelling changes. The internal name stays `print` -- the runtime symbol,
     * the emitted call and this check all predate the surface table, and none of
     * them is a thing a program can see. */
    if (via_surface && strcmp(name, "print") == 0) {
        if (n != 1) {
            diag_error(span, "'%s' expects 1 argument but got %d", PRINT_SURFACE, n);
            e->type = type_void(p->ty);
            return e;
        }
        Type *at0 = args[0]->type;
        /* An unknown type means the argument already failed to compile and the
         * real error has been reported. Saying anything about it here is a
         * second complaint about one mistake, and it buries the first. */
        if (!is_unk(at0) && !is_kind(at0, TK_INT) && !is_kind(at0, TK_BOOL) &&
            !is_kind(at0, TK_STRING) && !is_kind(at0, TK_F64)) {
            diag_error_code(span, "type_mismatch",
                            "'%s' expects 'int', 'bool', 'float' or 'string' but got '%s'",
                            PRINT_SURFACE, type_name(p->ty, at0));
        }
        e->type = type_void(p->ty);
        return e;
    }

    /* Built-in intrinsic. Checked only after the user-function lookup would
     * have failed, so a declaration named `min` shadows the intrinsic. */
    if (find_sig(p, name) == NULL && find_generic(p, name) == NULL) {
        int arity = intrinsic_arity(name);
        if (arity > 0) {
            if (n != arity) {
                diag_error(span, "'%s' expects %d argument%s but got %d", name, arity,
                           arity == 1 ? "" : "s", n);
                e->type = NULL;
                return e;
            }
            for (int i = 0; i < n; i++) {
                if (!is_kind(args[i]->type, TK_INT) && !is_unk(args[i]->type)) {
                    diag_error(args[i]->span, "argument %d of '%s' expects 'int' but got '%s'",
                               i + 1, name, type_name(p->ty, args[i]->type));
                }
            }
            Expr *in = new_expr(p, E_INTRINSIC, span);
            in->name = arena_strdup(p->arena, name);
            in->args = args;
            in->nargs = n;
            in->type = type_int(p->ty);
            return in;
        }
    }

    /* `Ok(v)` and `Err(e)`: the two ways to build a `Result`.
     *
     * Which one is meant, and with which types, has to come from where the value
     * is going -- the declared type of the variable, the enclosing function's
     * return type, or a parameter's type. There is no way to spell it inline:
     * `Ok(3)` alone does not say what the error type is, and guessing one would
     * make `Result<int,string>` and `Result<int,io>` silently different types
     * that then fail to unify. The expectation is recorded in p->expect_result
     * by the three places that know it.
     *
     * Resolved before the user-function lookup only in the sense that this is
     * tried first; a program that declares its own `Ok` still gets its own. */
    if ((strcmp(name, "Ok") == 0 || strcmp(name, "Err") == 0) && find_sig(p, name) == NULL &&
        find_generic(p, name) == NULL) {
        int is_ok = name[0] == 'O';
        if (n != 1) {
            diag_error(span, "'%s' expects 1 argument but got %d", name, n);
            e->type = NULL;
            return e;
        }
        Type *want = p->expect_result;
        if (want == NULL || !type_is_result(want)) {
            diag_error(span,
                       "'%s' needs to know the other type: declare the variable's type, or "
                       "use it where a 'Result<T, E>' is expected",
                       name);
            e->type = NULL;
            return e;
        }
        Type *slot = is_ok ? type_result_ok(want) : type_result_err(want);
        if (!is_unk(args[0]->type) && !type_equals(args[0]->type, slot))
            diag_error(args[0]->span, "argument 1 of '%s' expects '%s' but got '%s'", name,
                       type_name(p->ty, slot), type_name(p->ty, args[0]->type));
        Expr *u = new_expr(p, E_UNIONLIT, span);
        u->variant = &want->udef->variants[is_ok ? 0 : 1];
        u->args = args;
        u->nargs = 1;
        u->type = want;
        *e = *u;
        return e;
    }

    /* Standard-library built-in. Resolved after the user-function lookup would
     * have failed, for the same shadowing reason as an intrinsic: a program that
     * declares its own `len` gets its own. The call is checked against the
     * built-in's real signature and given a real result type, so from here on it
     * is an ordinary call and needs no special handling anywhere else. */
    if (find_sig(p, name) == NULL && find_generic(p, name) == NULL) {
        char ret_code = 0;
        const char *params = NULL;
        const char *sym = builtin_lookup(name, &ret_code, &params);
        if (sym != NULL) {
            int want = (int)strlen(params);
            if (n != want) {
                diag_error(span, "'%s' expects %d argument%s but got %d", name, want,
                           want == 1 ? "" : "s", n);
                e->type = NULL;
                return e;
            }
            for (int i = 0; i < n; i++) {
                if (is_unk(args[i]->type))
                    continue;
                Type *wt = builtin_type(p, params[i]);
                if (!type_equals(args[i]->type, wt)) {
                    diag_error(args[i]->span, "argument %d of '%s' expects '%s' but got '%s'",
                               i + 1, name, type_name(p->ty, wt), type_name(p->ty, args[i]->type));
                }
            }
            /* `hold` is declared `any` so that it can take a whole or a fractional
             * number of seconds, and `any` matches whatever it is handed -- which
             * would let a string through as a wait. It takes a number and nothing
             * else, and the gap is closed here, where the caller's own text is
             * still in hand to point at. */
            /* `str_buf_append` consumes its first argument.
             *
             * The call takes the buffer and hands back either the same buffer or a
             * new one, and only the runtime knows which. Marking the argument as
             * moved is what makes one rule work for both: the scope teardown skips
             * a moved variable, and reading one afterwards is a diagnostic rather
             * than a use of bytes the call has already released.
             *
             * Only a bare variable is marked. A field is not: `StringBuilder.append`
             * passes `buf`, and the assignment that receives the result overwrites
             * the field in the same statement, so there is nothing to skip and
             * nothing later that could read the released buffer. Marking it would
             * make the second append an error.
             *
             * The assignment path clears the flag again, so `buf = str_buf_append(
             * buf, s)` can be repeated -- which is how the builder is written, and
             * how `temporaries.z` exercises it. */
            if ((strcmp(name, "str_buf_append") == 0 || strcmp(name, sym) == 0) && n == 2 &&
                args[0]->kind == E_VAR && args[0]->name != NULL) {
                Var *taken = lookup_var(p, args[0]->name);
                if (taken != NULL) {
                    taken->is_moved = 1;
                    taken->move_span = args[0]->span;
                }
            }
            if (strcmp(name, "hold") == 0 && n == 1 && !is_unk(args[0]->type) &&
                !is_kind(args[0]->type, TK_INT) && !is_kind(args[0]->type, TK_F64))
                diag_error(args[0]->span,
                           "argument 1 of 'hold' expects a number of seconds but got '%s'",
                           type_name(p->ty, args[0]->type));
            /* is_extern makes codegen call the symbol verbatim, which is what
             * reaches the runtime instead of the z$ namespaced name. */
            e->name = arena_strdup(p->arena, sym);
            e->is_extern = 1;
            e->type = builtin_type(p, ret_code);
            return e;
        }
    }

    /* Generic function: infer type arguments from the call and instantiate. */
    Generic *g = find_generic(p, name);
    if (g != NULL) {
        if (g->nvisible != n) {
            diag_error(span, "'%s' expects %d argument%s but got %d", name, g->nvisible,
                       g->nvisible == 1 ? "" : "s", n);
            e->type = NULL;
            return e;
        }
        /* Bind each type parameter by unifying the template parameter types
         * with the argument types (handles `T`, `T[]`, `T*`). */
        Type **concrete = arena_alloc_array(p->arena, (size_t)g->ntparams, sizeof(Type *));
        int ok = 1;
        for (int i = 0; i < n && ok; i++)
            ok = unify_type(p, g, g->ptypes[g->vis_start + i], args[i]->type, concrete,
                            args[i]->span);
        for (int j = 0; j < g->ntparams; j++) {
            if (concrete[j] == NULL || is_unk(concrete[j])) {
                diag_error(span, "cannot infer type parameter '%s' of '%s'", g->tparams[j], name);
                ok = 0;
            }
        }
        if (!ok) {
            e->type = NULL;
            return e;
        }
        char *mangled = instantiate(p, g, concrete, span);
        name = mangled;
        e->name = mangled;
    }

    /* User function. A function declared inside another one answers to a mangled
     * symbol, and its signature was filed under that symbol, so both the lookup
     * and the emitted call go through the same resolution.
     *
     * The dialect is consulted here, before the lookup rather than instead of it:
     * the internal name has to be what is searched for and what is emitted, and
     * the caller's `name` is the surface spelling. A program that declares its
     * own function under the surface name has already claimed it, because this
     * runs only after `find_sig` below has had a chance to say so -- see the
     * retry there. */
    const char *sym = resolve_fn_sym(p, name);
    Sig *s = find_sig(p, sym);
    if (s == NULL) {
        /* A class or struct name called without `new` is the one missing token
         * that produces a bare "undefined function" for a name that is right
         * there in the file, so it gets named rather than suggested. */
        if (p->cur_this == NULL && type_find_struct(p->ty, name) != NULL) {
            diag_note("'%s' is a type, not a function; construct one with 'new %s(...)'", name,
                      name);
            diag_error_code(span, "not_a_function", "'%s(...)' cannot be called: '%s' is a type",
                            name, name);
            e->type = NULL;
            return e;
        }
        suggest_in_scope(p, SK_FUNC, "function", name);
        diag_error_code(span, "undefined_function", "undefined function '%s'", name);
        e->type = NULL;
        return e;
    }
    e->name = arena_strdup(p->arena, (s->is_extern || s->is_export) ? ns_short_name(sym) : sym);
    if (s->nparams != n) {
        diag_note("'%s' is declared %s", name, sig_text(p, name, s));
        diag_error_code(span, "wrong_arity", "'%s' expects %d argument%s but got %d", name,
                        s->nparams, s->nparams == 1 ? "" : "s", n);
        e->type = NULL;
        return e;
    }
    if (n > Z_MAX_ARGS) {
        diag_error(span, "call to '%s' passes %d arguments but the limit is %d", name, n,
                   Z_MAX_ARGS);
        e->type = NULL;
        return e;
    }
    int said_sig = 0;
    for (int i = 0; i < n; i++) {
        /* A concrete value going to an interface parameter is boxed on the way
         * in, so the conversion is made before the types are compared. */
        if (is_kind(s->ptypes[i], TK_IFACE) && !is_kind(args[i]->type, TK_IFACE))
            args[i] = to_iface(p, args[i], s->ptypes[i], args[i]->span);
        if (!is_unk(args[i]->type) && !is_unk(s->ptypes[i]) &&
            !type_equals(args[i]->type, s->ptypes[i])) {
            /* Every wrong argument is reported -- fixing one and rebuilding to
             * discover the next is a worse loop than being told all at once --
             * but the declaration is shown once, since repeating it under each
             * one turns a diagnosis into a wall. */
            if (!said_sig) {
                diag_note("'%s' is declared %s", name, sig_text(p, name, s));
                said_sig = 1;
            }
            note_missing_class_pointer(p, s->ptypes[i], args[i]->type);
            if (type_widens_to(s->ptypes[i], args[i]->type))
                diag_note("an 'int' converts to a 'float' implicitly; the reverse "
                          "truncates, so it needs an explicit 'float(x)'");
            diag_error_code(args[i]->span, "type_mismatch",
                            "argument %d of '%s' expects '%s' but got '%s'", i + 1, name,
                            type_name(p->ty, s->ptypes[i]), type_name(p->ty, args[i]->type));
        }
    }
    e->type = s->ret;
    /* An `extern` target keeps its own symbol: the C definition is the one
     * that has to be found at link time. An `export`ed one does too, so calls
     * inside Z reach the same symbol C does. */
    e->is_extern = s->is_extern || s->is_export;
    return e;
}

/* Postfix suffixes: indexing, .length, postfix ++/--, and `?`. */
static Expr *parse_postfix(Parser *p, Expr *e);
static void expect_semi(Parser *p);

static Expr *parse_postfix(Parser *p, Expr *e) {
    for (;;) {
        if (at(p, T_QUESTION) && type_is_result(e->type)) {
            /* `expr?` -- yield the Ok payload, or return the error from here.
             *
             * This cannot desugar to statements: it is an expression, and the
             * early return has to happen in the middle of evaluating whatever
             * contains it. So it stays a node and the code generator emits the
             * branch inline, the same sequence an explicit `match` followed by a
             * `return` would produce.
             *
             * A `?` whose left side is *not* a Result is the ternary's, and is
             * left alone: the two share a token, and a Result is never a valid
             * condition, so the type decides and nothing well-typed is
             * ambiguous. */
            Span span = cur(p)->span;
            advance(p);
            Expr *t = new_expr(p, E_TRY, span);
            t->lhs = e;
            Type *rt = e->type;
            Type *ret = p->cur_ret;
            if (is_unk(ret) || !type_is_result(ret)) {
                diag_error(span,
                           "'?' can only be used in a function that returns a "
                           "'Result<T, E>'; this one returns '%s'",
                           is_unk(ret) ? "nothing" : type_name(p->ty, ret));
                t->type = NULL;
                e = t;
                continue;
            }
            Type *want_err = type_result_err(rt);
            Type *have_err = type_result_err(ret);
            if (!type_equals(want_err, have_err)) {
                diag_error(span, "'?' propagates '%s' but this function returns '%s'",
                           type_name(p->ty, want_err), type_name(p->ty, have_err));
                t->type = NULL;
                e = t;
                continue;
            }
            t->type = type_result_ok(rt);
            /* The early return this `?` may take is an exit from the function like
             * any other, so the values the scopes it leaves own have to go with it.
             *
             * Built *here*, and not in the destructor pass with the rest of them,
             * because the pass sees every local in the scope while the `?` must
             * see only the ones that exist at this point:
             *
             *     var a = mayFail()?;   // a not yet declared
             *     var b = new T();
             *
             * A `?` on the first line returns before `b` is ever initialised, so a
             * drop list that included `b` would run its destructor on whatever the
             * slot happened to hold. Built here, the list stops at `a`. */
            t->try_drops = build_drops_chain(p, p->scope, span);
            e = t;
            continue;
        }
        if (at(p, T_LBRACKET)) {
            Span span = cur(p)->span;
            advance(p);
            /* `s[a..b]`: a range, which for a string is a slice. Both ends are
             * optional, because "the rest of this" and "the head of this" are
             * the two things a range is asked for most and writing `s[0..s.length]`
             * or `s[n..]` for them is noise. An omitted end becomes the literal 0
             * or the string's own length, so a half-open range still has a
             * half-open answer. */
            int is_range = at(p, T_DOTDOT);
            Expr *idx;
            if (is_range) {
                idx = new_expr(p, E_INT, span);
                idx->ival = 0;
                idx->type = type_int(p->ty);
            } else {
                idx = parse_expr(p);
            }
            if (is_range || at(p, T_DOTDOT)) {
                if (!match(p, T_DOTDOT))
                    diag_error(cur(p)->span, "expected '..' in a range");
                Expr *hi;
                if (at(p, T_RBRACKET)) {
                    /* s[a..]: an omitted end is the length. Reading it from the
                     * string rather than storing it keeps a slice of a slice
                     * right, since the inner slice's length is its own. */
                    hi = new_expr(p, E_STRLEN, span);
                    hi->lhs = e;
                    hi->type = type_int(p->ty);
                } else {
                    hi = parse_expr(p);
                }
                if (!match(p, T_RBRACKET))
                    diag_error(cur(p)->span, "expected ']' after range");
                Expr *sl = new_expr(p, E_SLICE, span);
                sl->lhs = e;
                sl->rhs = idx;
                sl->env = hi; /* the exclusive end; `env` is the free slot */
                if (is_kind(e->type, TK_STRING)) {
                    sl->type = type_string(p->ty);
                } else {
                    if (!is_unk(e->type)) {
                        diag_error(span, "cannot take a range of %s; only a string has one",
                                   type_name(p->ty, e->type));
                    }
                    sl->type = NULL;
                }
                e = sl;
                continue;
            }
            if (!match(p, T_RBRACKET)) {
                diag_error(cur(p)->span, "expected ']' after index");
            }
            Expr *ix = new_expr(p, E_INDEX, span);
            ix->lhs = e;
            ix->rhs = idx;
            Type *bt = e->type;
            if (is_kind(bt, TK_ARRAY) || is_kind(bt, TK_PTR)) {
                ix->type = bt->base;
            } else if (is_kind(bt, TK_STRING)) {
                /* A byte, as an int. Z has no character type, so this is the one
                 * byte and the value is 0..255 -- the same reading `char_at` and
                 * `.length` have always had, now indexed rather than called. */
                if (!is_unk(idx->type) && !type_equals(idx->type, type_int(p->ty)))
                    diag_error(idx->span, "a string index must be 'int' but got '%s'",
                               type_name(p->ty, idx->type));
                ix->type = type_int(p->ty);
            } else {
                if (!is_unk(bt)) {
                    diag_error(span, "cannot index %s", type_name(p->ty, bt));
                }
                ix->type = NULL;
            }
            e = ix;
        } else if (at(p, T_DOT) || at(p, T_ARROW)) {
            /* `->` is C++'s way of saying "member of what this points at". Z's
             * `.` already resolves through a pointer, so `->` is desugared to an
             * explicit deref here and then handled by exactly the same code as
             * `.`. That keeps one implementation of member access rather than two
             * that can disagree, and it is why `p.f` and `p->f` mean the same
             * thing. */
            int is_arrow = at(p, T_ARROW);
            Span span = cur(p)->span;
            if (is_arrow) {
                advance(p);
                if (!is_unk(e->type) && !is_kind(e->type, TK_PTR) && !is_kind(e->type, TK_ARRAY)) {
                    diag_error(span, "'->' needs a pointer but got '%s'",
                               type_name(p->ty, e->type));
                } else if (!is_unk(e->type)) {
                    Expr *d = new_expr(p, E_DEREF, span);
                    d->lhs = e;
                    d->type = e->type->base;
                    e = d;
                }
            } else {
                advance(p);
            }
            if (!at(p, T_IDENT)) {
                diag_error(cur(p)->span, "expected field name after '%s'", is_arrow ? "->" : ".");
                return e;
            }
            char *name = cur(p)->text;
            advance(p);
            /* A call on an interface value: `v.m(args)`. The method name is
             * resolved to an index in the interface's declaration order, and that
             * index is what the call site loads from the itab. Everything about
             * the signature comes from the interface, not from whatever type
             * happens to be in the cell -- that is the point of the interface. */
            if (is_kind(e->type, TK_IFACE)) {
                IfaceDef *id = e->type->idef;
                IfaceMethod *im = iface_find_method(id, name);
                if (im == NULL) {
                    diag_error(span, "'%s' does not require a method '%s'",
                               id != NULL ? id->name : "this interface", name);
                    return e;
                }
                if (!at(p, T_LPAREN)) {
                    diag_error(span, "'%s' is a method, so it needs an argument list", name);
                    return e;
                }
                advance(p); /* '(' */
                int cap = 4, n = 0;
                Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
                if (!at(p, T_RPAREN)) {
                    for (;;) {
                        if (n == cap) {
                            int ncap = cap * 2;
                            Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                            memcpy(na, args, (size_t)n * sizeof(Expr *));
                            args = na;
                            cap = ncap;
                        }
                        args[n++] = parse_expr(p);
                        if (!match(p, T_COMMA))
                            break;
                    }
                }
                match(p, T_RPAREN);
                int want = im->sig->nparams;
                if (n != want) {
                    diag_error(span, "'%s' expects %d argument%s but got %d", name, want,
                               want == 1 ? "" : "s", n);
                } else {
                    for (int i = 0; i < n; i++) {
                        if (is_kind(im->sig->ptypes[i], TK_IFACE) &&
                            !is_kind(args[i]->type, TK_IFACE))
                            args[i] = to_iface(p, args[i], im->sig->ptypes[i], args[i]->span);
                        if (!is_unk(args[i]->type) && !is_unk(im->sig->ptypes[i]) &&
                            !type_equals(args[i]->type, im->sig->ptypes[i]))
                            diag_error(args[i]->span,
                                       "argument %d of '%s' expects '%s' but got '%s'", i + 1, name,
                                       type_name(p->ty, im->sig->ptypes[i]),
                                       type_name(p->ty, args[i]->type));
                    }
                }
                Expr *ic = new_expr(p, E_ICALL, span);
                ic->lhs = e;
                ic->args = args;
                ic->nargs = n;
                ic->vtable_index = (int)(im - id->methods);
                ic->type = im->sig->ret;
                e = parse_postfix(p, ic);
                continue;
            }
            /* method call: recv.Name(args) */
            Type *st = e->type;
            if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
                st = st->base;
            /* A field of function-pointer type, called like a method, is a call
             * through that pointer rather than a missing method. Detect it here
             * so the method-call block is skipped entirely and the field access
             * below runs; the postfix loop then dispatches the call. */
            int fp_field = 0;
            if (is_kind(st, TK_STRUCT)) {
                Field *fld0 = struct_find_field(st->sdef, name);
                fp_field =
                    fld0 != NULL && (is_kind(fld0->type, TK_FNPTR) || is_kind(fld0->type, TK_MPTR));
            }
            /* `&obj.M`: bind the receiver now and yield a callable. A *field* is
             * not a method however the name is spelled, so `&obj.f` has to fall
             * through to the field access below: it is the address of the field,
             * which is what the user wrote, and the alternative was a
             * `&container.data[i]` reading as "type has no method 'data'".
             * Function- and method-typed fields are excluded too, because those
             * really are called through the value that is the field. */
            if (p->take_method_addr && is_kind(st, TK_STRUCT) && !fp_field && !at(p, T_LPAREN) &&
                struct_find_field(st->sdef, name) == NULL) {
                StructMethod *bm = find_method(p, st->sdef, name);
                if (bm == NULL) {
                    suggest_member(st->sdef, "method", name);
                    diag_error_code(span, "no_such_method", "type %s has no method '%s'",
                                    st->sdef->name, name);
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                    continue;
                }
                if (!st->sdef->is_class) {
                    diag_error(span,
                               "cannot take a method pointer to struct '%s': its methods take "
                               "the receiver by value, so there is nothing to bind",
                               st->sdef->name);
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                    continue;
                }
                /* A class variable already holds the object pointer, so it is
                 * the receiver; `*p` on a class pointer yields the same. */
                Expr *rcv = e;
                if (is_kind(e->type, TK_PTR) && e->kind == E_DEREF)
                    rcv = e->lhs;
                /* A method's parameter list excludes the receiver, so nparams
                 * and ptypes are exactly what a caller sees. */
                int np = bm->nparams;
                Type **pts = NULL;
                if (np > 0) {
                    pts = arena_alloc_array(p->arena, (size_t)np, sizeof(Type *));
                    for (int i = 0; i < np; i++)
                        pts[i] = bm->ptypes[i];
                }
                /* Mangle against the method's own name, not the one that was
                 * written. Under a dialect those differ -- `&b.push` reaches the
                 * standard library's `append` -- and the symbol has to be the one
                 * the method was emitted under or the link fails on a name that
                 * was never defined. */
                const char *iname = bm->name;
                char *mang = arena_alloc(p->arena, strlen(st->sdef->name) + strlen(iname) + 3);
                snprintf(mang, strlen(st->sdef->name) + strlen(iname) + 3, "%s__%s", st->sdef->name,
                         iname);
                Expr *mp = new_expr(p, E_MPTR, span);
                mp->name = mang;
                mp->lhs = rcv;
                /* A virtual method is bound through the object's vtable, so the
                 * pointer dispatches on the runtime type rather than pinning the
                 * implementation named here. */
                mp->vtable_index = bm->is_virtual ? bm->vtable_index : -1;
                mp->type = type_mptr(p->ty, pts, np, bm->ret);
                e = mp;
                continue;
            }
            if (at(p, T_LPAREN) && is_kind(st, TK_STRUCT) && !fp_field) {
                StructMethod *m = find_method(p, st->sdef, name);
                if (m == NULL) {
                    suggest_member(st->sdef, "method", name);
                    diag_error_code(span, "no_such_method", "type %s has no method '%s'",
                                    type_name(p->ty, st), name);
                    while (!at(p, T_RPAREN) && !at(p, T_EOF))
                        advance(p);
                    match(p, T_RPAREN);
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                    continue;
                }
                advance(p); /* '(' */
                int cap = 4, n = 0;
                Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
                args[n++] = e; /* receiver becomes `this` */
                if (n == cap) {
                    int ncap = cap * 2;
                    Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                    memcpy(na, args, (size_t)n * sizeof(Expr *));
                    args = na;
                    cap = ncap;
                }
                if (!at(p, T_RPAREN)) {
                    for (;;) {
                        if (n == cap) {
                            int ncap = cap * 2;
                            Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                            memcpy(na, args, (size_t)n * sizeof(Expr *));
                            args = na;
                            cap = ncap;
                        }
                        args[n++] = parse_expr(p);
                        if (!match(p, T_COMMA))
                            break;
                    }
                }
                if (!match(p, T_RPAREN)) {
                    diag_error(cur(p)->span, "expected ')' after method arguments");
                }
                int nargs = n - 1;
                if (nargs != m->nparams) {
                    diag_error(span, "'%s' expects %d argument%s but got %d", name, m->nparams,
                               m->nparams == 1 ? "" : "s", nargs);
                } else {
                    for (int i = 0; i < nargs; i++) {
                        if (!is_unk(args[i + 1]->type) && !is_unk(m->ptypes[i]) &&
                            !type_equals(args[i + 1]->type, m->ptypes[i])) {
                            diag_error(args[i + 1]->span,
                                       "argument %d of '%s' expects '%s' but got '%s'", i + 1, name,
                                       type_name(p->ty, m->ptypes[i]),
                                       type_name(p->ty, args[i + 1]->type));
                        }
                    }
                }
                /* Mangle against the DECLARING class so inherited non-virtual
                 * methods call the base implementation. */
                /* The method's own name again, for the same reason: the owner is
                 * found by the internal name and the symbol is built from it. */
                const char *iname = m->name;
                StructDef *owner = struct_method_owner(st->sdef, iname);
                if (owner == NULL)
                    owner = st->sdef;
                char *mang = arena_alloc(p->arena, strlen(owner->name) + strlen(iname) + 3);
                snprintf(mang, strlen(owner->name) + strlen(iname) + 3, "%s__%s", owner->name,
                         iname);
                if (st->sdef->is_class && m->is_virtual) {
                    /* Dynamic dispatch through the receiver's vtable. */
                    Expr *call = new_expr(p, E_VCALL, span);
                    call->name = mang; /* direct fallback / debug */
                    call->vtable_index = m->vtable_index;
                    call->args = args;
                    call->nargs = n;
                    call->type = m->ret;
                    e = call;
                    continue;
                }
                Expr *call = new_expr(p, E_CALL, span);
                call->name = mang;
                call->args = args;
                call->nargs = n;
                call->type = m->ret;
                e = call;
                continue;
            }
            /* Extension method: recv.Name(args) -> Name(recv, args). */
            if (at(p, T_LPAREN)) {
                Sig *ext = NULL;
                for (int si = 0; si < p->nsigs; si++) {
                    Sig *cand = &p->sigs[si];
                    if (cand->is_ext && strcmp(cand->name, name) == 0) {
                        if (!is_unk(e->type) && !is_unk(cand->ext_recv) &&
                            type_equals(e->type, cand->ext_recv)) {
                            ext = cand;
                            break;
                        }
                        if (ext == NULL)
                            ext = cand; /* fallback: first by name */
                    }
                }
                if (ext != NULL) {
                    advance(p); /* '(' */
                    int cap = 4, n = 0;
                    Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
                    args[n++] = e; /* receiver becomes the first argument */
                    if (n == cap) {
                        int ncap = cap * 2;
                        Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                        memcpy(na, args, (size_t)n * sizeof(Expr *));
                        args = na;
                        cap = ncap;
                    }
                    if (!at(p, T_RPAREN)) {
                        for (;;) {
                            if (n == cap) {
                                int ncap = cap * 2;
                                Expr **na =
                                    arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
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
                        diag_error(cur(p)->span, "expected ')' after method arguments");
                    if (n != ext->nparams) {
                        diag_error(span, "'%s' expects %d argument%s but got %d", name,
                                   ext->nparams, ext->nparams == 1 ? "" : "s", n - 1);
                    }
                    Expr *call = new_expr(p, E_CALL, span);
                    call->name = arena_strdup(p->arena, ext->name);
                    call->args = args;
                    call->nargs = n;
                    call->type = ext->ret;
                    e = call;
                    continue;
                }
            }
            /* `.length` is the length of an array or a string, and on a string
             * it is a load of the header rather than a scan: the length is
             * stored, so `len(s)` is now a field read. */
            if ((is_kind(e->type, TK_ARRAY) || is_kind(e->type, TK_STRING)) &&
                strcmp(name, "length") == 0) {
                int is_str = is_kind(e->type, TK_STRING);
                Expr *f = new_expr(p, is_str ? E_STRLEN : E_FIELD, span);
                f->lhs = e;
                f->name = name;
                f->type = type_int(p->ty);
                e = f;
            } else {
                /* Struct field access: s.f (also through a struct pointer). */
                Type *st = e->type;
                if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
                    st = st->base;
                if (is_kind(st, TK_STRUCT)) {
                    Field *fld = struct_find_field(st->sdef, name);
                    if (fld == NULL) {
                        suggest_member(st->sdef, "field or method", name);
                        diag_error_code(span, "no_such_field", "type %s has no field '%s'",
                                        type_name(p->ty, st), name);
                        e = new_expr(p, E_INT, span);
                        e->type = NULL;
                    } else {
                        Expr *f = new_expr(p, E_FIELD, span);
                        f->lhs = e;
                        f->name = name;
                        f->field_off = fld->offset;
                        f->type = fld->type;
                        e = f;
                    }
                } else {
                    if (!is_unk(e->type)) {
                        suggest_member(sdef_of(e->type), "field or method", name);
                        diag_error_code(span, "no_such_field", "type %s has no field '%s'",
                                        type_name(p->ty, e->type), name);
                    }
                    e = new_expr(p, E_INT, span);
                    e->type = NULL;
                }
            }
        } else if (at(p, T_PLUSPLUS) || at(p, T_MINUSMINUS)) {
            /* Postfix `x++` yields the *old* value. See E_POSTINC for why that
             * cannot be a rewrite to `x = x + 1`. */
            TokenKind op = cur(p)->kind == T_PLUSPLUS ? T_PLUS : T_MINUS;
            Span span = cur(p)->span;
            advance(p);
            if (!is_lvalue(e) || !is_kind(e->type, TK_INT)) {
                if (is_lvalue(e) && !is_unk(e->type)) {
                    diag_error(span, "operator %s requires 'int'", token_kind_name(op));
                }
                e = new_expr(p, E_INT, span);
                e->type = type_int(p->ty);
                e->ival = 0;
                continue;
            }
            Expr *inc = new_expr(p, E_POSTINC, span);
            inc->op = op;
            inc->lhs = e;
            inc->type = type_int(p->ty);
            e = inc;
        } else if (at(p, T_LPAREN) && (is_kind(e->type, TK_FNPTR) || is_kind(e->type, TK_MPTR))) {
            /* A call through a function pointer reached by a postfix: a struct
             * field, an array element, or a dereference. The bare-identifier
             * case is handled in parse_primary, where the name is still
             * current; here it is not. */
            Span span = cur(p)->span;
            Type *ft = e->type;
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
            if (n != ft->nparams) {
                diag_error(span, "function pointer expects %d argument%s but got %d", ft->nparams,
                           ft->nparams == 1 ? "" : "s", n);
            } else {
                for (int i = 0; i < n; i++) {
                    if (!is_unk(args[i]->type) && !type_equals(args[i]->type, ft->ptypes[i]))
                        diag_error(args[i]->span, "argument %d expects '%s' but got '%s'", i + 1,
                                   type_name(p->ty, ft->ptypes[i]),
                                   type_name(p->ty, args[i]->type));
                }
            }
            /* A bound receiver occupies rdi, which is also where a
             * struct-returning call puts its result buffer. A plain function
             * pointer has no receiver, so it returns a struct normally. */
            if (is_kind(ft, TK_MPTR) && (is_kind(ft->ret, TK_STRUCT) || is_kind(ft->ret, TK_UNION)))
                diag_error(span, "a method pointer cannot return a struct");
            Expr *ic = new_expr(p, E_ICALL, span);
            ic->lhs = e;
            ic->args = args;
            ic->nargs = n;
            ic->type = ft->ret;
            e = ic;
        } else {
            break;
        }
    }
    return e;
}

/* Parses a base type followed by `*` stars but NOT `[]` (used by `new T[n]`,
 * where the brackets are the element count). */
static Type *parse_type_base(Parser *p) {
    /* `new fn(int) -> int[3]` allocates an array of function pointers, so a
     * function type is a valid element type here. */
    if (at(p, T_KW_FN) || at(p, T_KW_METHOD) || at(p, T_KW_CLOSURE)) {
        Type *ft = parse_fn_type(p);
        if (ft != NULL) {
            while (at(p, T_STAR)) {
                advance(p);
                ft = type_ptr(p->ty, ft);
            }
        }
        return ft;
    }
    {
        char path[256];
        int end = 0;
        if (ns_scan_path(p, p->pos, path, sizeof path, &end)) {
            Type *nt = ns_lookup_plain_type(p, path);
            if (nt == NULL)
                return NULL;
            while (p->pos <= end)
                advance(p);
            while (at(p, T_STAR)) {
                advance(p);
                nt = type_ptr(p->ty, nt);
            }
            return nt;
        }
    }
    Type *t = base_type_or_name(p, cur(p));
    if (t == NULL)
        return NULL;
    advance(p);
    while (at(p, T_STAR)) {
        advance(p);
        t = type_ptr(p->ty, t);
    }
    return t;
}

/* new Name(a, b, ...) -> a struct value; new T[n] -> a heap array. */
/* Builds the expression for `new T(args)` once the type is settled: reads the
 * argument list, checks it against the constructor, and returns either a heap
 * object (`T` is a class) or a struct value.
 *
 * Split out from parse_new because a generic type reaches here by two routes --
 * `new Vec<int>()` instantiates the type and then wants the same argument
 * handling as `new Vec()` -- and the handling is the same in both cases, so it
 * is written once. `written` is the name as the reader spelled it, which is what
 * diagnostics and the constructor lookup use; `st` is the type actually being
 * built, whose name is the mangled one for an instance. */
static Expr *parse_new_args(Parser *p, Span start, Type *st, const char *written) {
    int cap = 4, n = 0;
    Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (n == cap) {
                int ncap = cap * 2;
                Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
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
        diag_error(cur(p)->span, "expected ')' after arguments to 'new'");
    if (st->sdef->is_class) {
        /* `new C(args)`: heap object. A constructor is a method named like the
         * class; if absent, fields are simply zero-initialized. */
        StructMethod *ctor = find_method(p, st->sdef, st->sdef->name);
        Expr *e = new_expr(p, E_NEWCLASS, start);
        e->args = args;
        e->nargs = n;
        e->type = type_ptr(p->ty, st);
        if (ctor != NULL) {
            if (ctor->nparams != n) {
                diag_error_code(start, "wrong_arity",
                                "constructor '%s' expects %d argument%s but got %d", written,
                                ctor->nparams, ctor->nparams == 1 ? "" : "s", n);
            }
            /* The symbol is the instance's, because the instance's is what was
             * emitted; a `Vec<int>` ctor and a `Vec<string>` ctor are two
             * different functions with two different bodies. */
            char *cm = arena_alloc(p->arena, strlen(st->sdef->name) * 2 + 4);
            snprintf(cm, strlen(st->sdef->name) * 2 + 4, "%s__%s", st->sdef->name, st->sdef->name);
            e->name = cm;
        }
        return e;
    }
    /* The positional constructor takes only the real (non-property) fields. */
    int nreal = 0;
    for (int i = 0; i < st->sdef->nfields; i++) {
        if (!st->sdef->fields[i].is_prop)
            nreal++;
    }
    if (n != nreal) {
        diag_error_code(start, "wrong_arity", "'%s' expects %d field%s but got %d", written, nreal,
                        nreal == 1 ? "" : "s", n);
    } else {
        int fi = 0;
        for (int i = 0; i < st->sdef->nfields; i++) {
            Field *f = &st->sdef->fields[i];
            if (f->is_prop)
                continue;
            if (fi < n && !is_unk(args[fi]->type) && !type_equals(args[fi]->type, f->type)) {
                diag_error(args[fi]->span, "field '%s' expects '%s' but got '%s'", f->name,
                           type_name(p->ty, f->type), type_name(p->ty, args[fi]->type));
            }
            fi++;
        }
    }
    Expr *e = new_expr(p, E_STRUCTLIT, start);
    e->name = arena_strdup(p->arena, st->sdef->name);
    e->args = args;
    e->nargs = n;
    e->type = st;
    return e;
}

static Expr *parse_new(Parser *p) {
    Span start = cur(p)->span;
    advance(p); /* 'new' */

    /* `new Vec<int>(args)`: a generic type being constructed.
     *
     * Handled before the plain `new Name(args)` shape because the name is
     * followed by `<` rather than `(`, and the instance has to exist before the
     * argument list can be checked against its constructor. The arguments are
     * then parsed exactly as they are for a plain class -- this only decides
     * *which* type is being built. */
    if (cur(p)->kind == T_IDENT && peek(p, 1)->kind == T_LT) {
        GenericType *g = find_gtype(p, cur(p)->text);
        if (g == NULL)
            g = ns_lookup(p, cur(p)->text, look_gtype);
        if (g != NULL) {
            Type *t = parse_gtype(p, g);
            if (t == NULL)
                return new_expr(p, E_INT, start);
            if (!at(p, T_LPAREN)) {
                diag_error(cur(p)->span, "expected '(' after '%s' to construct one", g->name);
                return new_expr(p, E_INT, start);
            }
            advance(p);
            return parse_new_args(p, start, t, g->name);
        }
    }

    /* Struct literal: `new Name(args)`, or `new ns.Name(args)`. */
    {
        char path[256];
        int end = 0;
        if (ns_scan_path(p, p->pos, path, sizeof path, &end) && end + 1 < p->ntoks &&
            p->toks[end + 1].kind == T_LPAREN) {
            while (p->pos <= end)
                advance(p);
            advance(p); /* '(' */
            Type *st = type_find_struct(p->ty, path);
            if (st == NULL) {
                suggest_in_scope(p, SK_TYPE, "type", path);
                diag_error_code(start, "unknown_type", "unknown type '%s'", path);
                int guard = 0;
                while (!at(p, T_RPAREN) && !at(p, T_EOF) && guard++ < 256)
                    advance(p);
                match(p, T_RPAREN);
                return new_expr(p, E_INT, start);
            }
            return parse_new_args(p, start, st, path);
        }
        if (ns_scan_path(p, p->pos, path, sizeof path, &end) && end + 1 < p->ntoks &&
            p->toks[end + 1].kind == T_LT) {
            GenericType *gns = find_gtype(p, path);
            if (gns != NULL) {
                while (p->pos < end)
                    advance(p);
                Type *t = parse_gtype(p, gns);
                if (t == NULL)
                    return new_expr(p, E_INT, start);
                if (!at(p, T_LPAREN)) {
                    diag_error(cur(p)->span, "expected '(' after '%s' to construct one", path);
                    return new_expr(p, E_INT, start);
                }
                advance(p);
                return parse_new_args(p, start, t, path);
            }
        }
    }
    if (cur(p)->kind == T_IDENT && peek(p, 1)->kind == T_LPAREN) {
        char *sname = cur(p)->text;
        advance(p);
        advance(p); /* '(' */
        Type *st = type_find_struct(p->ty, sname);
        if (st == NULL)
            st = ns_lookup(p, sname, look_struct);
        /* The dialect is the second attempt, not the first: a project that has
         * declared its own `TextBuffer` keeps it. `parse_new_args` is handed the
         * written name so the diagnostic quotes what the program wrote. */
        if (st == NULL)
            st = type_find_struct(p->ty, surf(p, sname));
        if (st == NULL) {
            suggest_in_scope(p, SK_TYPE, "type", sname);
            diag_error_code(start, "unknown_type", "unknown type '%s'", sname);
            int guard = 0;
            while (!at(p, T_RPAREN) && !at(p, T_EOF) && guard++ < 256)
                advance(p);
            match(p, T_RPAREN);
            return new_expr(p, E_INT, start);
        }
        return parse_new_args(p, start, st, sname);
    }

    Type *base = parse_type_base(p);
    if (base == NULL) {
        diag_error(cur(p)->span, "expected element type after 'new'");
        return new_expr(p, E_INT, start);
    }
    if (!match(p, T_LBRACKET)) {
        diag_error(cur(p)->span, "expected '[' after 'new' element type");
        return new_expr(p, E_INT, start);
    }
    Expr *count = parse_expr(p);
    if (!match(p, T_RBRACKET)) {
        diag_error(cur(p)->span, "expected ']' after array length");
    }
    if (!is_unk(count->type) && !is_kind(count->type, TK_INT)) {
        diag_error(start, "array length must be 'int' but got %s", type_name(p->ty, count->type));
    } else if (count->kind == E_INT && count->ival <= 0) {
        /* Zero elements is a diagnostic rather than a one-element array.
         *
         * `new T[0]` used to allocate an array of length zero, and every index
         * into it was then a bounds failure at run time: `a[0] = 1` printed
         * "array index 0 out of bounds (length 0)" and aborted. That is a run
         * time error about a thing the reader could have been told about while
         * reading the program, and the reader cannot act on it there.
         *
         * A negative count is the same mistake with the sign wrong, and it
         * reaches the allocator as a size_t, so it is refused here too rather
         * than turned into an enormous request. */
        diag_error(start, "array length must be at least 1 but got %lld", count->ival);
    }
    Expr *e = new_expr(p, E_NEW, start);
    e->lhs = count;
    e->type = type_array(p->ty, base, -1);
    return e;
}

/* Builds an interned string literal expression from a byte range, returning a
 * fresh E_STRING node of type string. */
static Expr *make_str_expr(Parser *p, const char *bytes, int len, Span span) {
    Expr *e = new_expr(p, E_STRING, span);
    e->str_id = string_intern(p->strings, bytes, len);
    e->type = type_string(p->ty);
    return e;
}

/* Builds a literal segment of an interpolated string, dropping the backslash
 * from an escaped brace.
 *
 * The lexer leaves `\{` and `\}` in an interpolated string's raw text precisely
 * so that this pass can tell a literal brace from a hole delimiter: by the time
 * the holes are split out, an escaped brace and a real one are otherwise the
 * same byte, and `$\"a\{b\"` would try to interpolate a variable named b. */
static Expr *make_interp_part(Parser *p, const char *src, int len, Span span) {
    char *out = arena_alloc(p->arena, (size_t)len + 1);
    int n = 0;
    for (int i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len && (src[i + 1] == '{' || src[i + 1] == '}'))
            i++;
        out[n++] = src[i];
    }
    return make_str_expr(p, out, n, span);
}

/* Lexes and parses an embedded interpolation expression (the text inside
 * {..} of $"...{expr}...") as a sub-stream, sharing the enclosing scope and
 * type context so local variables and the string table resolve correctly. */
static Expr *parse_embedded(Parser *p, const char *src, int len, Span span) {
    (void)span;
    int n = 0;
    /* Use the real string table so embedded string literals share ids with
     * the outer program. */
    Token *toks = lex_all(p->arena, src, len, p->strings, &n);
    Parser sub = *p; /* share arena, scope, types */
    sub.toks = toks;
    sub.ntoks = n;
    sub.pos = 0;
    Expr *e = parse_expr(&sub);
    return e;
}

/* Parses an interpolated string token into a left-associative chain of string
 * concatenations: $"a{x}b" becomes "a" + x + "b". Embedded holes are parsed
 * with parse_embedded. Returns an expression of type string. */
static Expr *parse_interp(Parser *p, Token *t) {
    Span span = t->span;
    Expr *acc = NULL;
    const char *s = t->text;
    const char *seg = s;
    const char *cur = s;
    while (*cur) {
        /* An escaped brace is a literal brace, never a hole delimiter, and it is
         * two bytes here rather than one. */
        if (*cur == '\\' && cur[1] != '\0') {
            cur += 2;
            continue;
        }
        if (*cur == '{') {
            int litlen = (int)(cur - seg);
            Expr *lit = make_interp_part(p, seg, litlen, span);
            acc = acc ? make_binary(p, T_PLUS, acc, lit, span) : lit;
            /* Scan to the matching '}', stepping over escapes so a brace inside
             * a string literal in the hole does not close it early. */
            const char *e = cur + 1;
            int depth = 1;
            while (*e && depth > 0) {
                if (*e == '\\' && e[1] != '\0') {
                    e += 2;
                    continue;
                }
                if (*e == '{')
                    depth++;
                else if (*e == '}')
                    depth--;
                if (depth > 0)
                    e++;
            }
            if (depth > 0) {
                diag_error(span, "unterminated '{' in an interpolated string");
                e = cur + 1 + strlen(cur + 1);
            }
            int exprlen = (int)(e - (cur + 1));
            Expr *sub = parse_embedded(p, cur + 1, exprlen, span);
            acc = acc ? make_binary(p, T_PLUS, acc, sub, span) : sub;
            cur = (*e == '}') ? e + 1 : e;
            seg = cur;
        } else {
            cur++;
        }
    }
    int litlen = (int)(cur - seg);
    Expr *tail = make_interp_part(p, seg, litlen, span);
    acc = acc ? make_binary(p, T_PLUS, acc, tail, span) : tail;
    return acc;
}

/* Builds `Variant(args)` once the variant and its owner are known -- either
 * by the global search for a bare name or by the namespace search for a
 * qualified one. `disp` is the spelling diagnostics quote. Handles the
 * `Ok`/`Err` expectation the same way for both. */
static Expr *parse_variant_call(Parser *p, VariantDef *vd, UnionDef *owner, const char *disp,
                                Span span) {
    /* `Ok` and `Err` are the variants of every Result, so the name alone
     * does not say which one is meant: type_find_variant returns whichever
     * Result was interned first, and `Ok(true)` in a function returning
     * `Result<bool,string>` would build a `Result<int,string>`. The
     * expectation disambiguates, so the variant is re-pointed at the
     * expected Result's own before anything is checked against it. */
    Type *want = NULL;
    if (type_is_result(type_find_union(p->ty, owner->name))) {
        want = p->expect_result;
        if (want == NULL || !type_is_result(want)) {
            diag_error(span,
                       "'%s' needs to know the other type: declare the variable's "
                       "type, or use it where a 'Result<T, E>' is expected",
                       disp);
            advance(p);
            while (at(p, T_IDENT) || at(p, T_LPAREN) || at(p, T_RPAREN))
                advance(p);
            match(p, T_RPAREN);
            Expr *bad = new_expr(p, E_INT, span);
            bad->type = NULL;
            return parse_postfix(p, bad);
        }
        int idx = strcmp(vd->name, "Ok") == 0 ? 0 : 1;
        if (want->udef->nvariants > idx) {
            vd = &want->udef->variants[idx];
            owner = want->udef;
        }
    }
    advance(p); /* '(' */
    int cap = 4, n = 0;
    Expr **args = arena_alloc_array(p->arena, (size_t)cap, sizeof(Expr *));
    if (!at(p, T_RPAREN)) {
        for (;;) {
            if (n == cap) {
                int ncap = cap * 2;
                Expr **na = arena_alloc_array(p->arena, (size_t)ncap, sizeof(Expr *));
                memcpy(na, args, (size_t)n * sizeof(Expr *));
                args = na;
                cap = ncap;
            }
            args[n++] = parse_expr(p);
            if (!match(p, T_COMMA))
                break;
        }
    }
    if (!match(p, T_RPAREN)) {
        diag_error(cur(p)->span, "expected ')' after variant fields");
    }
    if (n != vd->nfields) {
        diag_error(span, "'%s' expects %d field%s but got %d", disp, vd->nfields,
                   vd->nfields == 1 ? "" : "s", n);
    } else {
        for (int i = 0; i < n; i++) {
            if (!is_unk(args[i]->type) && !type_equals(args[i]->type, vd->fields[i].type)) {
                diag_error(args[i]->span, "field '%s' expects '%s' but got '%s'",
                           vd->fields[i].name, type_name(p->ty, vd->fields[i].type),
                           type_name(p->ty, args[i]->type));
            }
        }
    }
    Expr *e = new_expr(p, E_UNIONLIT, span);
    e->variant = vd;
    e->args = args;
    e->nargs = n;
    e->type = want != NULL ? want : type_find_union(p->ty, owner->name);
    return parse_postfix(p, e);
}

/* The C spelling of an internal name: the last segment. `extern` and
 * `export` bind the C symbol as written, so a call resolved through a
 * namespace still emits `c_add`, never `n.c_add`. */
static const char *ns_short_name(const char *internal) {
    const char *dot = strrchr(internal, '.');
    return dot != NULL ? dot + 1 : internal;
}

/* Splits an internal dotted path into qualifier + member: `netlink` + `B`
 * for `netlink.B`. Writes the qualifier (possibly "") and returns the member. */
static const char *ns_split_member(const char *path, char *qual, size_t cap) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        if (cap > 0)
            qual[0] = '\0';
        return path;
    }
    size_t ql = (size_t)(dot - path);
    if (ql >= cap)
        ql = cap - 1;
    memcpy(qual, path, ql);
    qual[ql] = '\0';
    return dot + 1;
}

/* A namespace-qualified name: `netlink.Lobby`, `netlink.connect(...)`.
 *
 * Only when the head is a declared namespace and NOT a value in scope -- a
 * local shades a namespace, exactly like the surface-name rule -- so every
 * `a.b` over values keeps its old meaning untouched. Returns NULL to fall
 * through to the ordinary paths (including their diagnostics) when the path
 * names nothing known. */
static Expr *parse_ns_qualified(Parser *p, char *first, Span span) {
    if (peek(p, 1)->kind != T_DOT || peek(p, 2)->kind != T_IDENT)
        return NULL;
    if (lookup_var(p, first) != NULL)
        return NULL;
    char path[256];
    int end = 0;
    if (!ns_scan_path(p, p->pos, path, sizeof path, &end))
        return NULL;
    char internal[256];
    size_t ipl = strlen(path);
    if (ipl >= sizeof internal)
        return NULL;
    memcpy(internal, path, ipl + 1);
    int after = end + 1;
    int is_call = after < p->ntoks && p->toks[after].kind == T_LPAREN;
    if (is_call) {
        Sig *s = find_sig(p, internal);
        Generic *g = s != NULL ? NULL : find_generic(p, internal);
        if (s == NULL && g == NULL) {
            /* A qualified variant constructs its union: `netlink.B(5)`. */
            {
                char qual[256];
                const char *vmem = ns_split_member(internal, qual, sizeof qual);
                UnionDef *vowner = NULL;
                VariantDef *vvd = type_find_variant_in_ns(p->ty, vmem, qual, &vowner);
                if (vvd != NULL) {
                    while (p->pos < end)
                        advance(p);
                    advance(p);
                    return parse_variant_call(p, vvd, vowner, internal, span);
                }
            }
            if (ns_lookup_plain_type(p, internal) != NULL) {
                diag_error(span, "'%s' is a type, not a function; construct one with 'new %s(...)'",
                           internal, internal);
                return NULL;
            }
            if (const_find(p, internal) != NULL || find_global(p, internal) != NULL) {
                diag_error(span, "'%s' is not a function and cannot be called", internal);
                return NULL;
            }
            /* Otherwise resolve the call against the qualified spelling, so the
             * diagnostic names what was written. */
            while (p->pos < end)
                advance(p);
            advance(p);
            return parse_call(p, arena_strdup(p->arena, internal), span, 0);
        }
        while (p->pos < end)
            advance(p);
        advance(p); /* '.'-separated member, leaving '(' current */
        return parse_call(p, arena_strdup(p->arena, internal), span, 0);
    }
    /* Under `&`, a qualified function names its address, mirroring `&f`. */
    if (p->take_method_addr) {
        Sig *s = find_sig(p, internal);
        if (s != NULL) {
            while (p->pos <= end)
                advance(p);
            Expr *fe = new_expr(p, E_FNPTR, span);
            fe->name = arena_strdup(
                p->arena, (s->is_extern || s->is_export) ? ns_short_name(internal) : internal);
            fe->is_extern = s->is_extern || s->is_export;
            Type **pts = NULL;
            if (s->nparams > 0) {
                pts = arena_alloc_array(p->arena, (size_t)s->nparams, sizeof(Type *));
                for (int i = 0; i < s->nparams; i++)
                    pts[i] = s->ptypes[i];
            }
            fe->type = type_fnptr(p->ty, pts, s->nparams, s->ret);
            return fe;
        }
    }
    {
        ConstDef *c = const_find(p, internal);
        if (c != NULL) {
            while (p->pos <= end)
                advance(p);
            return parse_postfix(p, const_expr(p, c, span));
        }
    }
    {
        GlobalDef *gd = find_global(p, internal);
        if (gd != NULL) {
            Var *v = global_var_ref(p, gd);
            while (p->pos <= end)
                advance(p);
            Expr *e = new_expr(p, E_VAR, span);
            e->name = arena_strdup(p->arena, internal);
            e->gidx = gd->gidx;
            e->slot = -1;
            e->type = v != NULL ? v->type : gd->type;
            return parse_postfix(p, e);
        }
    }
    {
        char qual[256];
        const char *vmem = ns_split_member(internal, qual, sizeof qual);
        UnionDef *vowner = NULL;
        VariantDef *vvd = type_find_variant_in_ns(p->ty, vmem, qual, &vowner);
        if (vvd != NULL && vvd->nfields == 0) {
            while (p->pos <= end)
                advance(p);
            Expr *eu = new_expr(p, E_UNIONLIT, span);
            eu->variant = vvd;
            eu->type = type_find_union(p->ty, vowner->name);
            return parse_postfix(p, eu);
        }
    }
    if (ns_lookup_plain_type(p, internal) != NULL) {
        diag_error(span, "'%s' names a type, which is not a value", internal);
        return NULL;
    }
    /* A namespace known but no member by that name: resolve the call or the
     * read against the qualified spelling, so the diagnostic names what was
     * written rather than blaming the head alone. */
    if (is_call) {
        while (p->pos < end)
            advance(p);
        advance(p);
        return parse_call(p, arena_strdup(p->arena, internal), span, 0);
    }
    {
        /* Name the qualifier without its last segment: `netlink` in
         * `netlink.total`, `a.b` in `a.b.c`. */
        char qual[256];
        size_t ql = 0;
        for (int k = p->pos; k < end; k++) {
            if (p->toks[k].kind != T_IDENT)
                break;
            if (ql > 0 && ql + 1 < sizeof qual)
                qual[ql++] = '.';
            size_t tl = strlen(p->toks[k].text);
            if (ql + tl >= sizeof qual)
                break;
            memcpy(qual + ql, p->toks[k].text, tl + 1);
            ql += tl;
            if (k + 1 < end && p->toks[k + 1].kind == T_DOT)
                k++;
            else if (k + 1 < end)
                break;
        }
        qual[ql] = '\0';
        diag_error_code(span, "undefined_variable", "'%s' has no member '%s'", qual,
                        p->toks[end].text);
    }
    while (p->pos <= end)
        advance(p);
    Expr *bad = new_expr(p, E_VAR, span);
    bad->name = arena_strdup(p->arena, internal);
    bad->type = NULL;
    return bad;
}

static Expr *parse_qualified(Parser *p, char *base, Span span) {
    /* The base is still the current token, so the dot is one past it. */
    if (peek(p, 1)->kind != T_DOT)
        return NULL;
    Token *member = peek(p, 2);
    if (member->kind != T_IDENT)
        return NULL;
    /* Only a call. `Console.Thing` without an argument list is not a function
     * being named, and treating it as one would report an arity error about a
     * spelling the programmer may have meant as something else entirely. */
    if (peek(p, 3)->kind != T_LPAREN)
        return NULL;
    size_t n = strlen(base) + 1 + strlen(member->text);
    char *joined = arena_alloc(p->arena, n + 1);
    snprintf(joined, n + 1, "%s.%s", base, member->text);
    const char *internal = surface_lookup(p->surf, joined, n);
    if (internal == NULL)
        return NULL;
    /* A declared name of the same spelling is the user's, not the table's. */
    if (lookup_var(p, base) != NULL)
        return NULL;
    advance(p); /* the base name */
    advance(p); /* the '.' */
    advance(p); /* the member name, leaving '(' as the current token */
    return parse_call(p, (char *)internal, span, 1);
}
