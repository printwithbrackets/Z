/* ---- expression parsing (precedence climbing) ---- */

static int binop_prec(TokenKind k) {
    switch (k) {
    case T_OR:
        return 1;
    case T_AND:
        return 2;
    case T_PIPE:
        return 3;
    case T_CARET:
        return 4;
    case T_AMP:
        return 5; /* binary &: shares its token with unary address-of */
    case T_EQ:
    case T_NE:
        return 6;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
        return 7;
    case T_SHL:
    case T_SHR:
        return 8;
    case T_PLUS:
    case T_MINUS:
        return 9;
    case T_STAR:
    case T_SLASH:
    case T_PERCENT:
        return 10;
    default:
        return 0;
    }
}

/* Arity of a built-in intrinsic, or 0 if the name is not one. */
static int intrinsic_arity(const char *name) {
#define Z_ARITY(n, k)                                                                              \
    if (strcmp(name, n) == 0)                                                                      \
        return k;
    Z_INTRINSIC_LIST(Z_ARITY)
#undef Z_ARITY
    return 0;
}

/* Materialises a concrete value as an interface value, when that is what the
 * destination type calls for. Returns `e` unchanged when no conversion is due,
 * so every site that assigns one expression to a place of a known type can call
 * this without testing first.
 *
 * Which types satisfy an interface is decided here rather than declared, so a
 * struct implements one by having the methods and nothing else. A class goes in
 * its object pointer; a struct value has to be copied to the heap first, because
 * the cell outlives whatever frame the value was in -- the same reason a closure
 * environment is on the heap. */
static Expr *to_iface(Parser *p, Expr *e, Type *want, Span span) {
    if (e == NULL || want == NULL || !is_kind(want, TK_IFACE) || is_unk(e->type))
        return e;
    if (is_kind(e->type, TK_IFACE))
        return e; /* interface to the same interface: the cell is already there */
    IfaceDef *id = want->idef;
    if (id == NULL)
        return e;
    int *slots =
        arena_alloc_array(p->arena, (size_t)(id->nmethods > 0 ? id->nmethods : 1), sizeof(int));
    if (!iface_implemented_by(p->ty, id, e->type, slots)) {
        /* Name the first method that is actually at fault, and say which kind of
         * fault it is: "does not implement I" on its own leaves the reader to work
         * out whether a method is absent or merely has the wrong shape, and
         * reporting a method that is present and correct is worse than useless. */
        const char *absent = NULL;
        const char *mismatched = NULL;
        StructDef *sd = is_kind(e->type, TK_STRUCT) ? e->type->sdef
                        : is_kind(e->type, TK_PTR) && is_kind(e->type->base, TK_STRUCT)
                            ? e->type->base->sdef
                            : NULL;
        if (sd != NULL) {
            for (int i = 0; i < id->nmethods; i++) {
                StructMethod *m = struct_find_method(sd, id->methods[i].name);
                if (m == NULL) {
                    if (absent == NULL)
                        absent = id->methods[i].name;
                } else if (mismatched == NULL && !iface_method_matches(m, id->methods[i].sig)) {
                    mismatched = id->methods[i].name;
                }
            }
        }
        if (sd == NULL)
            diag_error(span, "'%s' cannot implement '%s': it is not a struct or class",
                       type_name(p->ty, e->type), id->name);
        else if (absent != NULL)
            diag_error(span, "'%s' does not implement '%s': it has no method '%s'",
                       type_name(p->ty, e->type), id->name, absent);
        else if (mismatched != NULL)
            diag_error(span,
                       "'%s' does not implement '%s': '%s' does not match the "
                       "signature the interface requires",
                       type_name(p->ty, e->type), id->name, mismatched);
        else
            diag_error(span, "'%s' does not implement '%s'", type_name(p->ty, e->type), id->name);
        e->type = NULL;
        return e;
    }
    StructDef *impl = is_kind(e->type, TK_STRUCT) ? e->type->sdef : e->type->base->sdef;
    /* A class satisfies an interface through its vtable, so every method it
     * offers has to be virtual: a non-virtual one has no slot to dispatch
     * through, and an itab entry pointing at it directly would bind the
     * implementation the conversion site happened to name. */
    if (impl->is_class) {
        for (int i = 0; i < id->nmethods; i++) {
            StructMethod *m = struct_find_method(impl, id->methods[i].name);
            if (m == NULL || !m->is_virtual) {
                diag_error(span,
                           "'%s' cannot implement '%s' through a class: '%s' must be "
                           "declared 'virtual' to be dispatched dynamically",
                           type_name(p->ty, e->type), id->name, id->methods[i].name);
                e->type = NULL;
                return e;
            }
        }
    }
    Expr *c = new_expr(p, E_IFACE, span);
    c->lhs = e;
    c->type = want;
    c->impl = impl;
    c->idef = id;
    return c;
}

/* Looks a standard-library name up in Z_BUILTIN_LIST, returning the runtime
 * symbol to call and, through the out parameters, its return type code and its
 * parameter type codes. Returns NULL when the name is not a built-in. */
static const char *builtin_lookup(const char *name, char *ret_code, const char **params) {
#define Z_FIND(n, sym, ret, ps)                                                                    \
    if (strcmp(name, n) == 0) {                                                                    \
        *ret_code = ret;                                                                           \
        *params = ps;                                                                              \
        return sym;                                                                                \
    }
    Z_BUILTIN_LIST(Z_FIND)
#undef Z_FIND
    return NULL;
}

/* The type a signature code stands for. */
static Type *builtin_type(Parser *p, char code) {
    switch (code) {
    case 'i':
        return type_int(p->ty);
    case 'b':
        return type_bool(p->ty);
    case 's':
        return type_string(p->ty);
    case 'f':
        return type_f64(p->ty);
    case 'a':
        /* "Whatever this is". See TK_ANY. The argument's real type replaces it
         * at the call site, so the code generator can lower the call by it. */
        return type_any(p->ty);
    case 'S':
        /* The array length is -1 for every Z-written array type; the real
         * length lives in the header the runtime allocated. */
        return type_array(p->ty, type_string(p->ty), -1);
    default:
        return type_void(p->ty);
    }
}

static int is_cmp_op(TokenKind k) {
    return k == T_EQ || k == T_NE || k == T_LT || k == T_LE || k == T_GT || k == T_GE;
}

static int is_lvalue(Expr *e) {
    return e != NULL &&
           (e->kind == E_VAR || e->kind == E_INDEX || e->kind == E_DEREF || e->kind == E_FIELD);
}

/* Maps a binary operator to its operator-overload method-name suffix, or NULL
 * if the operator cannot be overloaded. */
static const char *op_method_suffix(TokenKind op) {
    switch (op) {
    case T_PLUS:
        return "Add";
    case T_MINUS:
        return "Sub";
    case T_STAR:
        return "Mul";
    case T_SLASH:
        return "Div";
    case T_PERCENT:
        return "Rem";
    case T_EQ:
        return "Eq";
    case T_NE:
        return "Neq";
    case T_LT:
        return "Lt";
    case T_LE:
        return "Le";
    case T_GT:
        return "Gt";
    case T_GE:
        return "Ge";
    default:
        return NULL;
    }
}

/* Folds a binary operation on two integer literals at compile time. Returns a
 * fresh E_INT, or NULL if the operands are not both integer literals. */
static Expr *fold_int_binary(Parser *p, TokenKind op, Expr *lhs, Expr *rhs, Span span) {
    if (lhs->kind != E_INT || rhs->kind != E_INT)
        return NULL;
    long long a = lhs->ival, b = rhs->ival, r = 0;
    switch (op) {
    case T_PLUS:
        r = a + b;
        break;
    case T_MINUS:
        r = a - b;
        break;
    case T_STAR:
        r = a * b;
        break;
    case T_SLASH:
        if (b == 0)
            return NULL; /* leave division by zero for runtime */
        r = a / b;
        break;
    case T_PERCENT:
        if (b == 0)
            return NULL;
        r = a % b;
        break;
    case T_AMP:
        r = a & b;
        break;
    case T_PIPE:
        r = a | b;
        break;
    case T_CARET:
        r = a ^ b;
        break;
    case T_SHL:
        if (b < 0 || b > 63)
            return NULL; /* undefined in C; leave it to the runtime check */
        r = a << b;
        break;
    case T_SHR:
        if (b < 0 || b > 63)
            return NULL;
        r = a >> b;
        break;
    case T_LT:
        r = a < b;
        break;
    case T_LE:
        r = a <= b;
        break;
    case T_GT:
        r = a > b;
        break;
    case T_GE:
        r = a >= b;
        break;
    case T_EQ:
        r = a == b;
        break;
    case T_NE:
        r = a != b;
        break;
    default:
        return NULL;
    }
    Expr *e = new_expr(p, E_INT, span);
    e->ival = r;
    e->type = (op == T_LT || op == T_LE || op == T_GT || op == T_GE || op == T_EQ || op == T_NE)
                  ? type_bool(p->ty)
                  : type_int(p->ty);
    return e;
}

/* Folds `a && b` / `a || b` on two boolean literals. */
static Expr *fold_bool_logic(Parser *p, TokenKind op, Expr *lhs, Expr *rhs, Span span) {
    if (lhs->kind != E_BOOL || rhs->kind != E_BOOL)
        return NULL;
    Expr *e = new_expr(p, E_BOOL, span);
    e->ival = op == T_AND ? (lhs->ival && rhs->ival) : (lhs->ival || rhs->ival);
    e->type = type_bool(p->ty);
    return e;
}

/* Wraps `e` in a conversion to `to`, unless it is already that type or its type
 * is not yet known. Both the implicit int-to-float widening and an explicit
 * `(float)` / `(int)` cast go through here, so the code generator only ever sees
 * one conversion node and never has to ask where a conversion came from. */
static Expr *make_cvt(Parser *p, Type *to, Expr *e, Span span) {
    if (e == NULL || to == NULL || is_unk(e->type) || type_equals(e->type, to))
        return e;
    Expr *c = new_expr(p, E_CVT, span);
    c->lhs = e;
    c->type = to;
    return c;
}

static Expr *make_binary(Parser *p, TokenKind op, Expr *lhs, Expr *rhs, Span span) {
    Expr *e = new_expr(p, E_BINARY, span);
    e->op = op;
    e->lhs = lhs;
    e->rhs = rhs;

    if (is_unk(lhs->type) || is_unk(rhs->type)) {
        e->type = NULL;
        return e;
    }

    /* Constant folding: fold binary operations on literal operands. */
    {
        Expr *folded = NULL;
        if (op == T_AND || op == T_OR)
            folded = fold_bool_logic(p, op, lhs, rhs, span);
        else
            folded = fold_int_binary(p, op, lhs, rhs, span);
        if (folded != NULL)
            return folded;
    }

    /* Operator overloading: if either operand is a struct declaring
     * `op_<Name>`, rewrite the binary expression into a method call on the
     * left operand. */
    const char *suffix = op_method_suffix(op);
    if (suffix != NULL) {
        Type *st = lhs->type;
        if (is_kind(st, TK_PTR) && is_kind(st->base, TK_STRUCT))
            st = st->base;
        if (is_kind(st, TK_STRUCT)) {
            char mname[64];
            snprintf(mname, sizeof mname, "op_%s", suffix);
            StructMethod *m = struct_find_method(st->sdef, mname);
            if (m != NULL && m->nparams == 1) {
                /* The RHS argument: if the operator takes a struct parameter it
                 * is passed by pointer, otherwise the scalar value is passed. */
                if (!is_unk(rhs->type) && !is_unk(m->ptypes[0])) {
                    int ok = is_kind(m->ptypes[0], TK_PTR)
                                 ? type_equals(m->ptypes[0]->base, rhs->type)
                                 : type_equals(m->ptypes[0], rhs->type);
                    if (!ok) {
                        Type *want =
                            is_kind(m->ptypes[0], TK_PTR) ? m->ptypes[0]->base : m->ptypes[0];
                        diag_error(span, "operator %s expects '%s' but got '%s'",
                                   token_kind_name(op), type_name(p->ty, want),
                                   type_name(p->ty, rhs->type));
                    }
                }
                char *mang = arena_alloc(p->arena, strlen(st->sdef->name) + strlen(mname) + 3);
                snprintf(mang, strlen(st->sdef->name) + strlen(mname) + 3, "%s__%s", st->sdef->name,
                         mname);
                Expr *call = new_expr(p, E_CALL, span);
                call->name = mang;
                Expr **args = arena_alloc_array(p->arena, 2, sizeof(Expr *));
                args[0] = lhs; /* receiver */
                args[1] = rhs;
                call->args = args;
                call->nargs = 2;
                call->type = m->ret;
                return call;
            }
        }
    }

    /* String concatenation: string+string, string+int, int+string. */
    if (op == T_PLUS && (is_kind(lhs->type, TK_STRING) || is_kind(rhs->type, TK_STRING))) {
        int lok = is_kind(lhs->type, TK_STRING) || is_kind(lhs->type, TK_INT) ||
                  is_kind(lhs->type, TK_BOOL) || is_kind(lhs->type, TK_F64);
        int rok = is_kind(rhs->type, TK_STRING) || is_kind(rhs->type, TK_INT) ||
                  is_kind(rhs->type, TK_BOOL) || is_kind(rhs->type, TK_F64);
        if (lok && rok) {
            e->type = type_string(p->ty);
            return e;
        }
    }

    if (op == T_AND || op == T_OR) {
        if (!is_kind(lhs->type, TK_BOOL) || !is_kind(rhs->type, TK_BOOL)) {
            diag_error(span, "operands of '%s' must be 'bool'", token_kind_name(op));
            e->type = NULL;
            return e;
        }
        e->type = type_bool(p->ty);
        return e;
    }

    /* A generic template is validated with TK_TYPEPARAM placeholders; the real
     * operand check runs again during each monomorphized instantiation. Allow
     * type-parameter operands here so the template body type-checks. */
    int lparam = is_kind(lhs->type, TK_TYPEPARAM);
    int rparam = is_kind(rhs->type, TK_TYPEPARAM);

    if (is_cmp_op(op)) {
        /* `p == null` / `p != null`: the literal carries no type of its own, so
         * compare it against any reference-like operand. */
        if ((lhs->kind == E_NULL || rhs->kind == E_NULL) && (op == T_EQ || op == T_NE)) {
            e->type = type_bool(p->ty);
            return e;
        }
        /* Two function pointers can be compared for identity, but not
         * ordered -- there is no meaningful "less than" for code addresses. */
        if (is_kind(lhs->type, TK_FNPTR) || is_kind(rhs->type, TK_FNPTR)) {
            int same = type_equals(lhs->type, rhs->type);
            if (!same || (op != T_EQ && op != T_NE)) {
                if (same) {
                    diag_error(span, "function pointers support only '==' and '!='");
                } else {
                    diag_error(span, "cannot compare '%s' with '%s'", type_name(p->ty, lhs->type),
                               type_name(p->ty, rhs->type));
                }
                e->type = NULL;
                return e;
            }
            e->type = type_bool(p->ty);
            return e;
        }
        int lok = is_kind(lhs->type, TK_INT) || is_kind(lhs->type, TK_BOOL) || lparam;
        int rok = is_kind(rhs->type, TK_INT) || is_kind(rhs->type, TK_BOOL) || rparam;
        /* Two floats compare in all six relational operators, and a float may be
         * compared with an int by widening the int -- so `x == 0` does not
         * quietly mean "the integer zero" for a float x. */
        if (is_kind(lhs->type, TK_F64) || is_kind(rhs->type, TK_F64)) {
            if (!lparam && !rparam) {
                if (!is_kind(lhs->type, TK_F64))
                    lhs = make_cvt(p, type_f64(p->ty), lhs, span);
                if (!is_kind(rhs->type, TK_F64))
                    rhs = make_cvt(p, type_f64(p->ty), rhs, span);
                e->lhs = lhs;
                e->rhs = rhs;
                e->type = type_bool(p->ty);
                return e;
            }
        }
        /* Strings compare lexicographically in all six relational operators. */
        int lstr = is_kind(lhs->type, TK_STRING);
        int rstr = is_kind(rhs->type, TK_STRING);
        int ok =
            (lstr && rstr) || ((lok || lstr) && (rok || rstr) && type_equals(lhs->type, rhs->type));
        if (!ok) {
            diag_error(span, "cannot compare '%s' with '%s'", type_name(p->ty, lhs->type),
                       type_name(p->ty, rhs->type));
            e->type = NULL;
            return e;
        }
        e->type = type_bool(p->ty);
        return e;
    }

    /* Arithmetic. A `float` operand makes the whole expression a `float`, and an
     * `int` operand is widened to match -- the one conversion Z performs on its
     * own, made explicit here as a node so the code generator has exactly one
     * shape to lower.
     *
     * `%` is the exception: a non-integer has no remainder, so it stays
     * integer-only rather than quietly truncating. */
    {
        int lf = is_kind(lhs->type, TK_F64), rf = is_kind(rhs->type, TK_F64);
        if ((lf || rf) && !lparam && !rparam) {
            int allowed = op == T_PLUS || op == T_MINUS || op == T_STAR || op == T_SLASH;
            if (!allowed) {
                diag_error(span, "operator %s is not defined for '%s' and '%s'",
                           token_kind_name(op), type_name(p->ty, lhs->type),
                           type_name(p->ty, rhs->type));
                e->type = NULL;
                return e;
            }
            if (!lf)
                lhs = make_cvt(p, type_f64(p->ty), lhs, span);
            if (!rf)
                rhs = make_cvt(p, type_f64(p->ty), rhs, span);
            e->lhs = lhs;
            e->rhs = rhs;
            e->type = type_f64(p->ty);
            return e;
        }
    }

    if ((!is_kind(lhs->type, TK_INT) && !lparam) || (!is_kind(rhs->type, TK_INT) && !rparam)) {
        diag_error(span, "operator %s is not defined for '%s' and '%s'", token_kind_name(op),
                   type_name(p->ty, lhs->type), type_name(p->ty, rhs->type));
        e->type = NULL;
        return e;
    }
    e->type = lparam ? lhs->type : type_int(p->ty);
    return e;
}
