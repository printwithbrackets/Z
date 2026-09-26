# Z — Tutorial

Z is a small, statically-typed, C#-flavored language that compiles to native
x86-64 machine code. It has a real type system, value types (structs),
reference types (classes) with inheritance and dynamic dispatch, monomorphized
generics, pattern matching, a tracing garbage collector, and a modern optimizing
backend — with no runtime dependency beyond libc.

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
print("Hello, world!");
```

```
Hello, world!
```

`print` is a builtin that accepts `int`, `bool`, or `string`.

---

## 3. Variables and types

Z has three built-in value types — `int` (64-bit signed), `bool`, and
`string` — plus the composite types you'll meet later (structs, classes,
arrays, pointers).

Use `var` to let the type be inferred, or name the type explicitly:

```csharp
var a = 42;          // int
var b = 3.5;         // (not a thing — no floats; see Note below)
```

Z has no floating-point type yet. Here are valid declarations:

```csharp
var count = 10;        // int
var ok = true;         // bool
var name = "Zeta";     // string
int explicit_int = 5;  // explicit type
bool flag = false;
```

Every `var` binding must be initialized where it is declared.

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

print(HALF);                     // 50
```

A const must be declared **before** it is used, the same way types must be. That
is different from functions, whose signatures are all known before the body of
anything is compiled, so a function may be called before it is written. Consts
may sit at the top level next to `main`.

---

## 4. Operators

Arithmetic, comparison, and logic work as you'd expect (integers are 64-bit):

```csharp
print(2 + 3);      // 5
print(7 - 2);      // 5
print(6 * 7);      // 42
print(7 / 2);      // 3   (integer division)
print(7 % 2);      // 1   (remainder)
print(1 + 2 * 3);  // 7   (* binds tighter than +)
print(10 / 3);     // 3

print(3 > 2);      // true
print(3 == 3);     // true
print(3 != 3);     // false
print(true && false);  // false
print(true || false);  // true
print(!true);          // false
```

Increment/decrement work on variables:

```csharp
var i = 5;
i++;
print(i);   // 6
i--;
print(i);   // 5
```

Bitwise operators work on the full 64-bit value, and wrap on overflow the way
C's do:

```csharp
print(12 & 10);        // 8    (1100 & 1010)
print(12 | 10);        // 14   (1100 | 1010)
print(12 ^ 10);        // 6    (1100 ^ 1010)
print(~0);             // -1   (all 64 bits set)
print(1 << 10);        // 1024
print(1024 >> 3);      // 128
```

The compound forms work too, and are common in flag manipulation:

```csharp
var flags = 0;
flags |= 1;      // set bit 0
flags |= 4;      // set bit 2
print(flags);    // 5
flags &= 6;      // keep only bits 1 and 2
print(flags);    // 4
flags ^= 4;      // clear bit 2
print(flags);    // 0
```

There are no hex or binary literals, and no octal; write the decimal value, or
build it with shifts (`1 << 20`).

**Note on division:** division and modulo by a *constant* are strength-reduced
by the compiler into a multiply–shift (much faster than a hardware divide), so
`x % 1000000007` in a hot loop is cheap.

---

## 5. Built-in functions

A handful of functions are built into the compiler rather than written in Z.
They are ordinary calls, so they read the same way:

```csharp
print(abs(0 - 7));            // 7
print(min(3, 9));             // 3
print(max(3, 9));             // 9
print(clamp(15, 0, 10));      // 10   (x, low, high)
print(sqrt(17));              // 4    (integer square root, rounded down)
```

`abs`, `min`, `max` and `clamp` compile down to a couple of instructions each.
`sqrt` calls into the runtime, whose integer root is exact for every input
rather than merely close.

Because Z has no floating point type, `sin` and `cos` are integer-only. **A full
turn is `1 << 30` units** and the result is Q30, so `1.0` is exactly `1 << 30`:

```csharp
const int TURN = 1 << 30;
const int ONE = 1 << 30;         // 1.0 in Q30
print(cos(0));            // 1073741824   exactly 1.0
print(sin(TURN / 4));     // 1073741824   a quarter turn: also 1.0
print(cos(TURN / 2));     // -1073741824  a half turn: exactly -1.0

/* sin of a whole turn is mathematically 0, but the fixed-point result can be
 * off by an ulp or two, so compare with a tolerance rather than == 0. */
print(abs(sin(TURN)) < 8 ? 1 : 0);   // 1
```

Angles outside one turn wrap, and the result is exactly periodic, so you can
pass a raw counter without normalizing it first. They are computed with an
integer CORDIC — no `libm`, no floating point, and the same answer on every
machine. Accuracy is a little under 7 ulp of Q30 (about 6e-9) over a full turn,
which is far more than a game needs and far better than a lookup table.

A user function of the same name shadows a built-in, so you can define your own
`min` if you prefer.

---

## 6. String interpolation and concatenation

The `+` operator concatenates strings (an `int` or `bool` operand is converted
automatically). The `$"..."` form interpolates expressions inline:

```csharp
var name = "World";
var n = 3;
print("count = " + 42);            // count = 42
print("name: " + name);           // name: World
print($"Hello, {name}!");          // Hello, World!
print($"n={n}, n*n={n*n}");        // n=3, n*n=9
print($"bool: {true}");            // bool: true
```

Nested quotes inside an interpolation work: `print($"a {"b"} c");`.

### Comparing strings

All six relational operators work on strings, ordered lexicographically — the
same ordering C's `strcmp` gives, so a shorter string that is a prefix of a
longer one sorts first.

```csharp
print("hello" == "hello" ? 1 : 0);   // 1
print("hello" == "world" ? 1 : 0);   // 0
print("apple" < "banana" ? 1 : 0);   // 1
print("ab" < "abc" ? 1 : 0);         // 1
print("B" < "a" ? 1 : 0);            // 1  (byte order, so uppercase sorts first)

if ("q" > "p") { print("yes"); }     // usable as a condition
```

Comparison is by byte value, not by any notion of alphabetical order, and it is
case-sensitive.

---

## 7. Control flow

### Conditionals

```csharp
var x = 10;
if (x > 5) {
    print("big");
} else {
    print("small");
}
```

The ternary operator is available too:

```csharp
var x = 10;
print(x > 5 ? "big" : "small");   // big
```

### `while`

```csharp
var i = 0;
while (i < 3) {
    print(i);
    i = i + 1;                    // 0, 1, 2
}
```

### `for`

`for` is C#-style: `init; condition; step`.

```csharp
for (var i = 0; i < 5; i++) {
    print(i);                     // 0 1 2 3 4
}
```

### `foreach`

`foreach` iterates an array. The loop variable may be typed or bare:

```csharp
var a = new int[3];
a[0] = 10; a[1] = 20; a[2] = 30;
foreach (var v in a) { print(v); }   // 10 20 30
foreach (v in a) { print(v); }       // also fine (type inferred)
```

### `break` and `continue`

`break` leaves the innermost loop; `continue` starts the next iteration.

```csharp
var i = 0;
while (true) {
    i = i + 1;
    if (i > 5) { break; }        // stops the loop entirely
}
print(i);                        // 6

var s = 0;
for (var k = 0; k < 10; k++) {
    if (k < 3) { continue; }     // skips the rest of this iteration...
    s = s + k;                   // ...so this only runs for k >= 3
}
print(s);                        // 42
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
print(fib(15));   // 610
```

A single-expression function can use `=>` (expression-bodied):

```csharp
int square(int n) => n * n;
print(square(7));   // 49
```

### The entry point

You can write a program two ways. Either use **top-level statements** (no
`main` needed), or define a function literally named `main`:

```csharp
// top-level style (used throughout this tutorial)
print("hi");
```

```csharp
// or explicit main
int main() {
    print("hi");
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
print(f(21));      // 42
```

Where a *type* is needed, write it as `fn(params) -> ret`:

```csharp
int apply(fn(int) -> int f, int v) { return f(v); }
print(apply(&dbl, 5));     // 10
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
print(fs[0](4));            // 8

struct Op { fn(int) -> int f; }
var op = new Op(&dbl);
print(op.f(3));             // 6
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
print(m(3));            // 8
c.bump(10);
print(m(0));            // 15   the receiver is bound, so mutation shows through
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

A function pointer is not a closure: it captures nothing. Z has no nested
functions, so there is nothing for a closure to capture from.

---

## 9. Arrays

Arrays live on the heap. Create with `new T[n]`, read/write with `[]`, and get
the length with `.length`:

```csharp
var a = new int[5];       // 5 zeros
a[0] = 10;
a[1] = 20;
print(a.length);          // 5
print(a[0] + a[1]);       // 30
```

Combine with a loop:

```csharp
var squares = new int[5];
for (var i = 0; i < 5; i++) { squares[i] = i * i; }
foreach (var s in squares) { print(s); }   // 0 1 4 9 16
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
    print(triple(4));     // 12, from lib/math.z
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
print(*p);      // 10
*p = 99;
print(x);       // 99
```

Pointers are useful for mutating through a reference, and for passing
by-reference parameters:

```csharp
void setTo(int* q, int v) { *q = v; return; }
int n = 5;
setTo(&n, 42);
print(n);       // 42
```

Pointers nest, so a pointer to a pointer (`int**`) is a type you can write:

```csharp
var a = new int[2];
a[0] = 7;
var p = &a[0];           // int*
var t = &p;              // int**
print(**t);              // 7
```

`&a[0]` is how you get a pointer to an array's first element. Passing an array
itself where a pointer is expected is a type error — arrays do not decay the way
they do in C.

### `null`

`null` is the null pointer. It can be compared against any pointer, and assigned
into any pointer slot:

```csharp
var p = null;
if (p == null) { print("unset"); }        // unset
var q = &a[0];
if (q != null) { print("set"); }          // set
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
print(p.x + p.y);          // 7

var q = p;                  // value copy
q.x = 10;
print(p.x);                // 3   (p is unchanged)
print(q.x);                // 10
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
print(pt.Sum());     // 7
print(pt.Mag2());    // 25
```

To pass a struct to another method, use a pointer parameter `Type*` and call
with `&`:

```csharp
struct Point { int x; int y; int Dot(Point* o) { return x * o.x + y * o.y; } }
var a = new Point(1, 2);
var b = new Point(3, 4);
print(a.Dot(&b));   // 11
```

### Auto-properties

`int Id { get; set; }` creates a property backed by a hidden field:

```csharp
struct Acct { int id; int Id { get; set; } }
var acc = new Acct(0);
acc.Id = 9;
print(acc.Id);   // 9
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
print(a + b);          // (4, 6)   -> "+" returns a Vec, print shows it via Show()
print(a * 3);          // (9, 12)
print(a == b);         // false
```

### Extension methods

Add a method to an existing type by marking the first parameter `this`:

```csharp
int Twice(this int n) { return n * 2; }
string Exclaim(this string s) => s + "!";
bool IsEven(this int n) => n % 2 == 0;

print(21.Twice());      // 42
print("hi".Exclaim());  // hi!
print(4.IsEven());      // true
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

print(area(Circle(5)));   // 75
print(area(Rect(3, 4)));  // 12
print(area(Point));       // 0
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
print(describe(Rect(3, 4)));   // 3x4 rect
```

---

## 13. Classes (reference types + vtables)

A `class` is heap-allocated and supports **inheritance** and **dynamic
dispatch** through a vtable.

```csharp
class Animal {
    int legs;
    Animal(int l) { this.legs = l; }              // constructor
    virtual string Speak() { return "..."; }     // virtual
    int Legs() { return this.legs; }              // non-virtual
}

class Dog : Animal {                              // inherits Animal
    string name;
    Dog(int l, string n) { base(l); this.name = n; }   // call base ctor
    override string Speak() { return this.name + " woof"; }
}

var d = new Dog(4, "Rex");
print(d.Speak());   // Rex woof   (dynamic dispatch)
print(d.Legs());    // 4          (inherited non-virtual method)
```

- `virtual` introduces a dispatch slot; `override` replaces a base virtual in
  the same slot.
- Calling a virtual method picks the implementation from the object's *actual*
  runtime class, so it works through a base-typed reference too:

```csharp
var animals = new Animal*[2];      // array of base-class pointers
animals[0] = new Dog(4, "Rex");
animals[1] = new Cat(4);
print(animals[0].Speak());   // Rex woof
print(animals[1].Speak());   // meow
```

- A derived-class pointer may be assigned to a base-class pointer (upcast).
- `new C(args)` allocates a garbage-collected object and runs the constructor
  (a method named like the class). Fields start zeroed.

---

## 14. Generics

Generic *functions* are compiled by monomorphization: each concrete set of type
arguments produces a specialized, natively compiled copy. Type arguments are
inferred at the call site.

```csharp
T max<T>(T a, T b) { if (a > b) { return a; } return b; }
U pick<T, U>(T a, U b) { return b; }          // two type parameters
int total<T>(T[] xs) { var s = 0; foreach (v in xs) { s = s + v; } return s; }

print(max(3, 7));      // 7
print(max(10, 4));     // 10
print(pick(3, 9));     // 9

var arr = new int[4]; arr[0]=1; arr[1]=2; arr[2]=3; arr[3]=4;
print(total(arr));     // 10
```

The declaration must appear before its uses. There is no runtime generic
machinery, boxing, or type erasure — every instantiation is real native code.

---

## 15. Memory & the garbage collector

The heap (arrays, `new` objects, and strings) is managed by a conservative
mark-sweep collector. You never free anything:

```csharp
var live = "survivor";
for (var i = 0; i < 100000; i = i + 1) { live = live + "x"; }  // tons of garbage
print(live);   // "survivorxxx..." — `live` survived
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

## 16. A complete program

Putting many features together — classes, generics, enums, operator
overloading, and control flow:

```csharp
class Shape {
    virtual int Area() { return 0; }
    virtual string Name() { return "shape"; }
}
class Square : Shape {
    int side;
    Square(int s) { this.side = s; }
    override int Area() { return this.side * this.side; }
    override string Name() { return "square"; }
}

enum Op { Add(int a, int b), Mul(int a, int b) }
int apply(Op o) => match o { Add(x, y) => x + y, Mul(m, n) => m * n };

T maxOf<T>(T a, T b) { if (a > b) { return a; } return b; }

int main() {
    var shapes = new Shape*[2];
    shapes[0] = new Square(5);
    shapes[1] = new Square(3);

    for (var i = 0; i < 2; i++) {
        print($"{shapes[i].Name()} area = {shapes[i].Area()}");
    }
    // square area = 25
    // square area = 9

    print(apply(Add(2, 3)) + " " + apply(Mul(4, 5)));   // 5 20
    print(maxOf(7, 9));                                 // 9
    return 0;
}
```

---

## 17. Calling C from Z

Z can call C, and C can call Z. Both directions are declared in the Z source,
and no assembly rewriting is needed.

### `extern` — implemented in C

`extern` declares a function whose body lives in C. The symbol is used exactly
as written, and nothing is emitted for it:

```csharp
extern int c_add(int a, int b);
extern int c_strlen(string s);

int main() {
    print(c_add(2, 3));         // 5
    print(c_strlen("hello"));   // 5
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

## 18. Things to know

- **No floats** — integers only (`int` is 64-bit signed). `sqrt`, `sin` and
  `cos` are integer built-ins; trig uses a full turn of `1 << 30` and Q30
  results.
- **No hex literals.** Write decimal, or build the value with shifts (`1 << 20`).
- **Bitwise operators wrap** on overflow, matching C on a 64-bit `int`.
- **Structs are values, classes are references.** Assigning or passing a struct
  copies it; a class variable holds a pointer to a heap object.
- **`foreach` loop variables** must be `var x`, a typed name (`int x`), or a
  bare identifier (`x`) — all infer the element type.
- **Match-arm bindings** share one scope per `match`, so reuse distinct names
  across arms.
- **Compile-time errors** (type errors, non-exhaustive `match`, undefined names)
  are reported with the file, line, column, and a caret.
- **A function takes at most 6 parameters** (5 if it returns a struct, which
  spends one on the result buffer). Exceeding it is a compile error rather than
  silently wrong code.
- **Array indexing is unchecked** unless you pass `--bounds`. See section 15.
- **A `const` must be declared before use**, unlike a function.
- **`break`/`continue` outside a loop** is a compile error.
- **Structs cannot be returned by value across the C boundary.** See section 17.
- **`import` splices, it does not isolate.** An imported name is visible
  everywhere, with no namespace to qualify it.
- **Function pointers are not closures.** `&f` captures nothing, and Z has no
  nested functions, so there is no closure type. A `method(...)` pointer binds a
  receiver but still captures nothing.
- **`method(...)` and `fn(...)` are not interchangeable.** One holds a code
  address; the other holds a `{ code, receiver }` binding, and a call through it
  spends an extra argument register on the receiver.

---

## 19. Where to go next

- `docs/LANGUAGE.md` — the full language specification / grammar.
- `README.md` — project overview, architecture, performance, and roadmap.
- `tests/cases/*.z` — a large set of runnable example programs.
- `src/` — the compiler source (lexer → parser → type checker → x86-64 codegen).

Happy coding!
