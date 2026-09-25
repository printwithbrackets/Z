#ifndef VELA_CODEGEN_H
#define VELA_CODEGEN_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"

/* Lowers a checked program to x86-64 assembly text (Intel syntax, System V
 * AMD64 ABI). Returns a malloc'd NUL-terminated string that the caller writes
 * to a .s file and hands to the system assembler. */
char *codegen_emit(Arena *arena, Stmt *program, StringTable *strings);

#endif /* VELA_CODEGEN_H */
