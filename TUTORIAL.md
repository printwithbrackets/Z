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

**Note on division:** division and modulo by a *constant* are strength-reduced
by the compiler into a multiply–shift (much faster than a hardware divide), so
`x % 1000000007` in a hot loop is cheap.

---

## 5. String interpolation and concatenation

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

---

## 6. Control flow

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

---

## 7. Functions

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

---

## 8. Arrays

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

---

## 9. Pointers

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

---

## 10. Structs (value types)

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

## 11. Enums and pattern matching

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

## 12. Classes (reference types + vtables)

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

## 13. Generics

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

## 14. Memory & the garbage collector

The heap (arrays, `new` objects, and strings) is managed by a conservative
mark-sweep collector. You never free anything:

```csharp
var live = "survivor";
for (var i = 0; i < 100000; i = i + 1) { live = live + "x"; }  // tons of garbage
print(live);   // "survivorxxx..." — `live` survived
```

Locals live on the stack (no GC cost); only heap allocations are collected.

---

## 15. A complete program

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

## 16. Things to know

- **No floats** yet — integers only (`int` is 64-bit signed).
- **Structs are values, classes are references.** Assigning or passing a struct
  copies it; a class variable holds a pointer to a heap object.
- **`foreach` loop variables** must be `var x`, a typed name (`int x`), or a
  bare identifier (`x`) — all infer the element type.
- **Match-arm bindings** share one scope per `match`, so reuse distinct names
  across arms.
- **Compile-time errors** (type errors, non-exhaustive `match`, undefined names)
  are reported with the file, line, column, and a caret.

---

## 17. Where to go next

- `docs/LANGUAGE.md` — the full language specification / grammar.
- `README.md` — project overview, architecture, performance, and roadmap.
- `tests/cases/*.z` — a large set of runnable example programs.
- `src/` — the compiler source (lexer → parser → type checker → x86-64 codegen).

Happy coding!
