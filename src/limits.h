#ifndef Z_LIMITS_H
#define Z_LIMITS_H

/* System V AMD64 passes integers in rdi, rsi, rdx, rcx, r8, r9 and the first
 * six floating registers. Everything past that has to spill to the stack,
 * which codegen does not do, so the parser rejects anything wider instead of
 * emitting assembly that does not assemble. A function returning a struct or
 * union uses a hidden first pointer to the caller's result buffer, which
 * spends one of these slots. */
#define Z_MAX_ARGS 6
#define Z_MAX_ARGS_STRUCT_RET (Z_MAX_ARGS - 1)

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

#endif /* Z_LIMITS_H */
