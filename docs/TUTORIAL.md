# Z — Tutorial

Z is a small, statically-typed, C#-flavored language that compiles to native
x86-64 machine code. It has a real type system, value types (structs),
reference types (classes) with inheritance and dynamic dispatch, interfaces,
closures and nested functions, monomorphized generics, pattern matching, a tracing garbage
collector, and a modern optimizing backend — with no runtime dependency beyond libc.

This tutorial walks through the whole language. Every example is runnable and
verified against the compiler in this repo.

---

## 1. Getting started

Build the compiler (needs `cc` and a Unix toolchain):

```sh
make          # produces ./z
make test     # run the language's own test suite
```

Run a program:

```sh
./z run hello.z            # compile to a temp binary and run it
./z build hello.z -o hello # compile to a native ./hello executable
./z asm   hello.z          # print the generated x86-64 assembly
```

Z source files use the `.z` extension.

---

## 2. Hello, world

```csharp
Console.WriteLog("Hello, world!");
```

```
Hello, world!
```

`Console.WriteLog` is a builtin that accepts `int`, `bool`, `float`, or
`string`. Z has no bare `print`: see [Surface names](#surface-names) for
why the name is a dotted one, and why that is a decision rather than an
accident.

---

## 3. Variables and types

Z has three built-in value types — `int` (64-bit signed), `bool`, and
`string` — plus the composite types you'll meet later (structs, classes,
arrays, pointers).

Use `var` to let the type be inferred, or name the type explicitly:

```csharp
var a = 42;          // int
var b = 3.5;         // (not a thing — there is no float type; see the note below)
```

Z has four types. Here are valid declarations:

```csharp
var count = 10;          // int
var ok = true;           // bool
var ratio = 0.5;         // float
var name = "Zeta";       // string
int explicit_int = 5;    // explicit type
bool flag = false;
float explicit = 2.5;
```

`float` is IEEE-754 binary64 -- the same 8-byte format C calls `double`. It is
named `float` here to match C#, not because it is 32 bits.

Every `var` binding must be initialized where it is declared.

The explicit form is not limited to the built-in types — a struct, union, or
generic type parameter works there too, which is how you get a local of a type
the compiler could not have inferred:

```csharp
struct P { int x; int y; }

P r = new P(3, 4);      // a struct local
P* q = &r;              // a pointer to one
P copy = r;             // copies by value
P[] rows = new P[2];    // an array of them
```

`P copy = r;` is a copy: setting `copy.x` leaves `r.x` alone. Use `P* q = &r;`
when you want both names to mean the same thing.

### `const`

A `const` is a named compile-time constant. It needs an explicit type, and it
occupies no stack slot: every reference is replaced by the literal while
parsing, so using one costs nothing at runtime.

```csharp
const int MAX = 100;
const int HALF = MAX / 2;        // constant expressions may use other consts
const int FLAGS = 1 | 2 | 4;
const int MASK = FLAGS & 6;      // 6
const int SHIFTED = 1 << 10;     // 1024

Console.WriteLog(HALF);                     // 50
```

A const must be declared **before** it is used, the same way types must be. That
is different from functions, whose signatures are all known before the body of
anything is compiled, so a function may be called before it is written. Consts
may sit at the top level next to `main`.

---

### Floats

A `float` literal needs either a `.` or an exponent. A `.` only starts a
fraction when a digit follows it, which is why `21.Twice()` below still means
"call the extension method on 21":

```csharp
Console.WriteLog(1.5);      // 1.5
Console.WriteLog(1e3);      // 1000
Console.WriteLog(2E-2);     // 0.02
Console.WriteLog(21.Twice());  // 42  -- the '.' belongs to the member access
```

Arithmetic is `+ - * /`. There is no `%` for a float, because a non-integer has
no remainder, and none of the bitwise operators either.

```csharp
var x = 1.5;
Console.WriteLog(x * 2.0);      // 3
Console.WriteLog(x + 2);        // 3.5   an int operand widens, and the result is a float
Console.WriteLog("v = " + 1.5); // v = 1.5
```

**The one conversion Z does on its own** is `int` to `float`. Every `int` is
exactly representable as a float, so nothing is lost. The other direction is
never implicit, because it does lose something:

```csharp
var n = 0;
n = 2.5;             // error: cannot assign 'float' to 'int' without losing
                     // precision; write '(int)' if that is what you want
Console.WriteLog((int)3.9);     // 3   a cast truncates toward zero; it does not round
Console.WriteLog((int)-3.9);    // -3
Console.WriteLog((int)0.5);     // 0
```

### Floats follow IEEE-754, which surprises people

Division by zero does not trap. It gives an infinity, because that is what
floating-point division is specified to do:

```csharp
float recip(float x) { return 1.0 / x; }
Console.WriteLog(recip(0.0));   // inf
Console.WriteLog(recip(-0.0));  // -inf
```

And a NaN -- "not a number", which you can make with `0.0 / 0.0` -- compares
**false against everything in every direction, including itself**:

```csharp
var nan = 0.0 / 0.0;
Console.WriteLog(nan == nan);   // false   ...it is not equal to itself
Console.WriteLog(nan != nan);   // true    ...so the two are "different"
Console.WriteLog(nan < 1.0);    // false
Console.WriteLog(nan > 1.0);    // false
Console.WriteLog(nan <= 1.0);   // false
Console.WriteLog(nan >= 1.0);   // false
```

The last four are the ones worth internalizing: a comparison against a NaN is
never true, so `if (x != y)` is *not* a safe way to ask whether two floats are
the same value. This is not a quirk of this compiler; it is IEEE-754, and every
other language with floats behaves the same way.

A float also prints with `%g`, so a whole-valued float looks like an integer:
`Console.WriteLog(100.0)` prints `100`, and `0.1 + 0.2` prints `0.3` rather than
`0.30000000000000004`. Six significant digits, trading exact digits for
readable ones.

---

## 4. Operators

Arithmetic, comparison, and logic work as you'd expect (integers are 64-bit):

```csharp
Console.WriteLog(2 + 3);      // 5
Console.WriteLog(7 - 2);      // 5
Console.WriteLog(6 * 7);      // 42
Console.WriteLog(7 / 2);      // 3   (integer division)
Console.WriteLog(7 % 2);      // 1   (remainder)
Console.WriteLog(1 + 2 * 3);  // 7   (* binds tighter than +)
Console.WriteLog(10 / 3);     // 3

Console.WriteLog(3 > 2);      // true
Console.WriteLog(3 == 3);     // true
Console.WriteLog(3 != 3);     // false
Console.WriteLog(true && false);  // false
Console.WriteLog(true || false);  // true
Console.WriteLog(!true);          // false
```

Increment/decrement work on variables:

```csharp
var i = 5;
i++;
Console.WriteLog(i);   // 6
i--;
Console.WriteLog(i);   // 5
```

Bitwise operators work on the full 64-bit value, and wrap on overflow the way
C's do:

```csharp
Console.WriteLog(12 & 10);        // 8    (1100 & 1010)
Console.WriteLog(12 | 10);        // 14   (1100 | 1010)
Console.WriteLog(12 ^ 10);        // 6    (1100 ^ 1010)
Console.WriteLog(~0);             // -1   (all 64 bits set)
Console.WriteLog(1 << 10);        // 1024
Console.WriteLog(1024 >> 3);      // 128
```

The compound forms work too, and are common in flag manipulation:

```csharp
var flags = 0;
flags |= 1;      // set bit 0
flags |= 4;      // set bit 2
Console.WriteLog(flags);    // 5
flags &= 6;      // keep only bits 1 and 2
Console.WriteLog(flags);    // 4
flags ^= 4;      // clear bit 2
Console.WriteLog(flags);    // 0
```

Literals come in every radix, and `_` is a digit separator:

```csharp
Console.WriteLog(0xff);            // 255
Console.WriteLog(0o755);           // 493   (octal)
Console.WriteLog(0b1010_0110);     // 166   (binary, grouped)
Console.WriteLog(1_000_000);       // 1000000
```

**Note on division:** division and modulo by a *constant* are strength-reduced
by the compiler into a multiply–shift (much faster than a hardware divide), so
`x % 1000000007` in a hot loop is cheap.

---

## 5. Built-in functions

A handful of functions are built into the compiler rather than written in Z.
They are ordinary calls, so they read the same way:

```csharp
Console.WriteLog(abs(0 - 7));            // 7
Console.WriteLog(min(3, 9));             // 3
Console.WriteLog(max(3, 9));             // 9
Console.WriteLog(clamp(15, 0, 10));      // 10   (x, low, high)
Console.WriteLog(sqrt(17));              // 4    (integer square root, rounded down)
```

`abs`, `min`, `max` and `clamp` compile down to a couple of instructions each.
`sqrt` calls into the runtime, whose integer root is exact for every input
rather than merely close.

Because Z has no floating point type, `sin` and `cos` are integer-only. **A full
turn is `1 << 30` units** and the result is Q30, so `1.0` is exactly `1 << 30`:

```csharp
const int TURN = 1 << 30;
const int ONE = 1 << 30;         // 1.0 in Q30
Console.WriteLog(cos(0));            // 1073741824   exactly 1.0
Console.WriteLog(sin(TURN / 4));     // 1073741824   a quarter turn: also 1.0
Console.WriteLog(cos(TURN / 2));     // -1073741824  a half turn: exactly -1.0

/* sin of a whole turn is mathematically 0, but the fixed-point result can be
 * off by an ulp or two, so compare with a tolerance rather than == 0. */
Console.WriteLog(abs(sin(TURN)) < 8 ? 1 : 0);   // 1
```

Angles outside one turn wrap, and the result is exactly periodic, so you can
pass a raw counter without normalizing it first. They are computed with an
integer CORDIC — no `libm`, no floating point, and the same answer on every
machine. Accuracy is a little under 7 ulp of Q30 (about 6e-9) over a full turn,
which is far more than a game needs and far better than a lookup table.

A user function of the same name shadows a built-in, so you can define your own
`min` if you prefer.

### Reading input

Three built-ins read a line. `input` prompts and reads, `read_string` reads
without prompting, and `read_line` is the same reader from a descriptor:

```csharp
var name = input("what is your name? ");
var silent = read_string();
Console.WriteLog(len(silent));

var fd = open("/tmp/names.txt", "r");
while (true) {
    var line = read_line(fd);
    if (len(line) == 0) { break; }     // end of file
    Console.WriteLog(line);
}
close(fd);
```

- The prompt is written with **no newline** after it, which is what lets the
  answer appear on the same line, and it is flushed before the read — otherwise a
  piped program would appear to hang with nothing on screen.
- All three stop **at** the line terminator and do not include it.
- `\r` ends a line as much as `\n` does, and a CRLF pair counts **once**. A file
  written on Windows does not come back with an empty line between every pair.
- At end of file they return `""` rather than failing, so a program that runs off
  the end of its input reads empty answers instead of trapping.

One byte is read per syscall, which is the same trade `read_byte` makes: stdio
would buffer the rest of the descriptor somewhere the program cannot see it, and
a later `seek` on that descriptor would be wrong in a way nothing in the output
would show.

### Surface names

Z has no namespaces, and `Console.WriteLog` is not one. It is a **surface name**:
a dotted spelling that the parser resolves to an ordinary global call before
anything else looks at it.

```csharp
Console.WriteLog("hello");   // the same function as the internal name `print`
console.logwrite("hello");   // case does not matter
```

Three things are worth knowing about it.

**Case is ignored, and only here.** `Console.WriteLog`, `console.logwrite` and
`CONSOLE.LOGWRITE` are one name. Z's own identifiers are case-sensitive; this one
is not, because a name in this table is a spelling a house style gets to choose
rather than an identifier the compiler is matching. Folding is ASCII-only, for
the same reason `upper` and `lower` are: a byte above 127 starts a UTF-8
sequence, and folding it alone would corrupt the character it belongs to.

**It only applies to a call.** `Console.WriteLog` followed by `(` is a surface
name. `p.x` is a field, `v.M()` is a method, and both stay exactly what they
were — a dotted name is a member access unless the table claims it *and* it is
being called. A variable of your own named `Console` also wins, because a name in
scope is a name in scope.

**There is no bare `print`.** Writing it is an ordinary undefined-name error,
which means you are free to define your own `print` and have it mean what you
like. A diagnostic about `Console.WriteLog` names `Console.WriteLog`, never the
internal spelling, because a message that named a form no program can write would
be the one thing a diagnostic must not do.

#### A project picks its own dialect

The names above ship with Z. A project overrides them in a `z.surface` file
beside its code, one mapping per line — what the program writes, then `=`, then
what the compiler calls it:

```
# a comment
Console.WriteLog = say
StringBuilder   = TextBuffer
append          = push
```

That program is now written in its own vocabulary, and none of the compiler, the
runtime or the emitted machine code changed:

```csharp
var b = new TextBuffer();
b.push("hello ");
say(b.finish());
```

The compiler looks for `z.surface` starting in the source file's own directory
and walking up, and takes the first one it finds. The nearest wins, so a project
vendored inside another keeps its own dialect. **One** manifest governs the whole
compilation rather than one per file, because `import` splices rather than
isolates: a library you import is read in *your* dialect, which is the same rule
`import` already follows.

```sh
z run main.z                      # uses ./z.surface if the walk finds one
z run main.z --no-surface         # ignore it; the shipped names only
z run main.z --surface=path/to/x  # use this one instead of searching
```

Two rules keep this from surprising anybody, and both are the whole design:

- **A surface name is a fallback, never a claim.** Every lookup tries the name as
  written *first*. If your project declares its own `say`, or its own
  `TextBuffer`, or its own `push`, that is what the code means — the table only
  answers for names that would otherwise be missing. A local variable named after
  a surface name is never renamed out from under the code that reads it.
- **A renamed method is still called by its own name.** `b.push(x)` reaches the
  library's `append` and the emitted symbol is the one `append` was emitted
  under. That is the detail a rename gets wrong first: it type-checks, and then
  the link fails on a symbol nobody defined.

What can be renamed is anything resolved by name — the builtins, the standard
library's types and their methods, and your own globals. What cannot yet be
renamed is the *keywords*: `var`, `foreach`, `match` and the rest are lexed, and
those are the lexer's table rather than a name lookup. That is the remaining half
of this idea.

### The string library

Available without declaring anything, and type-checked like any other call:

```csharp
var s = "  Hello, World  ";
Console.WriteLog($"[{trim(s)}]");            // [Hello, World]
Console.WriteLog(len(s));                    // 16   bytes, and 0 for null
Console.WriteLog(upper(trim(s)));            // HELLO, WORLD
Console.WriteLog(char_at("abc", 1));         // 98   the byte, or -1 past the end
Console.WriteLog(sub("abcdef", 1, 3));       // bcd
Console.WriteLog(sub("abcdef", -2, 2));      // ef   a negative start counts from the end
Console.WriteLog(index_of("abcdef", "cd"));  // 2    or -1
Console.WriteLog(contains("abcdef", "zz"));  // false
Console.WriteLog(replace("a-b-c", "-", "="));// a=b=c
Console.WriteLog(repeat("ab", 3));           // ababab

foreach (var part in split("a,b,c", ",")) {   // split returns a real string[]
    Console.WriteLog(part);
}
```

A few things worth knowing. A `null` string reads as empty rather than
crashing. `sub` clamps at both ends instead of trapping, and an out-of-range
`char_at` is `-1` so a loop walking to the end can tell. Lengths are **bytes**,
not characters — Z has no character type, so one character of UTF-8 may be
several bytes, and `char_at` hands you each byte in turn. `upper`/`lower` are
ASCII-only and leave bytes above 127 alone, because case-mapping a UTF-8
continuation byte on its own would corrupt the sequence.

### A string is a value with a length

```csharp
var s = "hello world";
Console.WriteLog(s.length);          // 11
Console.WriteLog(s[0]);              // 104   the byte, as an int
Console.WriteLog(s[1..3]);           // el    half-open: s[a..b] keeps a..b-1
Console.WriteLog(s[..5]);            // hello an end may be left out
Console.WriteLog(s[6..]);            // world and so may the start
Console.WriteLog(s[1..999]);          // ello world, an end past the end just clamps
Console.WriteLog(s[4..2]);           // (empty) an inverted range is empty, not an error

var z = "a\0bc";
Console.WriteLog(z.length);          // 4     a zero byte is a byte, not an end
Console.WriteLog(z[1]);              // 0
Console.WriteLog(z[0] + z[2] + z[3]);// 294
Console.WriteLog(contains("ab\0cd", "b\0"));   // true

Console.WriteLog("ab" < "abc");      // true   a prefix sorts before what extends it
Console.WriteLog("ab\0c" == "ab\0d"); // false, and strcmp would have called this equal
```

Three things follow from the length being stored rather than found by scanning
for a zero.

**`len` is a load, not a walk.** It used to be a `strlen`. Now it is a read of
the header immediately before the bytes.

**A zero byte is data.** A C string ends at the first zero, so `"a\0bc"` used to
print as nothing and measure as 0, and every function that took it stopped early.
Now the length is 4, every byte is reachable, `Console.WriteLog` writes all
four, and
`split`, `replace` and `index_of` all see it. C interoperability still stops at
a zero — that is a property of C's strings, not a bug in this one — and
[section 18](#types) says how a C caller gets past it.

**Comparison is by content, then by length.** Not by `strcmp`, which cannot order
two strings that differ only after an embedded zero.

### Building a string

`+` allocates a new string and copies both sides, and it still does. A `string` is
a value: `s = s + "x"` writes a *new* string and leaves every other name bound to
`s` alone. That is worth one copy, because it means `s` means one thing.

The cost is that building a string of *n* pieces in a loop copies O(n²) bytes,
and no amount of cleverness in the runtime fixes that without taking the value
semantics away. So the mutable buffer is a separate type that says so:

```csharp
var b = new StringBuilder();
for (var i = 0; i < 200; i++) {
    b.append("ab");
}
b.append("!");
b.appendInt(42);
Console.WriteLog(b.length());            // 403
Console.WriteLog(b.toString()[402]);     // 50, the '2'
Console.WriteLog(ends_with(b.toString(), "42"));    // true
```

`append` is amortized constant time: the builder owns its buffer, so appending
extends it in place and reallocates only when it is full.

`toString()` gives you a **copy**, and that is the whole point of the difference
between it and `take()`. The buffer belongs to the builder, so a string handed
straight out of it would change under its reader the next time anything was
appended. `take()` hands the buffer over and takes a new one, which is safe
because the builder gives up its claim — use it for the common tail of "build a
string, pass it on, don't need it again".

### The integer library

```csharp
Console.WriteLog(pow(2, 10));     // 1024
Console.WriteLog(gcd(12, 18));    // 6
Console.WriteLog(lcm(4, 6));      // 12
```

`exp`, `log` and `tan` are absent. They are worth having only for a `float`, and
there is nothing to call them on yet -- which is also why `sqrt`/`sin`/`cos` above
are the fixed-point integer versions rather than the real ones.

---

## 6. String interpolation and concatenation

The `+` operator concatenates strings (an `int` or `bool` operand is converted
automatically). The `$"..."` form interpolates expressions inline:

```csharp
var name = "World";
var n = 3;
Console.WriteLog("count = " + 42);            // count = 42
Console.WriteLog("name: " + name);           // name: World
Console.WriteLog($"Hello, {name}!");          // Hello, World!
Console.WriteLog($"n={n}, n*n={n*n}");        // n=3, n*n=9
Console.WriteLog($"bool: {true}");            // bool: true
```

Nested quotes inside an interpolation work: `Console.WriteLog($"a {"b"} c");`.

### Escapes, and literal braces

Inside an interpolated string, a brace is either a hole or a character. To write
the character, escape it — or spell it as a byte or a code point, which is also
how you get a brace that would otherwise open a hole:

```csharp
Console.WriteLog($"a\{b}c");              // a{b}c      a literal brace, nothing interpolated
Console.WriteLog($"\x7bnot a hole\x7d");    // {not a hole}
```

Outside an interpolated string a brace needs no escape, and `\{` is a mistake
worth reporting.

### Comparing strings

All six relational operators work on strings, ordered lexicographically — the
same ordering C's `strcmp` gives, so a shorter string that is a prefix of a
longer one sorts first.

```csharp
Console.WriteLog("hello" == "hello" ? 1 : 0);   // 1
Console.WriteLog("hello" == "world" ? 1 : 0);   // 0
Console.WriteLog("apple" < "banana" ? 1 : 0);   // 1
Console.WriteLog("ab" < "abc" ? 1 : 0);         // 1
Console.WriteLog("B" < "a" ? 1 : 0);            // 1  (byte order, so uppercase sorts first)

if ("q" > "p") { Console.WriteLog("yes"); }     // usable as a condition
```

Comparison is by byte value, not by any notion of alphabetical order, and it is
case-sensitive.

---

## 7. Control flow

### Conditionals

```csharp
var x = 10;
if (x > 5) {
    Console.WriteLog("big");
} else {
    Console.WriteLog("small");
}
```

The ternary operator is available too:

```csharp
var x = 10;
Console.WriteLog(x > 5 ? "big" : "small");   // big
```

### `while`

```csharp
var i = 0;
while (i < 3) {
    Console.WriteLog(i);
    i = i + 1;                    // 0, 1, 2
}
```

### `for`

`for` is C#-style: `init; condition; step`.

```csharp
for (var i = 0; i < 5; i++) {
    Console.WriteLog(i);                     // 0 1 2 3 4
}
```

### `foreach`

`foreach` iterates an array. The loop variable may be typed or bare:

```csharp
var a = new int[3];
a[0] = 10; a[1] = 20; a[2] = 30;
foreach (var v in a) { Console.WriteLog(v); }   // 10 20 30
foreach (v in a) { Console.WriteLog(v); }       // also fine (type inferred)
```

### `break` and `continue`

`break` leaves the innermost loop; `continue` starts the next iteration.

```csharp
var i = 0;
while (true) {
    i = i + 1;
    if (i > 5) { break; }        // stops the loop entirely
}
Console.WriteLog(i);                        // 6

var s = 0;
for (var k = 0; k < 10; k++) {
    if (k < 3) { continue; }     // skips the rest of this iteration...
    s = s + k;                   // ...so this only runs for k >= 3
}
Console.WriteLog(s);                        // 42
```

In a `for` loop, `continue` jumps to the step, not back to the top of the body,
so the counter still advances. `break` and `continue` inside nested loops
affect only the innermost one, and using either outside a loop is a compile
error rather than a silent no-op.

---

## 8. Functions

Functions use `returnType name(params) { ... }`. Recursion works:

```csharp
int fib(int n) {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}
Console.WriteLog(fib(15));   // 610
```

A single-expression function can use `=>` (expression-bodied):

```csharp
int square(int n) => n * n;
Console.WriteLog(square(7));   // 49
```

### The entry point

You can write a program two ways. Either use **top-level statements** (no
`main` needed), or define a function literally named `main`:

```csharp
// top-level style (used throughout this tutorial)
Console.WriteLog("hi");
```

```csharp
// or explicit main
int main() {
    Console.WriteLog("hi");
    return 0;
}
```

You cannot mix top-level statements with a `main` function.

### Function pointers

`&f` takes a function's address. The result is typed by that function's
signature, so `var` needs no annotation:

```csharp
int dbl(int x) { return x * 2; }

var f = &dbl;
Console.WriteLog(f(21));      // 42
```

Where a *type* is needed, write it as `fn(params) -> ret`:

```csharp
int apply(fn(int) -> int f, int v) { return f(v); }
Console.WriteLog(apply(&dbl, 5));     // 10
```

A call through a pointer is checked like a direct call — the argument count and
types must match. Two function pointers can be compared with `==` and `!=` to
test whether they point at the same function; they cannot be ordered, since
there is no meaningful "less than" for a code address.

Function pointers are ordinary values, so they can go in a variable, an array, a
struct field, or be passed around:

```csharp
var fs = new fn(int) -> int[2];
fs[0] = &dbl;
Console.WriteLog(fs[0](4));            // 8

struct Op { fn(int) -> int f; }
var op = new Op(&dbl);
Console.WriteLog(op.f(3));             // 6
```

### Method pointers

`&obj.M` binds the receiver and yields a callable. Where a *type* is needed, it
is spelled `method(params) -> ret` — deliberately distinct from `fn`, because the
two have different values and different call sequences:

```csharp
class Counter {
    int n;
    Counter(int a) { n = a; }
    int add(int x) { return n + x; }
    void bump(int by) { n = n + by; return; }
}

var c = new Counter(5);
var m = &c.add;
Console.WriteLog(m(3));            // 8
c.bump(10);
Console.WriteLog(m(0));            // 15   the receiver is bound, so mutation shows through
```

A `method(...)` value is a pointer to a garbage-collected cell holding the code
address and the receiver, so the receiver stays alive exactly as long as the
pointer does. That means the receiver may be a local that has otherwise gone out
of scope.

Method pointers go in arrays and struct fields and can be passed as parameters,
just like function pointers. A **virtual** method binds through the object's
vtable, so the pointer dispatches on the runtime type rather than pinning the
implementation you named.

Two things are deliberately not supported. A **struct** receiver cannot be bound,
because a struct's methods take the receiver by value — there is nothing to hold a
reference to. And a method pointer cannot return a struct, since the receiver
already occupies the register a struct result would need. A plain `fn` pointer
returning a struct is fine; it is only the bound form that cannot.

A function pointer is not a closure: it captures nothing. To capture, use a
lambda.

### Closures

A lambda is written with `=>`. It is an expression, so it fits wherever an
expression does, and it captures any variable in scope where it is written:

```csharp
var add = (int a, int b) => a + b;
Console.WriteLog(add(2, 3));            // 5
```

A one-expression body *is* the result. A block body must `return` on every path.
The parentheses are required even with no parameters, so `() => 1` is a closure
and `x => x` is not.

A function that returns a lambda declares its result as `closure`, with the same
spelling as `fn`:

```csharp
closure(int) -> int makeAdder(int n) {
    return (int x) => x + n;
}

var plus10 = makeAdder(10);
Console.WriteLog(plus10(5));            // 15
```

`closure` and `fn` are separate types on purpose — a closure carries an
environment, which changes how it is called — so assigning a lambda to an `int`,
or declaring a lambda-returning function as `fn`, is an error rather than
something that fails later.

The point of capturing is that the closure outlives the frame it was written in:

```csharp
closure() -> int counter() {
    var n = 0;
    return () => { n = n + 1; return n; };
}

var c = counter();
Console.WriteLog(c());                  // 1
Console.WriteLog(c());                  // 2
Console.WriteLog(c());                  // 3
```

Writing to a captured variable writes to the closure's own copy, shared by every
closure made from the same `n`; the enclosing frame is untouched. Captured
variables are boxed for this reason, so the copies are the same cell.

Capture is by reference for anything with fields, so a lambda over a struct or
class reads and writes the real thing:

```csharp
struct P { int x; int y; }
var p = new P(3, 4);
var getX = () => p.x;
Console.WriteLog(getX());               // 3
```

There is one limit: a lambda may not be written inside another lambda. A
function may define as many lambdas as it likes, and may return one.

### Nested functions

A function can also be declared inside another one, written where a statement
goes. This is the named counterpart to a lambda — use it when the function does
not need to capture, and a lambda when it does.

```csharp
int outer() {
    int helper(int n) { return n + 1; }
    int twice(int n) { return n * 2; }
    return helper(twice(5));
}
Console.WriteLog(outer());             // 11
```

It is hoisted out to the top level and emitted under a symbol derived from the
enclosing function, so two functions that each declare `helper` do not collide.
It can call itself, because its signature is filed as it is read:

```csharp
int fact(int n) {
    if (n < 2) { return 1; }
    return n * fact(n - 1);
}
Console.WriteLog(fact(5));             // 120
```

Declare it before you call it. A nested function's scope runs to the end of the
enclosing function, but nothing pre-scans a body, so a sibling declared further
down is not yet known at the call.

**A nested function cannot capture.** It has a frame of its own and frame slots
are numbered per function, so naming a variable declared beside it would read
whatever its own frame happened to hold. That is an error, not a wrong answer:

```csharp
int main() {
    var k = 7;
    int peek() { return k; }   // error: undefined variable 'k'
    Console.WriteLog(peek());
    return 0;
}
```

The same rule stops a top-level function from reading a top-level variable. When
you want to capture, write a lambda — `int addK(int x) => x + k;` becomes
`var addK = (int x) => x + k;`.

### `Result` and `?`

A call that can fail returns a `Result`: an `Ok` carrying a value, or an `Err`
carrying an error. Because a `Result` is a two-variant union, `match` handles it
and the exhaustiveness rule applies like any other.

```csharp
Result<int, string> parse(string s) {
    if (s == "42") { return Ok(42); }
    return Err("not a number: " + s);
}

Console.WriteLog(match parse("42") { Ok(v) => v, Err(e) => 0 });   // 42
Console.WriteLog(match parse("x")  { Ok(_) => 0, Err(e) => 0 });  // 0
```

`_` is a binding that is deliberately not read, so an arm can ignore the payload
it does not need. Each arm is its own scope, so both arms above may bind `e`.

**`?` propagates the error.** On a `Result`, inside a function that returns a
`Result` with the same error type, `expr?` hands you the `Ok` payload — and
returns the `Err` from the function if that is what it turned out to be:

```csharp
Result<bool, string> positive(string s) {
    var n = parse(s)?;     // an Err here leaves the function right here
    return Ok(n > 0);
}
```

Watch the two `Result`s: different `T`, same `E`. That is the whole trick. The
error travels up untouched while the payload comes out, so a chain of calls that
can each fail reads as straight-line code, and whoever cares about the error
handles it once, at the end.

`Ok(3)` cannot be written on its own — it does not say what the error type is. It
takes that from where the value is going: a declared type, a `return`, or a
parameter. Both sides of a `Result` must be one word (`int`, `bool`, `float`,
`string`, or a pointer); a struct by value does not fit in one, and a pointer to
it does.

### Interfaces

An interface is a named set of method signatures. A type satisfies one by having
those methods — there is nothing to declare, because a struct has no vtable of
its own to list them in.

```csharp
interface Shape {
    int Area();
    string Name();
}

struct Square { int side; int Area() { return side * side; } string Name() => "square"; }
struct Rect   { int w; int h; int Area() { return w * h; } string Name() { return "rect"; } }
```

Assigning to an interface-typed place converts, and a call resolves to the
interface's method rather than to anything the value happens to be:

```csharp
Shape a = new Square(5);
Console.WriteLog(a.Area());      // 25
Console.WriteLog(a.Name());      // square
```

That is the point: unrelated types in one collection. Structs, classes and
subclasses of different hierarchies, all called the same way.

```csharp
var shapes = new Shape[2];
shapes[0] = new Square(3);
shapes[1] = new Circle(2);      // a class, in the same array
for (var i = 0; i < 2; i = i + 1) { Console.WriteLog(shapes[i].Name()); }
```

An interface value is a *pointer* to a two-word cell holding a method table and
the receiver, so it behaves like a pointer everywhere: it can be `null`, and
comparing it to `null` works. A struct is copied to the heap when it becomes an
interface value, because the cell outlives the frame it was made in.

A **class** must declare every method it offers to an interface `virtual` or
`override`, since it is dispatched through its vtable; a struct has no such
requirement. The method has to match the signature, not just the name, and the
error says which went wrong.

`&nestedFn` works and has the same `fn(params) -> ret` type as any other
function, so a nested function can be passed around like a top-level one. A
function name and a variable of the same name can coexist; they are different
namespaces, as in C.

---

## 9. Arrays

Arrays live on the heap. Create with `new T[n]`, read/write with `[]`, and get
the length with `.length`:

```csharp
var a = new int[5];       // 5 zeros
a[0] = 10;
a[1] = 20;
Console.WriteLog(a.length);          // 5
Console.WriteLog(a[0] + a[1]);       // 30
```

Combine with a loop:

```csharp
var squares = new int[5];
for (var i = 0; i < 5; i++) { squares[i] = i * i; }
foreach (var s in squares) { Console.WriteLog(s); }   // 0 1 4 9 16
```

### Modules

`import "path.z";` pulls another file's top-level declarations into the current
one. There is no namespace: an imported name is simply visible, exactly as if the
text had been pasted in above the import.

Paths resolve **relative to the importing file**, so a file can be moved with
its neighbours and still work:

```csharp
import "lib/math.z";
import "lib/shapes.z";

int main() {
    Console.WriteLog(triple(4));     // 12, from lib/math.z
    return 0;
}
```

An imported file may import others, to any depth. Each file is expanded once
however many times it is named, so two files importing the same helper is not a
duplicate-definition error. An import cycle is reported rather than followed.

Everything crosses the boundary, not just functions: `const`s, structs, classes
and `extern` declarations all work. Note that a `const` must be declared before
use, and imported declarations are spliced in ahead of the importing file, so
they are always in scope.

A diagnostic in an imported file names that file and quotes it, not the file that
imported it.

---

## 10. Pointers

Take an address with `&`, dereference with `*`:

```csharp
var x = 10;
var p = &x;
Console.WriteLog(*p);      // 10
*p = 99;
Console.WriteLog(x);       // 99
```

Pointers are useful for mutating through a reference, and for passing
by-reference parameters:

```csharp
void setTo(int* q, int v) { *q = v; return; }
int n = 5;
setTo(&n, 42);
Console.WriteLog(n);       // 42
```

Pointers nest, so a pointer to a pointer (`int**`) is a type you can write:

```csharp
var a = new int[2];
a[0] = 7;
var p = &a[0];           // int*
var t = &p;              // int**
Console.WriteLog(**t);              // 7
```

`&a[0]` is how you get a pointer to an array's first element. Passing an array
itself where a pointer is expected is a type error — arrays do not decay the way
they do in C.

### `null`

`null` is the null pointer. It can be compared against any pointer, and assigned
into any pointer slot:

```csharp
var p = null;
if (p == null) { Console.WriteLog("unset"); }        // unset
var q = &a[0];
if (q != null) { Console.WriteLog("set"); }          // set
p = q;                                    // a var declared null takes a pointer later
```

`var p = null;` gives `p` the type `int*`, so it is assignable later. Comparing
`null` with a non-pointer, such as `p == 0`, is a type error; write `p == null`.

---

## 11. Structs (value types)

A `struct` is a value type: copying a struct copies its fields.

```csharp
struct Point { int x; int y; }

var p = new Point(3, 4);   // positional constructor (matches field order)
Console.WriteLog(p.x + p.y);          // 7

var q = p;                  // value copy
q.x = 10;
Console.WriteLog(p.x);                // 3   (p is unchanged)
Console.WriteLog(q.x);                // 10
```

### Methods

Struct methods get an implicit `this` (C# style):

```csharp
struct Point {
    int x; int y;
    int Sum() { return x + y; }
    int Mag2() => x * x + y * y;    // expression-bodied
}

var pt = new Point(3, 4);
Console.WriteLog(pt.Sum());     // 7
Console.WriteLog(pt.Mag2());    // 25
```

To pass a struct to another method, use a pointer parameter `Type*` and call
with `&`:

```csharp
struct Point { int x; int y; int Dot(Point* o) { return x * o.x + y * o.y; } }
var a = new Point(1, 2);
var b = new Point(3, 4);
Console.WriteLog(a.Dot(&b));   // 11
```

### Auto-properties

`int Id { get; set; }` creates a property backed by a hidden field:

```csharp
struct Acct { int id; int Id { get; set; } }
var acc = new Acct(0);
acc.Id = 9;
Console.WriteLog(acc.Id);   // 9
```

### Operator overloading

Define `op_<Name>` methods to overload operators for your struct:

```csharp
struct Vec {
    int x; int y;
    Vec op_Add(Vec* o) => new Vec(x + o.x, y + o.y);
    Vec op_Mul(int k)  => new Vec(x * k, y * k);
    bool op_Eq(Vec* o) => x == o.x && y == o.y;
    string Show()      => $"({x}, {y})";
}

var a = new Vec(3, 4);
var b = new Vec(1, 2);
Console.WriteLog(a + b);          // (4, 6)   -> "+" returns a Vec, print shows it via Show()
Console.WriteLog(a * 3);          // (9, 12)
Console.WriteLog(a == b);         // false
```

### Extension methods

Add a method to an existing type by marking the first parameter `this`:

```csharp
int Twice(this int n) { return n * 2; }
string Exclaim(this string s) => s + "!";
bool IsEven(this int n) => n % 2 == 0;

Console.WriteLog(21.Twice());      // 42
Console.WriteLog("hi".Exclaim());  // hi!
Console.WriteLog(4.IsEven());      // true
```

---

## 12. Enums and pattern matching

An `enum` defines a set of *variants*, each optionally carrying data. This is
Z's sum-type facility.

```csharp
enum Shape {
    Circle(int r),
    Rect(int w, int h),
    Point
}
```

Construct a variant by calling its name. Use `match` to destructure it:

```csharp
int area(Shape s) => match s {
    Circle(r)   => 3 * r * r,   // pi ≈ 3
    Rect(w, h)  => w * h,
    Point       => 0
};

Console.WriteLog(area(Circle(5)));   // 75
Console.WriteLog(area(Rect(3, 4)));  // 12
Console.WriteLog(area(Point));       // 0
```

The `match` must be **exhaustive** — cover every variant. Add a new variant to
`Shape` and any non-exhaustive `match` becomes a compile error. Bindings
(`r`, `w`, `h`) name the variant's payload. String interpolation works inside
arms:

```csharp
string describe(Shape s) => match s {
    Circle(r)  => $"circle r={r}",
    Rect(w, h) => $"{w}x{h} rect",
    Point      => "a point"
};
Console.WriteLog(describe(Rect(3, 4)));   // 3x4 rect
```

---

## 13. Classes (reference types + vtables)

A `class` is heap-allocated and supports **dynamic dispatch** through a vtable.
Z has **no inheritance** — an `interface` (below) is what plays that role.

```csharp
class Dog {
    int legs;
    string name;
    Dog(int l, string n) { this.legs = l; this.name = n; }   // constructor
    virtual string Speak() { return this.name + " woof"; }   // virtual
    virtual int Legs() { return this.legs; }                  // virtual
}

var d = new Dog(4, "Rex");
Console.WriteLog(d.Speak());   // Rex woof
Console.WriteLog(d.Legs());    // 4
```

- `virtual` gives the method a dispatch slot, one per virtual in declaration
  order. Calling one reads the implementation out of the object's vtable; a
  non-virtual method is called directly.
- A class becomes useful for dispatch when it satisfies an **interface**: every
  method the interface names must be `virtual`, and an interface value calls
  through the itab. That is how unrelated types end up in one collection:

```csharp
interface Animal { string Speak(); int Legs(); }

class Cat {
    int legs;
    Cat(int l) { this.legs = l; }
    virtual string Speak() { return "meow"; }
    virtual int Legs() { return this.legs; }
}

var animals = new Animal[2];
animals[0] = new Dog(4, "Rex");
animals[1] = new Cat(4);
Console.WriteLog(animals[0].Speak());   // Rex woof
Console.WriteLog(animals[1].Speak());   // meow
```

- `new C(args)` allocates a garbage-collected object and runs the constructor
  (a method named like the class). Fields start zeroed.
- There is no `: Base`, no `override` and no `base(args)`. Each is a diagnostic
  that says what to do instead, because a program carried over from a version
  that had them needs to be told rather than left with an undefined name.

---

## 14. Interfaces

An `interface` is a named set of method signatures. A type satisfies it simply by
having those methods — nothing is declared, and neither `class` nor `struct` is
required to be a subclass of anything.

```csharp
interface Shape {
    int Area();
    string Name();
}

class Square {
    int side;
    Square(int s) { this.side = s; }
    virtual int Area() { return this.side * this.side; }   // virtual: a class
    virtual string Name() { return "square"; }             // must be virtual
}

struct Rect {                                              // a struct satisfies
    int w; int h;                                          // it too, without
    int Area() { return w * h; }                           // declaring virtual
    string Name() { return "rect"; }
}
```

Assigning a value to an interface-typed place converts it, and a call resolves
to the interface's method rather than to whatever the value happens to be:

```csharp
var a = new Square(5);
Console.WriteLog(a.Area());   // 25

Shape one = new Square(5);
Console.WriteLog(one.Area());  // 25   -- dispatched through the interface
```

- A **`class`** must declare every method it offers to an interface `virtual`,
  because a non-virtual method has no vtable slot to dispatch through.
- A **`struct`** has no such requirement: its methods are reached directly.
- A struct is copied to the heap when it becomes an interface value, because the
  cell outlives the frame. A class is already a pointer, so it goes in as it is.
- `null` is a valid interface value, and calling one is a mistake worth
  diagnosing.

This is what a class hierarchy was for, and it is why Z has no inheritance: an
interface collects related types without a base type to upcast to, a base
constructor that can be forgotten, or a vtable layout that has to be inherited.
A `struct` and a `class` can sit in the same array as long as both satisfy the
interface.

---

## 15. Generics

Generic *functions* are compiled by monomorphization: each concrete set of type
arguments produces a specialized, natively compiled copy. Type arguments are
inferred at the call site.

```csharp
T max<T>(T a, T b) { if (a > b) { return a; } return b; }
U pick<T, U>(T a, U b) { return b; }          // two type parameters
int total<T>(T[] xs) { var s = 0; foreach (v in xs) { s = s + v; } return s; }

Console.WriteLog(max(3, 7));      // 7
Console.WriteLog(max(10, 4));     // 10
Console.WriteLog(pick(3, 9));     // 9

var arr = new int[4]; arr[0]=1; arr[1]=2; arr[2]=3; arr[3]=4;
Console.WriteLog(total(arr));     // 10
```

The declaration must appear before its uses. There is no runtime generic
machinery, boxing, or type erasure — every instantiation is real native code.

---

## 16. Memory & the garbage collector

The heap (arrays, `new` objects, and strings) is managed by a conservative
mark-sweep collector. You never free anything:

```csharp
var live = "survivor";
for (var i = 0; i < 100000; i = i + 1) { live = live + "x"; }  // tons of garbage
Console.WriteLog(live);   // "survivorxxx..." — `live` survived
```

Locals live on the stack (no GC cost); only heap allocations are collected.

### Bounds checking

Array indexing is **not** checked by default, so it costs nothing. Pass
`--bounds` and every index into an array is range-checked at runtime:

```sh
./z run game.z --bounds
```

```console
$ ./z run oob.z --bounds
runtime error: array index 5 out of bounds (length 3)
```

The check reads the length out of the array's own header, so a length that is
only known at runtime is still checked exactly, and a negative index is caught
too. Out-of-range access aborts with a message instead of quietly reading
whatever follows the array in the heap.

The flag applies to `build` as well, and costs a length load plus two branches
per access, which is why it is off by default — turn it on while testing, and
leave it off for anything you care about throughput.

---

## 17. A complete program

Putting many features together — classes, generics, enums, operator
overloading, and control flow:

```csharp
interface Shape {
    int Area();
    string Name();
}
class Square {
    int side;
    Square(int s) { this.side = s; }
    virtual int Area() { return this.side * this.side; }
    virtual string Name() { return "square"; }
}

enum Op { Add(int a, int b), Mul(int a, int b) }
int apply(Op o) => match o { Add(x, y) => x + y, Mul(m, n) => m * n };

T maxOf<T>(T a, T b) { if (a > b) { return a; } return b; }

int main() {
    var shapes = new Shape[2];
    shapes[0] = new Square(5);
    shapes[1] = new Square(3);

    for (var i = 0; i < 2; i++) {
        Console.WriteLog($"{shapes[i].Name()} area = {shapes[i].Area()}");
    }
    // square area = 25
    // square area = 9

    Console.WriteLog(apply(Add(2, 3)) + " " + apply(Mul(4, 5)));   // 5 20
    Console.WriteLog(maxOf(7, 9));                                 // 9
    return 0;
}
```

---

## 18. Debugging

`z build prog.z -o prog -g` emits DWARF, and a debugger can use it directly:

```sh
z build prog.z -o prog -g
gdb ./prog
(gdb) break prog.z:12      # a line, not an address
(gdb) break add            # or a function
(gdb) run
(gdb) print count          # a parameter
(gdb) info locals
(gdb) where                # a backtrace
```

The line table is exact, so breakpoints on a line work even though the code
moved. Function names, parameters and their values are all there.

Two honest limits. A local the compiler promoted to a register is reported as
*optimized out* rather than given a value that would only sometimes be right —
describing a variable that moves between a register and the stack properly needs
location lists, which is more machinery than this earns. And a local is typed
only if its type is one of Z's three scalars (`long`, boolean, `char *`);
anything else shows up untyped rather than mistyped.

---

## 19. Calling C from Z

Z can call C, and C can call Z. Both directions are declared in the Z source,
and no assembly rewriting is needed.

### `extern` — implemented in C

`extern` declares a function whose body lives in C. The symbol is used exactly
as written, and nothing is emitted for it:

```csharp
extern int c_add(int a, int b);
extern int c_strlen(string s);

int main() {
    Console.WriteLog(c_add(2, 3));         // 5
    Console.WriteLog(c_strlen("hello"));   // 5
    return 0;
}
```

### `export` — defined in Z, callable from C

`export` does the other half: the body is here, but the symbol keeps the name
as written and is made visible outside the Z translation unit, so C can call it.

```csharp
export int z_triple(int v) { return v * 3; }
```

### Types

Z's `int` is 64-bit and maps onto C's `long`; `bool` maps onto an `int` that is
0 or 1; `string` is a NUL-terminated `const char *`. Structs and arrays are
passed as pointers.

A Z `string` keeps its length in a header immediately before the bytes the value
points at, and a NUL terminator after them, so it is a valid `const char *` and
needs no conversion. The terminator is what makes the boundary work; the header
is what lets a C function that needs the *whole* string read it, at `s[-16]`. C
itself still stops at a zero byte, so a Z string containing one crosses the
boundary truncated unless the C side asks for the length.

### Linking

Anything the compiler does not recognize after the source file is handed
straight to the link step, so a `.c` file, a `.o`, or a library all work:

```sh
./z run main.z host.c        # compile host.c and link it in
./z build main.z -o main -lm # pass a library to the linker
./z build main.z -o main -L/opt/lib -lfoo
```

**One limitation:** a struct cannot be returned by value across the boundary. Z
returns every aggregate through a hidden result pointer, while the C ABI returns
aggregates of 16 bytes or fewer in registers, so the two conventions disagree.
An `extern` function should return `int`, `bool`, or `string`. Returning a
struct *from* Z to Z is fine, since both sides use the same convention.

### Why symbols are namespaced

Every Z function you write is emitted under a private prefix (`z$` plus the
name), so it cannot collide with a libc symbol, one of the runtime's own
helpers, or — importantly — with the words the assembler reserves. Under
`.intel_syntax` the assembler reads `near`, `far`, `byte`, `word`, `dword` and
`qword` as keywords; before namespacing, a Z function called `near` assembled
into a branch to a garbage address with no error from either the assembler or
the linker. The separator is `$`, which the assembler accepts in a symbol but
which cannot appear in a Z identifier, so a program cannot accidentally define
something in that namespace. `extern` and `export` are how you name a symbol
deliberately.

---

## 20. Things to know

- **`float` is binary64**, the format C calls `double`. Division by zero gives an
  infinity rather than trapping, and a NaN compares false against everything
  including itself, so `if (x != y)` is not a safe "are these the same value?"
  test. See section 3.
- **`sqrt`, `sin` and `cos` are integer built-ins.** They are *not* the
  floating-point functions of the same name: trig uses a full turn of
  `1 << 30` and Q30 results, and `sqrt` is an exact integer root. For a real
  square root, `sqrt` a float argument after a cast -- or use `x * 0.5` style
  Newton steps, since there is no `exp`/`log`/`pow` for floats.
- **Literals come in every radix.** `0xff`, `0o755`, `0b1010_0110`, and `_` as
  a digit separator, so `1_000_000` is a million. A literal too large for `int`
  is a compile error, not a silent wrap.
- **Bitwise operators wrap** on overflow, matching C on a 64-bit `int`.
- **Structs are values, classes are references.** Assigning or passing a struct
  copies it; a class variable holds a pointer to a heap object.
- **`foreach` loop variables** must be `var x`, a typed name (`int x`), or a
  bare identifier (`x`) — all infer the element type. The collection must be an
  array: a pointer has no length, so there is no bound to iterate to.
- **Match-arm bindings** share one scope per `match`, so reuse distinct names
  across arms.
- **Compile-time errors** (type errors, non-exhaustive `match`, undefined names)
  are reported with the file, line, column, and a caret.
- **The compiler warns too.** An unused local, a shadowed one, or a statement
  that can never run. Warnings are on by default and never fail a build; use
  `-w` to silence them, `-Werror` to make them fail, and `-Wno-<name>` for one.
- **A function takes at most 16 parameters**, or 15 if it returns a struct or
  union. A method's list is capped the same way but one lower, at 15, because
  the receiver is a parameter too. The argument registers run out well before
  those numbers, and the surplus is passed on the stack. Exceeding one is a
  compile error rather than silently wrong code.
- **Array and string indexing are unchecked** unless you pass `--bounds`. See
  section 15.
- **A `string` is a sequence of bytes, and that is all it is.** `s.length`,
  `s[i]` and `s[a..b]` all count and slice bytes. Z has no character type, so
  one `é` is two ints and a `\uXXXX` escape is two to four of them. Nothing
  validates UTF-8, and nothing needs to: the type makes no claim that the bytes
  are text.
- **`+` on a string always copies.** It has to, because a `string` is a value and
  two names bound to one string must not see each other's appends. Use
  `StringBuilder` to build a string in a loop.
- **A `const` must be declared before use**, unlike a function.
- **`break`/`continue` outside a loop** is a compile error.
- **Structs cannot be returned by value across the C boundary.** See section 17.
- **`import` splices, it does not isolate.** An imported name is visible
  everywhere, with no namespace to qualify it and nothing marked private.
- **The standard library is Z source inside the compiler.** `lib/*.z` is spliced
  ahead of every program, so `StringBuilder` is checked and compiled by the same
  front end your code is. `--no-std` compiles without it.
- **Three callable things, none interchangeable.** `&f` is a bare code address
  and captures nothing. A lambda is a closure -- a `{ code, env }` cell -- and
  captures by value the declarations its body names, so `var add = () => x + 1`
  keeps working when `x` changes afterwards. A `method(...)` pointer is a
  `{ code, receiver }` cell: it binds the receiver, and still captures nothing.
  A closure that was written in another function's body cannot outlive that
  body's frame, which is why the compiler declines to inline one.
- **A small same-file function is inlined, not called**, from `-O1` up. It makes
  no difference to what your program prints, and you should not write code around
  it, but it is why a debugger may step you through a function you never called:
  the body is there, and a `step` follows the code rather than the calls.
- **`method(...)` and `fn(...)` are not interchangeable.** One holds a code
  address; the other holds a `{ code, receiver }` binding, and a call through it
  spends an extra argument register on the receiver.

---

## 21. Where to go next

- `docs/LANGUAGE.md` — the full language specification / grammar.
- `README.md` — project overview, architecture, performance, and roadmap.
- `tests/cases/*.z` — a large set of runnable example programs.
- `src/` — the compiler source (lexer → parser → type checker → x86-64 codegen).

Happy coding!
