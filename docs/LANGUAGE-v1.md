# Z — language specification (v1)

**This is the v1 specification, kept as a record.** The compiler in this repository
implements v1, but v1 has since changed underneath this document: inheritance and
`base(...)` were cut in v2 slice 2 and the collector was replaced by destructors
in slice 3, and neither is reflected below. Where this document describes
inheritance, `override`, `base(...)` or the garbage collector, it is describing
the language as it was, not as the compiler now accepts it.

For the current behaviour see [LANGUAGE.md](LANGUAGE.md), and for a walkthrough
see [TUTORIAL.md](TUTORIAL.md). Sections here that the compiler has not changed
are still accurate and are deliberately not duplicated there — that is why the
two documents sit side by side.

The grammar below was, at the time of writing, the source of truth for what the
compiler accepted.

## Lexical

- Comments: `// line`, `/* block */`.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
- Integer literals: decimal digits, or `0x`/`0o`/`0b` for hex/octal/binary
  (64-bit signed `int`). `_` may separate digits.
- Float literals: digits, a `.` and more digits, and an optional `e`/`E`
  exponent with a sign -- `1.5`, `0.5`, `1e3`, `2E-2`, `1.5e2`. A `.` begins a
  fraction only when a **digit** follows it, which is what leaves `21.Twice()` a
  member access rather than the number 21 followed by a stray dot. An exponent
  needs no fraction, so `1e3` is a float. A float literal too large to represent
  saturates rather than being an error: `1e400` is a very large number, not a
  mistake.
- String literals: `"..."` with the escapes listed under Lexical below.
- Keywords: `int bool string void var const new struct enum class match this if
  else while for foreach in return break continue true false null extern export
  virtual fn method import`.
- Operators: `+ - * / %`, `== != < <= > >=`, `&& || !`, `& | ^ ~ << >>`,
  `= += -= *= /= %= &= |= ^= <<= >>=`, `++ --`, `( ) { } [ ] ; , .`.
- Integer literals are decimal by default, or hexadecimal (`0xff`), octal
  (`0o755`) or binary (`0b1010_0110`) by prefix. `_` is a digit separator and
  carries no meaning: `1_000_000` is a million. A literal too large for `int` is
  an error rather than a silent wrap, and digits running straight into letters
  (`123abc`) is an error rather than two tokens.
- String escapes: `\n \t \r \0 \\ \"`, `\xNN` for a byte, and `\uXXXX` for a
  code point, encoded as UTF-8. An unrecognized escape is an error.
- `s.length` / `len(s)` is the length in **bytes**. `s[i]` is the byte at `i` as
  an `int` in `0..255`, and `s[a..b]` is the half-open byte range, with either
  end optional and both clamped. A `string` is a sequence of bytes and Z has no
  character type, so a multi-byte UTF-8 sequence is several `int`s.

## Types

| Type | Meaning | Representation |
|------|---------|----------------|
| `int` | 64-bit signed integer | machine word in `rax` |
| `bool` | `true` / `false` | 0 or 1 |
| `float` | IEEE-754 binary64 | 8 bytes; C's `double` |
| `string` | immutable bytes with a length | pointer to the bytes; `{ len, cap }` header at `ptr[-16]` |
| `T*` | pointer to `T` | machine word |
| `T[]` | array of `T` | pointer to first element; length stored at `ptr[-8]` |
| `struct S` | user-defined value type | inline in the frame; fields at byte offsets |
| `void` | no value | — |
| `null` | the null pointer literal | `0`; assignable to any pointer |
| `fn(P...) -> R` | function pointer | machine word: the code address |
| `method(P...) -> R` | bound method pointer, from `&obj.M` | machine word: a pointer to a GC cell `{ code, receiver }` |

Pointers and arrays compose (`int**`, `int*[]`). Arithmetic (`+ - * /`) works on
`int` and on `float`; `%` is `int`-only, since there is no remainder for a
non-integer. `+` also concatenates when either side is a `string` (the other side
may be `string`, `int`, `bool` or `float`). Comparisons yield `bool`.
`&&`/`||`/`!` are `bool`-only and short-circuit; a `float` is not falsey, not
even `0.0`.

### The one conversion Z performs on its own

`int` widens to `float` implicitly, and in no other direction. Every `int` is
exactly representable as a binary64, so the conversion cannot lose anything,
which is what makes it safe to do without being asked; the result of an
operation with a `float` operand is a `float`.

The reverse is never implicit. `(int)f` truncates toward zero -- it does not
round, and does not clamp -- and assigning a `float` to an `int` is a compile
error rather than a silent truncation. `type_widens_to` in the type module is the
single place that decision is made.

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

## `Result<T,E>` and `?`

A call that can fail returns a `Result`: either an `Ok` carrying a `T`, or an
`Err` carrying an `E`.

```
Result<int, string> parse(string s) {
    if (s == "42") { return Ok(42); }
    return Err("not a number: " + s);
}
```

`Result` is a two-variant union, so everything about enums applies to it: `match`
handles it, the exhaustiveness check applies, and the payload is bound by name.

```
match parse(s) { Ok(v) => v, Err(e) => 0 }
```

Both sides must be one machine word — `int`, `bool`, `float`, `string`, or a
pointer — which keeps every `Result` the same sixteen bytes: a tag, then one
payload word. A struct by value does not fit, and is a diagnostic rather than a
truncation; a pointer to one does.

`Ok(...)` and `Err(...)` cannot be written on their own, because neither says
what the *other* type is. They take it from where the value is going: the
declared type of a variable, the enclosing function's return type, or a
parameter's type. `var r = Ok(3);` on its own is an error for that reason.

**`?` propagates the error.** On a `Result` inside a function that returns a
`Result` with the same `E`, `expr?` yields the `Ok` payload, and returns the
error from the function if the value turned out to be an `Err`:

```
Result<bool, string> positive(string s) {
    var n = parse(s)?;     // an Err here returns from this function
    return Ok(n > 0);
}
```

Note that the two `Result`s have different `T` and the same `E`: that is the
point. `?` moves the error along and hands back the payload, so a chain of calls
that can each fail reads as straight-line code, and the caller who cares deals
with the error once at the end.

`?` shares a token with the ternary operator, and the type decides which is
which: a `Result` is never a valid condition, so a `?` after one is always
propagation and a `?` after anything else is always the ternary.

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
decl        := varDecl | constDecl | funcDecl | nestedFunc | statement
varDecl     := "var" IDENT "=" expr ";"
             | type IDENT "=" expr ";"
constDecl   := "const" type IDENT "=" expr ";"
nestedFunc  := type IDENT "(" params? ")" block          // inside a function
type        := ("int" | "bool" | "string" | "float") "*"* "[]"*
             | "Result" "<" type "," type ">"
             | "interface" IDENT "{" ifaceMember* "}"
             | ("fn" | "method" | "closure") "(" typeList? ")" "->" type
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

The other declaration form names the type instead of inferring it, and the type
may be spelled by name — a struct, a union, an enum, or a generic type
parameter — not only as a keyword. Any number of `*` and `[]` may follow.

```
struct P { int x; int y; }

P r = new P(3, 4);      // a struct local
P* q = &r;              // a pointer to one
P copy = r;             // copies by value: copy.x = 1 leaves r.x alone
P[] rows = new P[2];    // an array of them
T r = a;                // a type parameter, inside a generic function
```

A statement that begins with a type is a declaration, not an expression, so a
name that is not a type in that position is reported as one — `foo bar = 1;`
says `unknown type 'foo'` rather than treating `foo` as a value.

A `const` requires an explicit type, is visible only from its declaration
onward, and occupies no frame slot: each reference is replaced by the literal
during parsing. Its initializer may be a constant expression over other
consts. A top-level `const` may sit next to `main`.

A declaration without a body declares a function whose definition appears
later. `extern` additionally means the body is in C: the symbol is used exactly
as written and nothing is emitted. `export` means the opposite — the body is
here, but the symbol keeps the name as written and is emitted `.globl` so C can
call it. A function may take at most 16 parameters; past the argument
registers the surplus is passed on the stack. A struct or union return spends a
parameter slot on the hidden result pointer, so such a function is capped at 15.
A method's list is one shorter again, at 15, because the receiver is a parameter
too — 14 when the method returns an aggregate. The caps count the hidden
parameters deliberately: the argument-setup tables hold one entry per parameter,
and a function that exceeded them wrote past the end of the table rather than
failing.

Every function written in Z is emitted under a private `z$`-prefixed symbol, so
it cannot collide with a libc symbol, a runtime helper, or a word the assembler
reserves. `extern` and `export` are how a symbol is named deliberately.

## Closures

A lambda is written with `=>`, and is an expression, so it can appear anywhere
an expression can:

```
lambda      := "(" params? ")" "=>" (block | expr)
```

```
var add = (int a, int b) => a + b;      // an int-returning closure
print(add(2, 3));                       // 5
var counter = () => { n = n + 1; return n; };
```

The body is either a single expression, whose value is the result, or a block,
which must `return` a value on every path. A lambda with an empty parameter list
still needs its parentheses, so `() => 1` is a closure and `x => x` is not.

A function whose result is a lambda must say so: the type of a closure is
`closure`, spelled with the same parameter and result syntax as `fn`.

```
closure(int) -> int makeAdder(int n) {
    return (int x) => x + n;
}
var plus10 = makeAdder(10);
print(plus10(5));   // 15
```

Assigning a lambda to an `int` is an error, and so is declaring a function that
returns a lambda as `fn`. `fn` and `closure` are deliberately different types
because they are called differently — see **ABI** — and the compiler will not
paper over that by letting one stand in for the other.

**Captures.** A lambda may read any variable that is in scope where it is
written, including a local of the enclosing function, and the value is copied
into the closure when the lambda is created. A lambda that outlives the frame it
was written in therefore keeps working:

```
closure() -> int counter() {
    var n = 0;
    return () => { n = n + 1; return n; };
}
var c = counter();
print(c());   // 1
print(c());   // 2
print(c());   // 3
```

Writing to a captured variable writes to the closure's own copy, shared by every
closure made from the same `n`; the enclosing frame is not changed. A captured
variable is boxed, so every closure over the same variable sees each write.

Nested data is shared rather than copied: capturing a struct or a class value
captures a reference to it, so a lambda can read and write its fields.

```
struct P { int x; int y; }
var p = new P(3, 4);
var getX = () => p.x;
print(getX());   // 3
```

A lambda may not be written inside another lambda; a closure whose body defines
another closure is a compile-time error. There is no limit on how many lambdas
one function may define.

## Nested functions

A function may also be declared inside another function, written where a
statement goes:

```
int outer() {
    int helper(int n) { return n + 1; }
    int twice(int n) { return n * 2; }
    return helper(twice(5));
}
```

Like a lambda it is hoisted out to the top level and emitted under a symbol built
from the enclosing function's id, so two functions that each declare `helper` do
not collide on one symbol. The name is visible from its declaration to the end of
the enclosing function, and — because the signature is filed as the declaration
is read — a nested function may call itself. Declare it before calling it: there
is no pre-scan of function bodies, so a sibling declared *later* is not yet
known.

```
int main() {
    int fact(int n) {
        if (n < 2) { return 1; }
        return n * fact(n - 1);
    }
    print(fact(5));   // 120
}
```

**A nested function does not capture.** It has a frame of its own, and frame
slots are numbered per function, so naming a variable declared beside it would
read whatever its own frame happened to hold. That is a diagnostic rather than a
silent wrong answer:

```
var g = 5;
int f() { return g; }    // error: undefined variable 'g'
```

The same rule stops a top-level function reading a top-level variable, for the
same reason. Where you want to capture, use a lambda, which does.

`extern` and `export` name a symbol deliberately, so neither is allowed on a
nested function; nor is a body-less declaration, since both mean "defined
somewhere else" and a nested function has nowhere else to be.

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
index-based `while` loop over the collection, so the collection must be an
*array*: a pointer has no length, and there would be no bound to iterate to.
`++`/`--` desugar to `x = x ± 1` (the value is the *new* value).

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
closure -- it captures nothing, where a lambda captures the declarations its
body names.

`&obj.M` on a class yields a `method` value: a pointer to a garbage-collected
cell holding the code address and the receiver, so the receiver is traced and
stays alive as long as the pointer. The call spends `rdi` on the receiver and
starts the declared arguments at `rsi`. A virtual method is bound through the
object's vtable, so it dispatches on the runtime type. A struct receiver cannot
be bound (its methods take the receiver by value), and a `method` value may not
return a struct (the receiver occupies the register a struct result needs).

The **intrinsics** are `abs(x)`, `min(a,b)`, `max(a,b)`, `clamp(x,lo,hi)`,
`sqrt(n)`, `sin(a)` and `cos(a)`, all on `int` and returning `int`. These are
*not* the floating-point functions of the same name: `sqrt` is an exact integer
root, and `sin`/`cos` work in fixed point with a full turn of `1 << 30` units and
Q30 results, so `1.0` is `1 << 30`; angles wrap and the result is exactly
periodic. Z has no `float` overloads of them.

The **standard library** is available without declaring anything:

| | |
|---|---|
| `len(s)` | byte length; `0` for `null` |
| `sub(s, start, count)` | substring; clamps at both ends, and a negative `start` counts back from the end |
| `index_of(s, needle)` | byte offset, or `-1` |
| `contains(s, needle)` | `bool` |
| `starts_with(s, p)` / `ends_with(s, p)` | `bool`; an empty needle matches |
| `char_at(s, i)` | the byte at `i` as `0..255`, or `-1` past the end |
| `trim(s)` | strips ASCII whitespace from both ends |
| `upper(s)` / `lower(s)` | ASCII case mapping; bytes above 127 are left alone |
| `replace(s, from, to)` | every occurrence; an empty `from` changes nothing |
| `repeat(s, n)` | `s` repeated `n` times |
| `split(s, sep)` | a `string[]`; an empty `sep` splits into characters, a trailing separator produces no empty piece, and an empty `s` yields one empty piece |
| `pow(b, e)` | integer exponentiation; a negative exponent gives `0` |
| `gcd(a, b)` / `lcm(a, b)` | sign-insensitive; `lcm` of anything with `0` is `0` |

Every one of these is checked like an ordinary call — the argument types are
verified and the result carries a type — so `len(s) + 1` compiles and `len(3)`
does not. A `null` string reads as the empty string rather than crashing.
Lengths are byte counts, not character counts: Z has no character type, so in
UTF-8 one character may be several bytes.

`exp`, `log`, `tan` and the other transcendentals are absent. They are worth
having only for a `float`, and there is nothing to call them on yet.

A user function or extension method of the same name shadows any built-in, which
is how a program defines its own `len`.

## Collections

`Vec<T>`, `Map<K,V>` and `Set<T>` are declared in `lib/collections.z`, in Z, and
spliced ahead of every program the same way the rest of the standard library is
— so a bug in one is a bug any program using it would have found, rather than
something only the C side can reach. `--no-std` compiles without them.

**All three are reference types.** Each is a `class`, so a container is one
pointer and assignment shares it: `var b = a` gives two names for one vector,
which is what anyone reaching for a growable buffer expects, and what makes
`Vec<Vec<T>>` behave. A `struct` holding a pointer would be the same size and
would alias on assignment, which is the trap a language with value-semantics
structs invites badly. The language already draws the line (structs copy,
classes share), so the containers follow it rather than inventing a third answer.

Because a class type and its pointer are different types, a field or parameter
of class type must say which it wants:

```
class Set<T> {
    Map<T,bool>* inner;      // a pointer, so a copy shares one table
    Set() { inner = new Map<T,bool>(); }
}
```

### `Vec<T>`

| `push(v)` / `push_back(v)` | Append one. Amortized O(1); the buffer doubles. |
| `pushAll(other)` | Append every element of another. |
| `pop()` / `pop_back()` | Remove and return the last. |
| `empty()` / `isEmpty()` | `size() == 0`. |
| `at(i)` | Bounds-checked element; a message and a non-zero exit out of range, not a clamp. |
| `size()` / `capacity()` | Element count / allocated elements. |
| `insertAt(i, v)` / `removeAt(i)` | Middle insert and removal; removal shifts, which is O(n) and always will be. |
| `insert`/`reserve(n)` | Grow to hold `n` without changing the length. |
| `clear()` / `release()` | Length to zero, buffer kept / buffer freed. |
| `drop(n)` | Remove the last `n` elements. |
| `reversed()` | A new vector, back to front. |
| `slice(from, to)` / `sliceFrom` / `sliceTo` / `take(n)` | A new vector over a half-open range, each end clamped. This is the "take part of an array without copying it" the M10 notes asked for. |
| `join(sep)` / `toString()` | The elements as text, separated by `sep` / by `", "`. |

`push_back`, `pop_back` and `empty` are the C++ names. Each is a one-line alias
for the method beside it rather than a second body, so the two spellings cannot
drift apart — and both are kept, because `push` is shorter and a reader who
knows either name should be able to use the other.

`foreach` over a `Vec<T>` visits the elements. There is no `Iterator<T>`: an
iterator is an interface cell and an indirect call per element, on every loop in
every program, to buy a generality no container here needs. Each container is
iterable through a `foreach` that knows its own representation, which is a plain
index loop in the container and nothing at the call site.

### `for (auto x : v)` and `for (auto& x : v)`

```
rangeFor := "for" "(" ("var"|"auto"|type) "&"? IDENT ":" expr ")" statement
```

The range-based `for` is C++'s spelling and desugars to the same index loop
`foreach` produces, so `break` and `continue` behave identically and every
optimizer that understands a `for` already understands this.

It works over a fixed array, a `string` (as its bytes, like an array), and a
class with an `at(i)` method — which is what a `Vec<T>` is. The element type
comes from the container: a written type is checked against it, so
`for (int x : v)` over a `Vec<string>` is a diagnostic rather than a silent
misreading.

**`&` makes the loop variable a pointer to the element**, and the body reaches
the element through `*`:

```
var v = new Vec<int>();
v.push(1);
for (auto& x : v) { *x = 99; }    // v holds 99 afterwards
print(*(v.data[0]));
```

That is one dereference away from C++, where `auto&` is a reference and the
`*` is not written. The difference is honest rather than a missing feature: Z
has no reference type, so a borrow is spelled as a pointer to the element. What
the two have in common is the part that matters — the write lands in the
container, and without the `&` each element is copied into a fresh local
instead.

A class that has no `at` cannot be ranged over, and the message says so. A
`Set<T>` is one: it is unordered, and "the element at i" is not a thing it can
answer. `values()` hands out its elements as a `Vec`, which is iterable in turn.

`&obj.field` is the address of the field, and `&obj.M` is a method pointer. The
two share a token, so the parser decides by looking the name up as a field
first; anything that is a field is addressed, and only a name that is not a
field goes looking for a method.

### `Map<K,V>`

An open-addressed table with linear probing: one entry per occupied slot, and
the probe walks from the hash until the key matches or the run ends at a
never-used slot. A *removed* slot does not end the run, which is the only reason
removal is not simply emptying. Rehashing doubles the slot count.

| | |
|---|---|
| `set(k, v)` | Insert, or overwrite an existing key in place. |
| `get(k, fallback)` | The value, or `fallback` if absent. Not a `Result`: absent is the ordinary state of a lookup, and making every read pay for a match would be the wrong default. |
| `getOr(k, make)` | The value, inserting `make` first if absent. The shape of a cache or a memo table; `make` is an expression, so it is only evaluated when there is something to store. |
| `remove(k)` | Remove, returning whether it was there. |
| `keysOf()` / `values()` | The keys, or the values, in slot order. Named `keysOf` because `keys` is the field holding the parallel key array. |
| `size()` / `isEmpty()` / `empty()` / `clear()` / `reserve(n)` | As for `Vec`. |
| `slots()` / `load()` | Slot count, and a summary of the table's load. |
| `join(sep)` / `toString()` | `k=v` pairs, separated by `sep` / by `", "`. |

### `Set<T>`

A `Map<T,bool>`, and it says so rather than hiding it. It reaches `has` and
`set` on the table underneath, so the set needs no method of its own for either.
A set that can be asked for a value is a set that will be, so the pair of
questions a set answers is `has` and nothing else: `has`, `add`, `addIfNew`,
`remove`, `clear`, `reserve`, `size`, `isEmpty`, `empty`, `values`,
`containsAll`, `isEqualTo`, `toString`.

### `any`

A parameter kind, not a type a program can declare. There is no way to write it,
name it, or declare a variable of it, and it appears only in a built-in
signature.

A generic function cannot ask what it was instantiated with in a way its branches
can use. `typeof` answers, but every branch after the test still has to
type-check, and a branch that calls `int_to_string` on a `string` does not — so
the generic `Vec`/`Map` could not format or hash an element at all. The two
questions that genuinely depend on the element type, a value's text form and a
value's hash, are therefore asked of the **code generator** through a parameter
declared `any`. The generator knows the static type at the call site and lowers
the call per type, so `to_text(x)` and `hash_of(x, cap)` mean the right thing for
every key and element type a `Map` or `Set` can be instantiated with, and
neither the standard library nor a user has to declare an interface that `int`
and `string` would then both have to satisfy.

The cost is that `any` is opaque to the type checker: it checks nothing about
its argument, and a mistake in a lowering shows up as a wrong answer rather than
a diagnostic. That is why it is a parameter kind for built-ins only.

An **aggregate** argument (a struct, a union, an array) lowers to its *type
name*, not a rendering of its value: `Vec<Pt>`'s `toString` prints `Pt` for each
element. Printing the fields would need a per-field formatter chosen at compile
time and a description of the layout the runtime does not have, and the useful
case is a container of a type whose fields are not interesting. A container of
`int` or `string` prints its values, because those are what a reader is looking
at.

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
- `print` is a builtin: `print(int)`, `print(bool)`, `print(float)`, `print(string)`.
  A float prints with `%g`, so `1.5` is `1.5`, `100.0` is `100` and `0.1 + 0.2`
  is `0.3` -- six significant digits, trading exact digits for readable ones.
  This is the same formatting `"x = " + 1.5` uses, so the two never disagree.
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

## Interfaces

An interface is a named set of method signatures. A type satisfies it by having
those methods — nothing is declared, because a struct has no vtable of its own to
list them in.

```
interface Shape {
    int Area();
    string Name();
}

struct Square { int side; int Area() { return side * side; } string Name() => "square"; }
struct Rect   { int w; int h; int Area() { return w * h; } string Name() { return "rect"; } }
```

Assigning a value to an interface-typed place converts it, and a call resolves to
the interface's method rather than to anything the value happens to be:

```
Shape a = new Square(5);
print(a.Area());        // 25
print(a.Name());        // square
```

That is what makes unrelated types collectable together — structs, classes and
subclasses of different hierarchies in one array:

```
var shapes = new Shape[3];
shapes[0] = new Square(3);
shapes[1] = new Rect(3, 4);
// ...
for (var i = 0; i < 3; i = i + 1) { print(shapes[i].Name()); }
```

An interface value is a **pointer** to a two-word cell, `{ itab, receiver }`, so
it is one scalar: it passes, stores, returns and compares like a pointer, and
`null` is meaningful. The itab is a static array of code pointers, one per
required method, **in the interface's declaration order** — that order is the
contract, and every implementing type follows it. A method call resolves its name
to an index in that array once, at compile time.

A **struct** is copied to the heap when it becomes an interface value, because
the cell outlives the frame the value was in. A **class** is already a pointer,
so it goes in as it stands; its itab entries are small trampolines that go
through the receiver's vtable. The trampolines used to be what kept a subclass
calling the override rather than the implementation the conversion site named;
with no inheritance there is no override to miss, and they remain because the
vtable is still how a class stored in an interface is dispatched.

A class must therefore declare every method it offers to an interface `virtual`
— a non-virtual method has no vtable slot to dispatch through. A
struct has no such requirement, since its methods are reached directly.

The method has to match the signature, not just the name, and a diagnostic says
which of the two went wrong: `it has no method 'Name'` versus `'Scale' does not
match the signature the interface requires`.

Like every other callable in Z, an interface value that is `null` must not be
called.

## Standard library

The string and integer helpers are available without declaring anything. Each is
checked like an ordinary call, so `len(s) + 1` compiles and `len(3)` does not.
A user function of the same name shadows any built-in.

| | |
|---|---|
| `len(s)` | byte length; `0` for `null` |
| `sub(s, start, count)` | substring; clamps at both ends, and a negative `start` counts back from the end |
| `index_of(s, needle)` | byte offset, or `-1` |
| `contains(s, needle)` | `bool` |
| `starts_with(s, p)` / `ends_with(s, p)` | `bool`; an empty needle matches |
| `char_at(s, i)` | the byte at `i` as `0..255`, or `-1` past the end |
| `trim(s)` | strips ASCII whitespace from both ends |
| `upper(s)` / `lower(s)` | ASCII case mapping; bytes above 127 are left alone |
| `replace(s, from, to)` | every occurrence; an empty `from` changes nothing |
| `repeat(s, n)` | `s` repeated `n` times |
| `split(s, sep)` | a `string[]`; an empty `sep` splits into characters, a trailing separator produces no empty piece, and an empty `s` yields one empty piece |
| `pow(b, e)` | integer exponentiation; a negative exponent gives `0` |
| `gcd(a, b)` / `lcm(a, b)` | sign-insensitive; `lcm` of anything with `0` is `0` |

A `null` string reads as the empty string rather than crashing. Lengths are byte
counts, not character counts: Z has no character type, so in UTF-8 one character
may be several bytes.

`exp`, `log`, `tan` and the other transcendentals are absent. They are worth
having only for a `float`, and there is nothing to call them on yet.

**[v2]** `format(fmt, args...)` replaces `+` concatenation and `$"..."`
interpolation, and these helpers move to methods on `string` so that the
receiver is the string rather than the first argument. See
[LANGUAGE.md](LANGUAGE.md#expressions).

## Classes, inheritance & virtual dispatch

A `class` is a heap-allocated reference type with a vtable, enabling dynamic
(dispatch) polymorphism. Structs are value types; classes are reference types.

```csharp
class Square {
    int side;
    Square(int s) { this.side = s; }
    virtual int Area() { return this.side * this.side; }
    virtual string Name() { return "square"; }
}
```

- Objects are allocated with `new C(args)`, which returns a `C*`. Each object
  stores a vtable pointer as its first word; `this.field` accesses instance
  fields.
- A **constructor** is a method named like the class, `C(params) { ... }`. It
  runs on `new C(args)`.
- `virtual` introduces a dispatch slot, one per virtual method in declaration
  order. Calling one dispatches through the receiver's vtable; a non-virtual
  method is called directly.
- A class satisfies an `interface` by having its methods declared `virtual`,
  and an interface value dispatches through the itab. A class with no interface
  and no base has nothing to dispatch *for*, so `virtual` on a lone class only
  costs a slot.

**There is no inheritance.** `class B : A`, `override` and `base(args)` are
diagnostics, each saying what to do instead. What a hierarchy was for — putting
related types in one collection — is what `interface` is for, and it does it
without a base type to upcast to, a base constructor that can be forgotten, or a
vtable whose layout has to be inherited. See [Interfaces](#interfaces).

The vtable is still here for a reason: it is what a class stored in an
interface dispatches through. A vtable with no subclass is a working vtable.

## Diagnostics

Errors are reported with source spans and carets:

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
- an unknown escape, a literal too large for `int`, digits running into letters

### Warnings

The compiler also warns about code that is legal but probably not what was
meant. Warnings are on by default, never fail a build on their own, and are
controlled by `-w`, `-Werror`, `-Wno-<name>` and `-W<name>`:

| | |
|---|---|
| `unused-local` | a local or parameter is declared and never referenced |
| `shadowed-local` | a local redeclares one from an enclosing scope |
| `unreachable` | a statement follows one that always leaves the block |

A suppressed warning costs nothing to run: the check that produces it does not
execute at all. `-Werror` turns every enabled warning into an error.

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
passed in `rdi, rsi, rdx, rcx, r8, r9`; return values in `rax`. Floats go in
`xmm0`-`xmm7` and come back in `xmm0`. The two sequences are numbered
independently, so a float does not displace the integers that follow it:
`f(1.0, 2)` passes `1.0` in `xmm0` and `2` in `rsi`, not in `rdx`. The compiler
reserves `rbp` as the frame pointer and keeps the stack 16-byte aligned at
every call.

A closure value is a pointer to a cell of two words: the function to call, and
the environment it captures. Calling it puts the environment in `rdi` and the
declared arguments from `rsi` up, and the callee's prologue reads them from the
same place. That is a different convention from a plain call, which is why
`fn` and `closure` are separate types: the hidden environment takes `rdi` first,
so a float among the declared arguments can no longer start at `xmm0` and
travels as raw bits in a general-purpose register instead. Bound methods and
constructor calls take a hidden receiver for the same reason and follow the
same rule.

Two consequences of the 6-register limit are enforced rather than silently
miscompiled: a function may not take more than 16 parameters (15 with a struct
return, 15 for a method, 14 for a method returning one), and an
`add`/`sub`/`imul`/`cmp` against a constant too wide for a sign-extended
`imm32` is routed through a register.

Locals live at `[rbp-8]` and below. When callee-saved registers are pushed for
register-allocated locals, `rbp` is rebased below them (`lea rbp, [rbp - 8*n]`)
so the two regions cannot overlap, and the epilogue's pops line up again
against the rebased frame.

## Optimization levels & command line

```
z <run|build|asm> <file.z> [-o output] [-O0..-O3] [--bounds] [-g]
                    [-w] [-Werror] [-Wno-<name>] [linker args...]
```

`[--bounds]` is v1's spelling, written above because the rest of this document is
v1. The checks it asked for are now **on by default**, so the flag to turn them
off is `--no-bounds`; `--bounds` is still accepted as an explicit request for the
default. The driver also accepts `--version` and prints the compiler version.

| | |
|---|---|
| `-O0` | naive: every local in memory, no folding, a real `idiv`. A baseline to measure against, and the level to debug codegen with. |
| `-O1` | the default: constant folding and propagation, function inlining, a liveness-based local register allocator, leaf and immediate operand selection, branch-on-flags conditions, in-place compound assignment, and constant division/modulo strength reduction. |
| `-O2` | adds loop-invariant code motion. |
| `-O3` | adds loop unrolling: a loop with a condition and a small enough body is emitted four times over, testing the condition before each copy. |
| `--bounds` | range-check every array access. Costs a length load and two branches per access, which is why it is off by default. It covers array indices only; a `T*` has no length header to check against. **[changed since]** checks are now on by default and the flag to turn them off is `--no-bounds`; `--bounds` is still accepted as an explicit request for the default. The check has also grown past array indices: it now covers dereferencing a null `T*`, printing `runtime error: null pointer dereference` and exiting 134. |
| `-g` | emit DWARF: a line table, and a symbol table naming every function, its parameters and its frame-resident locals. |

A level only gates passes; it never changes what a program means, and the test
suite runs at all four levels (`make test-all`) precisely because a pass that
only runs at a higher level can miscompile while the default level shows nothing
wrong.

A **counted loop whose body only accumulates loop-invariant amounts** is solved
arithmetically and does not run at all:

```
var sum = 0; var i = 0;
while (i < 20000000) { sum = sum + 82; i = i + 1; }
// becomes: sum = sum + 82 * 20000000
```

That is the limit of what loop-invariant code motion can reach on its own: it
hoists the pieces, and this notices there is nothing left to run. On the
`loopbench` case it takes a 24 ms loop to 4 ms.

The pattern is deliberately narrow, because getting it wrong is a wrong answer
rather than a missed speedup. The step must be a self-update by a nonzero
constant; the condition must compare that variable with `<`, `<=`, `>` or `>=`
against a bound; the body must be a straight-line run of `x = x ± E`, `x += E`
and `x -= E` where `x` is an `int`; no contribution may read the loop variable or
an accumulator; and there must be no call, allocation, `break` or `continue`,
since an early exit means the trip count is not the one computed. The start
must be a constant — from the initializer, or from the nearest preceding write to
the variable — because it is the one number the closed form cannot otherwise
recover. Anything else runs the loop as written.

Unrolling tests the condition before every copy rather than once per pass. The
body therefore runs exactly as many times as it did rolled — the tail is handled
by the same test instead of by a computed iteration count — and what changes is
that the last copy branches back to the top while the others fall through into
the next, so three-quarters of the loop-back branches are gone and consecutive
iterations sit next to each other for the prefetcher. Each copy gets its own
continuation label, so `continue` runs the step of the copy it appears in, and
`break` leaves the whole loop from any copy.

A loop is left rolled when it has no condition (`for(;;)`, whose trip count
nothing bounds), when its body is over 24 statements (the point is to fit the
body in the instruction cache, and a body that does not fit gains nothing from
being copied four times), and at any level below `-O3`.

### Inlining

A call to a function defined in the same file is replaced by the function's
body, from `-O1` up. A body that is a single `return` becomes the expression
itself, so `twice(21)` is `21 * 21`; a longer body is spliced in as statements
and its result is left in a hidden local. A call inside the spliced body is
itself a candidate, so nesting is followed up to four levels deep, which is where
code growth stops paying for the calls it removes.

```
int clamp(int v, int lo, int hi) {
    var out = v;
    if (v < lo) { out = lo; }
    if (v > hi) { out = hi; }
    return out;
}
print(clamp(5, 1, 3));      // the body above, inline, with no call
```

The point is the call overhead: the argument setup, the call and return, and the
frame the callee needed for its own locals. The body's locals get slots in the
*caller's* frame rather than a frame of their own, and a call that was only ever
prologue and epilogue disappears.

Arguments are substituted by copying the caller's expression into each place the
callee used the parameter, so a parameter used three times costs three reads of
the same value. That is only sound for an expression that costs nothing to
repeat, and a call is left alone when an argument might act (`f().add(3)`) or
cost something (`1/den`), rather than being inlined into a program that calls
`f()` three times.

These are declined, each because the pass would have to model something the
inliner does not:

| | |
|---|---|
| recursive functions | inlining one would leave a call to itself, so nothing is saved and the body is copied for nothing. |
| `extern` and `export` functions | the body is not this program's to copy. |
| a body containing a closure, a bound method, or a nested function | the environment is built where the lambda is written and points at that frame's slots; the code can move, the frame it names cannot. |
| struct or union parameters, and functions returning one | an aggregate argument arrives as a pointer, so substituting the expression would substitute the value rather than its address. A struct result needs a hidden return pointer rebuilt, which is the same ABI in reverse. |
| a body over 24 statements | past that the copy costs more in instruction cache than the call it removed. |
| a lambda or nested function as the *caller* | a captured variable is a box created by a declaration outside the closure, so a copy that moved its references would name slots nothing writes. |

Arguments the compiler does not recognize are passed through to the link step,
so C libraries and linker flags work as usual.

### What `-g` describes, and what it does not

The line table is produced by the assembler from `.file`/`.loc` directives
emitted beside the code, so it is exact by construction.

Two things are left out on purpose. A local that the register allocator promoted
to a callee-saved register is not described: a variable that moves between a
register and the stack over its lifetime needs a location list to describe
accurately, and emitting a location that is right only part of the time is worse
than emitting none, so a debugger reports it as optimized out. Parameters are
always spilled to the frame in the prologue, so those are exact.

And a local is typed as a `long`, a `boolean` or a `char *` only. Z has three
scalar types; anything else gets no type attribute at all, which a debugger
shows as an untyped value rather than a confidently wrong one.
