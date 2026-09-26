# Z — language specification

This is the M0–M6a subset that is implemented and tested. It grows each
milestone; the grammar below is the source of truth for what the compiler
accepts today.

## Lexical

- Comments: `// line`, `/* block */`.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
- Integer literals: decimal digits (64-bit signed `int`).
- String literals: `"..."` with escapes `\n \t \r \0 \\ \"`.
- Keywords: `int bool string void var const new struct enum class match this if
  else while for foreach in return break continue true false null extern export
  virtual override fn method import`.
- Operators: `+ - * / %`, `== != < <= > >=`, `&& || !`, `& | ^ ~ << >>`,
  `= += -= *= /= %= &= |= ^= <<= >>=`, `++ --`, `( ) { } [ ] ; , .`.
- There are no hex, octal or binary literals; write decimal or build the value
  with shifts.

## Types

| Type | Meaning | Representation |
|------|---------|----------------|
| `int` | 64-bit signed integer | machine word in `rax` |
| `bool` | `true` / `false` | 0 or 1 |
| `string` | immutable NUL-terminated bytes | pointer to `.rodata` |
| `T*` | pointer to `T` | machine word |
| `T[]` | array of `T` | pointer to first element; length stored at `ptr[-8]` |
| `struct S` | user-defined value type | inline in the frame; fields at byte offsets |
| `void` | no value | — |
| `null` | the null pointer literal | `0`; assignable to any pointer |
| `fn(P...) -> R` | function pointer | machine word: the code address |
| `method(P...) -> R` | bound method pointer, from `&obj.M` | machine word: a pointer to a GC cell `{ code, receiver }` |

Pointers and arrays compose (`int**`, `int*[]`). There are no implicit numeric
conversions; `int` and `bool` are distinct. Arithmetic (`+ - * / %`) is
`int`-only, except `+` which also concatenates when either side is a `string`
(the other side may be `string` or `int`). Comparisons yield `bool`.
`&&`/`||`/`!` are `bool`-only and short-circuit.

`& | ^ ~ << >>` and their compound forms operate on the full 64-bit `int` and
wrap on overflow, matching C. `& | ^ << >>` require `int` operands; `~` is
unary. Shift counts follow C: a count at or above 64 yields 0 (or the sign bit
for `>>`). Division by zero traps rather than being defined.

The six relational operators also accept two `string`s, ordered
lexicographically by unsigned byte value (the ordering C's `strcmp` gives).
Comparing a `string` with an `int` is a type error. `==`/`!=` additionally
accept `null` against any pointer.

## Structs

```
structDecl := "struct" IDENT "{" member* "}"
member     := type IDENT ";"                       // field
            | type IDENT "(" params ")" block      // method
            | type IDENT "=>" expr ";"             // get-only property
            | type IDENT "{" "get" ";" ("set" ";")? "}"  // auto-property
structLit  := "new" IDENT "(" expr ("," expr)* ")"
```

- Structs are value types laid out inline with padding to each field's
  alignment. `new S(a, b, ...)` initializes the real (non-property) fields
  positionally.
- Field access is `s.f`, and also works through a pointer: `p.f` (there is no
  `->`; Z uses C#-style `.` for both). Nested: `l.a.x`.
- Struct assignment (`a = b`) copies the whole value. `var q = p` is a copy.
- Functions/methods returning a struct use the SysV **hidden-pointer** return
  convention (a hidden first parameter holds the result buffer).
- **Methods** live on the struct and get an implicit `this` receiver (a
  hidden first pointer parameter); unqualified field names inside a method
  resolve to `this.<field>`. Emitted symbols are mangled `Struct__method`.
- **Properties**: `int X => expr;` is a get-only computed property;
  `int X { get; set; }` is an auto-property backed by a field of the same
  name (accessed directly, equivalent to a trivial getter/setter).
- Structs are **passed to and returned from functions by pointer** for now
  (declare the parameter as `S*`); by-value passing/returning needs a SysV
  argument classifier and is a later milestone.
- Structs can be array elements: `new S[n]`, `arr[i].f`.

## Enums / sum types & pattern matching

```
enumDecl := "enum" IDENT "{" variant ("," variant)* ","? "}"
variant  := IDENT ("(" type IDENT ("," type IDENT)* ")")?
match    := "match" expr "{" arm ("," arm)* ","? "}"
arm      := IDENT ("(" IDENT ("," IDENT)* ")")? "=>" expr    // a variant
          | "_" "=>" expr                                    // wildcard
```

- A union is a tagged value: an `int` discriminant at offset 0 plus each
  variant's payload. Construct a value with a bare variant name: `Circle(5)`.
- `match` dispatches on the discriminant, binds the active variant's payload
  fields, and runs the matching arm. Only the taken arm is evaluated.
- **The match must be exhaustive** — every variant must be covered (or a `_`
  wildcard is present), or it is a compile error. Adding a variant later makes
  every non-exhaustive `match` fail to compile.

```
enum Shape { Circle(int r), Rect(int w, int h), Point }
string describe(Shape s) => match s {
    Circle(r) => "circle " + r,
    Rect(w, h) => "rect " + w + "x" + h,
    Point      => "point"
};
```

## Ternary

`cond ? a : b` — right-associative, condition must be `bool`, both branches
must have the same type; only the taken branch is evaluated.

## Operator overloading

A struct can define `op_<Name>` methods that overload the binary operators.
The operator method takes the right operand by pointer (structs are passed by
pointer) or by value for scalar parameters like `op_Mul(int k)`:

```
struct Vec {
    int x; int y;
    Vec op_Add(Vec* o) { return new Vec(x + o.x, y + o.y); }
    Vec op_Mul(int k)   { return new Vec(x * k, y * k); }
    bool op_Eq(Vec* o)  { return x == o.x && y == o.y; }
}
var v = a + b;   // -> Vec__op_Add(a, b)
```

## C#-style sugar (desugared in the parser)

- **Expression-bodied members**: `int f(int a) => a * 2;` and methods/properties
  inside structs desugar to `{ return expr; }`.
- **String interpolation**: `$"n = {x}, sq = {x*x}"` desugars to a chain of
  `+` (string concat); embedded `{expr}` holes are re-lexed as sub-expressions
  that share the enclosing scope, so they can reference locals. Escaped
  `\{`/`\}` are literal.
- **Extension methods**: `int Twice(this int n) { ... }` at top level lets you
  write `5.Twice()`; it desugars to `Twice(5)`.

## Declarations

```
decl        := varDecl | constDecl | funcDecl | statement
varDecl     := "var" IDENT "=" expr ";"
             | type IDENT "=" expr ";"
constDecl   := "const" type IDENT "=" expr ";"
type        := ("int" | "bool" | "string") "*"* "[]"*
             | ("fn" | "method") "(" typeList? ")" "->" type
funcDecl    := type IDENT "(" params? ")" block          // definition
             | type IDENT "(" params? ")" ";"            // declaration only
externDecl  := "extern" type IDENT "(" params? ")" ";"
exportFunc  := "export" type IDENT "(" params? ")" block
params      := param ("," param)*
param       := type IDENT
```

`var` infers the type from the initializer. Variables must be initialized.
Redeclaring in the *same* scope is an error; shadowing an outer variable in a
nested block is allowed.

A `const` requires an explicit type, is visible only from its declaration
onward, and occupies no frame slot: each reference is replaced by the literal
during parsing. Its initializer may be a constant expression over other
consts. A top-level `const` may sit next to `main`.

A declaration without a body declares a function whose definition appears
later. `extern` additionally means the body is in C: the symbol is used exactly
as written and nothing is emitted. `export` means the opposite — the body is
here, but the symbol keeps the name as written and is emitted `.globl` so C can
call it. A function may take at most 6 parameters, or 5 when it returns a
struct.

Every function written in Z is emitted under a private `z$`-prefixed symbol, so
it cannot collide with a libc symbol, a runtime helper, or a word the assembler
reserves. `extern` and `export` are how a symbol is named deliberately.

## Pointers and arrays

- `&x` yields `T*` for an lvalue `x`; `*p` reads/writes through a `T*`.
- `new T[n]` allocates a heap array; its value is a pointer to the first
  element. Indexing `a[i]` and pointer arithmetic both scale by the element
  size. `a.length` reads the element count.
- An array does not decay to a pointer: pass `&a[0]` where a `T*` is expected.
- Indexing is unchecked unless the compiler is given `--bounds`, which range-
  checks every array access against the header length and aborts on failure.

## Statements

```
statement := block | ifStmt | whileStmt | forStmt | foreachStmt
           | returnStmt | breakStmt | continueStmt | varDecl | exprStmt
breakStmt    := "break" ";"
continueStmt := "continue" ";"
ifStmt    := "if" "(" expr ")" statement ("else" statement)?
whileStmt := "while" "(" expr ")" statement
forStmt   := "for" "(" (varDecl|exprStmt)? ";" expr? ";" expr? ")" statement
foreachStmt := "foreach" ("var" IDENT | type IDENT) "in" expr ")" statement
returnStmt:= "return" expr? ";"
block     := "{" statement* "}"
exprStmt  := expr ";"
```

`if`/`while`/`for` conditions must be `bool`. `foreach` desugars to an
index-based `while` loop over the collection. `++`/`--` desugar to
`x = x ± 1` (the value is the *new* value).

`break` leaves the innermost enclosing loop and `continue` starts its next
iteration; in a `for`, `continue` jumps to the step rather than the top of the
body. Both are a compile error outside a loop.

## Expressions

Precedence, loosest to tightest:

1. assignment `= += -= *= /= %= &= |= ^= <<= >>=` (right-associative; the left
   side must be assignable)
2. `||`
3. `&&`
4. `== !=`
5. `< <= > >=`
6. `|`
7. `^`
8. `&`
9. `+ -`
10. `* / %`
11. `<< >>`
12. unary `- ! & * ~`
13. primary: literals, `new T[n]`, `(` expr `)`, variable, `print(expr)`, a
    built-in (`abs min max clamp sqrt sin cos`), a user function, postfix
    `[i]`, `.length`, `++`, `--`

`&f` on a function name yields a `fn` value typed by that function's signature.
Calling a `fn` value checks the argument count and types; `==`/`!=` compare
identity, and the ordering operators are rejected. A function pointer is not a
closure -- it captures nothing, and the language has no nested functions.

`&obj.M` on a class yields a `method` value: a pointer to a garbage-collected
cell holding the code address and the receiver, so the receiver is traced and
stays alive as long as the pointer. The call spends `rdi` on the receiver and
starts the declared arguments at `rsi`. A virtual method is bound through the
object's vtable, so it dispatches on the runtime type. A struct receiver cannot
be bound (its methods take the receiver by value), and a `method` value may not
return a struct (the receiver occupies the register a struct result needs).

The built-ins are `abs(x)`, `min(a,b)`, `max(a,b)`, `clamp(x,lo,hi)` and
`sqrt(n)`, all on `int` and returning `int`. `sqrt` is an exact integer root.
`sin(a)`/`cos(a)` are integer-only — there are no floats — with a full turn of
`1 << 30` units and Q30 results, so `1.0` is `1 << 30`; angles wrap and the
result is exactly periodic. A user function of the same name shadows a
built-in.

## Functions & entry point

- A program is a sequence of top-level declarations and statements.
- `import "path.z";` splices the named file's tokens in ahead of the importing
  file's, so its top-level declarations are in scope with no namespace. The path
  is relative to the importing file; each file is expanded once however many
  times it is named; a cycle is an error. Every token keeps the file it was lexed
  from, so diagnostics quote the file the error is actually in.
- If a function named `main` is defined, it is the entry point; combining it
  with top-level statements is an error.
- Otherwise, top-level statements are wrapped in a synthesized `main` (like
  C# top-level programs).
- `print` is a builtin: `print(int)`, `print(bool)`, `print(string)`.
- See **Declarations** for `extern` (implemented in C) and `export` (defined in
  Z, callable from C), and for the `z$` symbol namespace.

## Interoperating with C

Both directions are declared in the Z source, and the linker is invoked with
whatever extra arguments follow the source file.

```
extern int c_add(int a, int b);          // body in C
export int z_triple(int v) { ... }       // body here, callable from C
```

```
./z run main.z host.c        # compile and link a C file
./z build main.z -o main -lm # pass library flags through
```

Type mapping: Z `int` ↔ C `long` (both 64-bit), `bool` ↔ an `int` that is 0 or
1, `string` ↔ `const char *`, aggregates ↔ a pointer to them. A struct cannot
be returned by value across the boundary: Z returns every aggregate through a
hidden result pointer, whereas the C ABI returns aggregates of 16 bytes or
fewer in registers. Returning a struct from Z to Z is fine, since both sides
agree there.

## Generics (monomorphization)

Functions may take type parameters and are compiled by monomorphization: each
distinct set of concrete type arguments produces a specialized, natively
compiled copy (there is no runtime generic machinery, no boxing, no vtables).

```csharp
T max<T>(T a, T b) { if (a > b) { return a; } return b; }   // generic
U pick<T, U>(T a, U b) { return b; }                        // two parameters
int total<T>(T[] xs) { var s = 0; foreach (v in xs) { s = s + v; } return s; }

int main() {
    print(max(3, 7));       // 7
    print(pick(3, 9));     // 9
    var a = new int[3]; a[0]=1; a[1]=2; a[2]=3;
    print(total(a));       // 6
}
```

- Type arguments are **inferred** from the call site; the declaration must appear
  before its uses.
- The return type and body may reference the type parameters (`T max<T>(...)`).
- Parameter types may be `T`, `T[]`, or `T*`; these drive inference.
- A generic function that is never called is never emitted.
- The template body is validated with type-parameter placeholders, then each
  instantiation is re-type-checked against the concrete types (so a bad
  instantiation is a compile error).

## Classes, inheritance & virtual dispatch

A `class` is a heap-allocated reference type with a vtable, enabling dynamic
(dispatch) polymorphism. Structs are value types; classes are reference types.

```csharp
class Shape {
    virtual int Area() { return 0; }          // virtual -> dispatched dynamically
    virtual string Name() { return "shape"; }
}
class Square : Shape {
    int side;
    override int Area() { return this.side * this.side; }  // override a base virtual
    override string Name() { return "square"; }
}
```

- Objects are allocated with `new C(args)`, which returns a `C*`. Each object
  stores a vtable pointer as its first word; `this.field` accesses instance
  fields.
- A **constructor** is a method named like the class, `C(params) { ... }`. It
  runs on `new C(args)`; `base(args)` calls the base-class constructor.
- `virtual` introduces a dispatch slot; `override` replaces a base virtual's
  implementation in the same slot.
- Calling a virtual method dispatches through the receiver's vtable, so the
  *actual* runtime class decides the implementation — even through a base-typed
  reference. A derived class pointer may be assigned to a base class pointer
  (upcast); an array of base pointers (`new Shape*[n]`) gives a classic
  polymorphic collection.
- Non-virtual methods (including inherited ones) are called directly.

## Errors (compile time)

The type checker reports, with source spans and carets:

- undefined variable / undefined function
- cannot assign `B` to `A`
- binary operator applied to the wrong types
- calling a function with the wrong arity or argument type
- `if`/`while`/`for` condition is not `bool`
- `return` type mismatch
- cannot index a non-array/pointer; cannot dereference a non-pointer; cannot
  take the address of a temporary
- unknown type, redeclaration in the same scope, missing `;`, unterminated
  string, unexpected character

## Runtime & GC

A small C runtime (embedded in the compiler, linked into every program) provides
`z_newarray` (heap arrays with a length header), `z_concat`, and
`z_itoa`. All heap allocation goes through a **conservative mark-sweep
garbage collector**: it scans the C stack and spilled registers for words that
point into the managed heap, marks reachable objects transitively, and frees
the rest. Collection runs automatically when the heap grows past a threshold
(so the compiler needs no shadow-stack bookkeeping). Value types (structs) live
in the stack frame and are unaffected by GC.

## ABI

Code targets x86-64 Linux, System V AMD64 ABI. Integers/pointers/array-ptrs are
passed in `rdi, rsi, rdx, rcx, r8, r9`; return values in `rax`. The compiler
reserves `rbp` as the frame pointer and keeps the stack 16-byte aligned at
every call.

Two consequences of the 6-register limit are enforced rather than silently
miscompiled: a function may not take more than 6 parameters, and an
`add`/`sub`/`imul`/`cmp` against a constant too wide for a sign-extended
`imm32` is routed through a register.

Locals live at `[rbp-8]` and below. When callee-saved registers are pushed for
register-allocated locals, `rbp` is rebased below them (`lea rbp, [rbp - 8*n]`)
so the two regions cannot overlap, and the epilogue's pops line up again
against the rebased frame.
