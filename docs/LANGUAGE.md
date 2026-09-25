# Z — language specification

This is the M0–M6a subset that is implemented and tested. It grows each
milestone; the grammar below is the source of truth for what the compiler
accepts today.

## Lexical

- Comments: `// line`, `/* block */`.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
- Integer literals: decimal digits (64-bit signed `int`).
- String literals: `"..."` with escapes `\n \t \r \0 \\ \"`.
- Keywords: `int bool string void var new struct enum match this if else while for foreach in return true false`.
- Operators: `+ - * / %`, `== != < <= > >=`, `&& || ! &`, `= += -= *= /= %=`,
  `++ --`, `( ) { } [ ] ; , .`.

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

Pointers and arrays compose (`int**`, `int*[]`). There are no implicit numeric
conversions; `int` and `bool` are distinct. Arithmetic (`+ - * / %`) is
`int`-only, except `+` which also concatenates when either side is a `string`
(the other side may be `string` or `int`). Comparisons yield `bool`.
`&&`/`||`/`!` are `bool`-only and short-circuit.

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
decl      := varDecl | funcDecl | statement
varDecl   := "var" IDENT "=" expr ";"
           | type IDENT "=" expr ";"
type      := ("int" | "bool" | "string") "*"* "[]"*
funcDecl  := type IDENT "(" params? ")" block
params    := param ("," param)*
param     := type IDENT
```

`var` infers the type from the initializer. Variables must be initialized.
Redeclaring in the *same* scope is an error; shadowing an outer variable in a
nested block is allowed.

## Pointers and arrays

- `&x` yields `T*` for an lvalue `x`; `*p` reads/writes through a `T*`.
- `new T[n]` allocates a heap array; its value is a pointer to the first
  element. Indexing `a[i]` and pointer arithmetic both scale by the element
  size. `a.length` reads the element count.

## Statements

```
statement := block | ifStmt | whileStmt | forStmt | foreachStmt
           | returnStmt | varDecl | exprStmt
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

## Expressions

Precedence, loosest to tightest:

1. assignment `= += -= *= /= %=` (right-associative; left side must be a variable)
2. `||`
3. `&&`
4. `== !=`
5. `< <= > >=`
6. `+ -`
7. `* / %`
8. unary `- ! & *`
9. primary: literals, `new T[n]`, `(` expr `)`, variable, `print(expr)`, `userfn(args...)`, postfix `[i]`, `.length`, `++`, `--`

## Functions & entry point

- A program is a sequence of top-level declarations and statements.
- If a function named `main` is defined, it is the entry point; combining it
  with top-level statements is an error.
- Otherwise, top-level statements are wrapped in a synthesized `main` (like
  C# top-level programs).
- `print` is a builtin: `print(int)`, `print(bool)`, `print(string)`.

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
`vela_newarray` (heap arrays with a length header), `vela_concat`, and
`vela_itoa`. All heap allocation goes through a **conservative mark-sweep
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
