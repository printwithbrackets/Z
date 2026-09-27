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
#define Z_INTRINSIC_LIST(X)                                                                 \
    X("abs", 1)                                                                             \
    X("min", 2)                                                                              \
    X("max", 2)                                                                              \
    X("clamp", 3)                                                                            \
    X("sqrt", 1)                                                                             \
    X("sin", 1)                                                                              \
    X("cos", 1)

/* The standard library, available without the program declaring it.
 *
 * Each entry is the name a program writes, the runtime symbol it lowers to, the
 * type it returns, and its parameter types. Types are single codes so a
 * signature stays readable in the table: i=int, b=bool, s=string, S=string[].
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
 * string's length from Z. */
#define Z_BUILTIN_LIST(X)                                                                   \
    X("len", "z_strlen", 'i', "s")                                                           \
    X("sub", "z_sub", 's', "sii")                                                            \
    X("index_of", "z_index_of", 'i', "ss")                                                   \
    X("index_of_byte", "z_index_of_byte", 'i', "si")                                         \
    X("last_index_of_byte", "z_last_index_of_byte", 'i', "si")                               \
    X("contains", "z_contains", 'b', "ss")                                                   \
    X("starts_with", "z_starts_with", 'b', "ss")                                             \
    X("ends_with", "z_ends_with", 'b', "ss")                                                 \
    X("char_at", "z_char_at", 'i', "si")                                                     \
    X("trim", "z_trim", 's', "s")                                                             \
    X("trim_start", "z_trim_start", 's', "s")                                                 \
    X("trim_end", "z_trim_end", 's', "s")                                                     \
    X("upper", "z_upper", 's', "s")                                                          \
    X("lower", "z_lower", 's', "s")                                                          \
    X("replace", "z_replace", 's', "sss")                                                    \
    X("repeat", "z_repeat", 's', "si")                                                       \
    X("reverse", "z_reverse", 's', "s")                                                      \
    X("pad_left", "z_pad_left", 's', "sii")                                                  \
    X("pad_right", "z_pad_right", 's', "sii")                                                \
    X("split", "z_split", 'S', "ss")                                                         \
    X("join", "z_join", 's', "sS")                                                           \
    X("int_to_string", "z_itoa", 's', "i")                                                  \
    X("float_to_string", "z_ftoa", 's', "f")                                                \
    X("char_str", "z_char_str", 's', "i")                                                   \
    X("pow", "z_pow", 'i', "ii")                                                             \
    X("gcd", "z_gcd", 'i', "ii")                                                             \
    X("lcm", "z_lcm", 'i', "ii")                                                             \
    /* StringBuilder's buffer primitives: a buffer the program owns, and the
     * in-place append that makes repeated appending amortized O(1). A builder in
     * Z is a class holding one of these, which keeps the growth policy next to
     * the allocation -- the one thing Z cannot do itself, because a string is
     * opaque bytes and there is no way to write past a string's length from Z. */ \
    X("str_buf_new", "z_str_buf_new", 's', "i")                                              \
    X("str_buf_append", "z_str_buf_append", 's', "ss")                                      \
    X("str_dup", "z_str_dup", 's', "s")

#endif /* Z_LIMITS_H */
