#ifndef Z_LIMITS_H
#define Z_LIMITS_H

/* System V AMD64 passes integers in rdi, rsi, rdx, rcx, r8, r9 and the first eight
 * floating registers; anything past that spills to the stack, in argument order.
 * Codegen does that spilling, so this is a cap on how many arguments a function
 * may take rather than a limitation of the calling convention -- it exists so a
 * runaway generated signature has somewhere to stop.
 *
 * A function returning a struct or union takes a hidden first pointer to the
 * caller's result buffer, which spends one of the integer slots; that shifts
 * where the declared arguments start rather than costing an argument, so there
 * is no separate limit for it. */
#define Z_MAX_ARGS 16

/* Built-in functions the parser resolves to codegen instead of to a call.
 * Each entry is the name and its arity, so a miscalled intrinsic is diagnosed
 * the same way a miscalled user function is. A user function of the same name
 * shadows the intrinsic, so existing code keeps working. */
#define Z_INTRINSIC_LIST(X)                                                                        \
    X("abs", 1)                                                                                    \
    X("min", 2)                                                                                    \
    X("max", 2)                                                                                    \
    X("clamp", 3)                                                                                  \
    X("sqrt", 1)                                                                                   \
    X("sin", 1)                                                                                    \
    X("cos", 1)

/* The standard library, available without the program declaring it.
 *
 * Each entry is the name a program writes, the runtime symbol it lowers to, the
 * type it returns, and its parameter types. Types are single codes so a
 * signature stays readable in the table: i=int, b=bool, f=float, s=string,
 * S=string[], a=any.
 *
 * `a` is the one that needs explaining. A generic function cannot ask what it
 * was instantiated with in a way its branches can use: `typeof` answers, but
 * every branch after the test still has to type-check, and a branch that calls
 * int_to_string on a string does not. So the two questions that genuinely depend
 * on the type -- a value's text form, and a value's hash -- are asked of the code
 * generator through a parameter declared `any`. The generator knows the static
 * type at the call site and lowers the call accordingly, so `to_text(x)` and
 * `hash_of(x, cap)` mean the right thing for every element and key type a Map or
 * a Set can be instantiated with, and neither the standard library nor a user has
 * to declare an interface that `int` and `string` would then both have to
 * satisfy.
 *
 * These are checked like any other call. The parser builds real types from the
 * signature and reports an ordinary argument-type error, and the call carries a
 * result type, so `len(s) + 1` compiles, `len(3)` does not, and the result
 * composes with the rest of the language instead of being a special form. A user
 * function or extension method of the same name shadows the entry, which is what
 * lets a program define its own `len`.
 *
 * Where Z_INTRINSIC_LIST is lowered inline to a couple of instructions, every one
 * of these is a call into the runtime, so codegen only has to name the symbol.
 *
 * exp, log, tan and the rest of the transcendentals are deliberately absent: with
 * no floating-point type they would have to invent a fixed-point convention that
 * means nothing at the call site, which is the very thing sin/cos already have to
 * apologize for. They belong with a real float type.
 *
 * int_to_string, float_to_string and char_str are the number- and byte-to-text
 * operations under names of their own. `"" + i` does the same job and is the
 * right spelling inside an expression; these exist for the places where
 * building a string one piece at a time would otherwise allocate a temporary
 * string just to copy it into the next one.
 *
 * str_buf_new and str_buf_append are StringBuilder's buffer primitives: a buffer
 * the program owns, and the in-place append that makes repeated appending
 * amortized O(1). A builder written in Z is a class holding one of these, which
 * keeps the growth policy next to the allocation -- the one thing Z cannot do
 * itself, because a string is opaque bytes and there is no way to write past a
 * string's length from Z.
 *
 * str_free is the other half of that: it releases a string's bytes, and skips a
 * literal because a literal's header carries no capacity. It is declared here,
 * rather than emitted as a bare symbol by the destructor pass, so that the call
 * the compiler generates is type-checked like any other call and a wrong arity
 * is an ordinary argument error rather than an undefined symbol at link time.
 * str_dup is its counterpart and is the copy the compiler inserts when a
 * borrowed string is stored.
 *
 * hold is the odd one out, because its argument is usually not written as a
 * number. A duration literal is an ordinary integer count of seconds to
 * everything downstream -- `2h21m37` is 8497 -- so `hold(2h21m37)` and
 * `hold(8497)` are the same call and no duration type is needed anywhere. It is
 * here rather than a language form because a program that writes `hold(...)`
 * wants a call, and a call is what this lowers to. */
#define Z_BUILTIN_LIST(X)                                                                          \
    X("len", "z_strlen", 'i', "s")                                                                 \
    X("sub", "z_sub", 's', "sii")                                                                  \
    X("index_of", "z_index_of", 'i', "ss")                                                         \
    X("index_of_byte", "z_index_of_byte", 'i', "si")                                               \
    X("last_index_of_byte", "z_last_index_of_byte", 'i', "si")                                     \
    X("contains", "z_contains", 'b', "ss")                                                         \
    X("starts_with", "z_starts_with", 'b', "ss")                                                   \
    X("ends_with", "z_ends_with", 'b', "ss")                                                       \
    X("char_at", "z_char_at", 'i', "si")                                                           \
    X("trim", "z_trim", 's', "s")                                                                  \
    X("trim_start", "z_trim_start", 's', "s")                                                      \
    X("trim_end", "z_trim_end", 's', "s")                                                          \
    X("upper", "z_upper", 's', "s")                                                                \
    X("lower", "z_lower", 's', "s")                                                                \
    X("replace", "z_replace", 's', "sss")                                                          \
    X("repeat", "z_repeat", 's', "si")                                                             \
    X("reverse", "z_reverse", 's', "s")                                                            \
    X("pad_left", "z_pad_left", 's', "sii")                                                        \
    X("pad_right", "z_pad_right", 's', "sii")                                                      \
    X("split", "z_split", 'S', "ss")                                                               \
    X("join", "z_join", 's', "sS")                                                                 \
    X("int_to_string", "z_itoa", 's', "i")                                                         \
    X("float_to_string", "z_ftoa", 's', "f")                                                       \
    X("char_str", "z_char_str", 's', "i")                                                          \
    /* File descriptors, not FILE handles, so nothing on this side of the boundary                 \
     * owns a buffer the program cannot see. Each returns a plain int: a file open                 \
     * either gives a descriptor or does not, and lib/io.z turns that into a                       \
     * Result<_, IoError> where the error can carry errno and the path, which is                   \
     * the part with any use in it. */                                                             \
    X("open", "z_open", 'i', "ss")                                                                 \
    X("close", "z_close", 'i', "i")                                                                \
    X("read_byte", "z_read_byte", 'i', "i")                                                        \
    X("write_bytes", "z_write", 'i', "is")                                                         \
    X("io_errno", "z_io_errno", 'i', "")                                                           \
    X("io_eof", "z_io_eof", 'i', "")                                                               \
    X("hold", "z_hold", 'v', "a")                                                                  \
    X("die", "z_die", 'i', "i")                                                                    \
    X("exit", "z_exit", 'i', "i")                                                                  \
    X("to_text", "z_to_text", 's', "a")                                                            \
    X("hash_of", "z_hash_of", 'i', "ai")                                                           \
    X("pow", "z_pow", 'i', "ii")                                                                   \
    X("gcd", "z_gcd", 'i', "ii")                                                                   \
    X("lcm", "z_lcm", 'i', "ii")                                                                   \
    /* StringBuilder's buffer primitives: a buffer the program owns, and the                       \
     * in-place append that makes repeated appending amortized O(1). A builder in                  \
     * Z is a class holding one of these, which keeps the growth policy next to                    \
     * the allocation -- the one thing Z cannot do itself, because a string is                     \
     * opaque bytes and there is no way to write past a string's length from Z. */                 \
    X("str_buf_new", "z_str_buf_new", 's', "i")                                                    \
    X("str_buf_append", "z_str_buf_append", 's', "ss")                                             \
    X("str_free", "z_str_free", 'i', "s")                                                          \
    X("str_dup", "z_str_dup", 's', "s")

#endif /* Z_LIMITS_H */
