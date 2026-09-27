# Z

A small, statically-typed, C#-flavored language that compiles to native x86-64
machine code. The compiler itself is written in C11 with no dependencies
beyond a C toolchain.

**Status: M0–M6c done (generics, classes/vtables), M7 optimizations in progress.** A working end-to-end compiler: source → lexer →
parser → type checker → x86-64 assembly → native binary. It has a real type system,
pointers, heap arrays, structs with methods and properties, `for`/`foreach`,
the C#-style sugar layer (string interpolation, expression-bodied members,
struct methods/properties, extension methods, operator overloading, ternary),
enums with exhaustive pattern matching, monomorphized generics, classes with
vtables and inheritance, a tracing garbage collector for the heap, IEEE-754
`float`, closures, nested functions, interfaces, a standard library, warnings, DWARF
debug info, and a register-allocating, constant-folding, strength-reducing
backend. See
[TUTORIAL.md](docs/TUTORIAL.md) to learn the language and [Roadmap](#roadmap) for
what's next.

## Build & test

```sh
make          # build ./z
make test     # golden + diagnostic + warning test suite at -O1
make test-all # the same suite at -O0, -O1, -O2 and -O3
make clean
```

Requires `cc` (gcc or clang) and a Unix `as`/`ld` via the system toolchain.
Code is compiled with `-std=c11 -Wall -Wextra -Wpedantic -Werror`.

## Usage

```sh
./z run   hello.z          # compile to a temp binary and run it
./z build hello.z -o hello # compile to ./hello
./z asm   hello.z          # print the generated x86-64 assembly
./z build hello.z -o hello -g   # ...with DWARF, for gdb
```

Optimization levels are spelled like gcc's (`-O0`..`-O3`), and `-O2` turns on
loop-invariant code motion. `--bounds` range-checks array indexing, `-w`
silences warnings, and `-Werror` makes them fail the build. Anything the
compiler does not recognize is passed through to the linker.

## Editor support

`vim/` holds Vim/Neovim runtime files: `syntax/z.vim` (highlighting, including
`$"..."` interpolation holes), `ftplugin/z.vim` (comments, formatting and a
brace/`match`-aware indent), `ftdetect/z.vim` (`*.z`) and `compiler/z.vim`, so
`:make` builds the current file and puts compiler errors in the quickfix list.
Point any plugin manager at that directory, e.g. with lazy.nvim:

```lua
{ dir = "/path/to/z/vim" }
```

## Quick tour

```csharp
// Top-level statements — no main() boilerplate.
var total = 0;
for (var i = 1; i <= 10; i++) {   // C#-style for + ++
    total += i;
}
print(total);                        // 55

// Arrays live on the heap; index with [], read .length.
var a = new int[5];
a[0] = 10;  a[1] = 20;
print(a.length);                     // 5
print(a[0] + a[1]);                  // 30

// foreach desugars to an index-based loop.
foreach (var x in a) { print(x); }   // 10 20 0 0 0

// Pointers: & to take an address, * to deref.
var p = &a[0];
print(*p);                           // 10
*p = 99;
print(a[0]);                         // 99

// Functions (C#-style: return type, then name).
int fib(int n) {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}
print(fib(15));                       // 610

// Structs (value types with fields).
struct Point { int x; int y; }
var p = new Point(3, 4);        // positional constructor
var q = p;                      // value copy
q.x = 10;                       // q.x = 10, p.x still 3
print(p.x + p.y);               // 7
print(q.x);                     // 10

// Methods with an implicit `this` (C# style).
struct Point { int x; int y; int Sum() { return x + y; } int Mag2() => x*x + y*y; }
var pt = new Point(3, 4);
print(pt.Sum());                // 7
print(pt.Mag2());               // 25

// Auto-properties and string interpolation.
struct Acct { int id; int Id { get; set; } }
var acc = new Acct(0); acc.Id = 9;
print($"id = {acc.Id}");         // id = 9

// Extension methods.
int Twice(this int n) { return n * 2; }
print(21.Twice());               // 42

// bool operators and string concatenation (incl. int→string).
bool ok = 3 > 2 && !false;
print(ok);                            // true
print("count = " + 42);               // count = 42
```

## How it works

The compiler is a classic multi-pass pipeline, each pass in its own module:

| Pass | File | Role |
|------|------|------|
| Lexer | `src/lexer.c` | source → tokens (spans, comments, string interning) |
| Parser / checker | `src/parser.c` | recursive descent + precedence climbing; resolves names, type-checks, builds a typed AST; desugars `foreach`/`++` |
| Types | `src/types.c` | pointer-based `Type` graph, sizes/alignment, struct defs |
| Codegen | `src/codegen.c` | typed AST → x86-64 assembly (System V AMD64 ABI), lvalue/rvalue |
| Driver | `src/main.c` | orchestrates passes, embeds + links the runtime, invokes `cc` |
| Runtime | `runtime/z_rt.c` | conservative mark-sweep GC + heap array alloc, string concat/itoa; embedded in the binary |
| Types | `src/types.c` | pointer-based `Type` graph: scalars, pointers, arrays, structs, tagged unions, type params |
| Arena | `src/arena.c` | bump allocator — all compiler memory freed in one call |
| Diag | `src/diag.c` | `file:line:col: error: …` with a caret under the token |

Code generation uses an lvalue/rvalue split: `gen_addr` computes the address of
a variable/index/deref, `gen_expr` loads through it. Operands are staged through
rbp-relative temporary slots; calls follow the System V integer-argument
registers (`rdi, rsi, rdx, rcx, r8, r9`). Output is text assembly handed to the
system assembler — we do not write an ELF encoder.

## Roadmap

- **M0–M1 (done):** pipeline, expressions, control flow, functions, `int`/`bool`/`string`, `var`, `print`, diagnostics.
- **M2 (done):** real type system, pointers (`&`/`*`), heap arrays (`new T[n]`, indexing, `.length`), `for`/`foreach`, `++`/`--`, string concatenation/`int`→string, **structs** (fields, nested, copy semantics, arrays of structs, by-pointer params), embedded runtime. **Later:** by-value struct pass/return (SysV classifier).
- **M3 (done):** C# sugar: properties, `$""` string interpolation, expression-bodied members, `operator` overloading, extension methods.
- **M3.5 (done):** the everyday-language layer — `const`; bitwise `& | ^ ~ << >>` and the compound forms; `break`/`continue`; the `null` literal; lexicographic string comparison; integer built-ins `abs min max clamp sqrt` and `sin`/`cos` (fixed point, a full turn of `1 << 30`); opt-in `--bounds` range checking.
- **M3.7 (done):** C interoperability — `extern` (implemented in C) and `export` (defined in Z, callable from C) in both directions, with linker arguments passed through. Every Z function is emitted under a private `z$` symbol, so it can no longer collide with a libc name, a runtime helper, or a word the assembler reserves.
- **M3.8 (done):** first-class function pointers — `&f` yields a value typed by
  the function's signature, `fn(params) -> ret` names the type, and calls through
  a pointer are argument-checked.
- **M3.11 (done):** **closures** — `(params) => expr` or `(params) => { ... }` as
  an expression, capturing anything in scope where it is written. A closure is a
  GC cell of `{ code, env }`; captured variables are boxed, so a closure that
  outlives the frame it was written in keeps working and every closure over the
  same variable sees each write. A function returning one declares
  `closure(params) -> ret`, kept distinct from `fn` because the hidden
  environment changes the calling convention. One nesting level: a lambda may
  not be written inside another lambda.
- **M3.12 (done):** **nested functions** — a function declared inside another
  one, written where a statement goes. Hoisted out and emitted under a symbol
  derived from the enclosing function, so the same name in two bodies does not
  collide; recursive, since its signature is filed as it is read. It does *not*
  capture: it has its own frame, and frame slots are numbered per function, so
  reading a variable declared beside it would read the wrong slot. That rule now
  applies at the top level too, where a function reading a top-level variable
  used to compile and silently return 0.
- **M3.9 (done):** modules — `import "path.z";` splices a file's declarations
  into the importing one, resolved relative to it, de-duplicated, cycle-checked,
  and with per-file diagnostics.
- **M3.10 (done):** bound method pointers — `&obj.M` yields a `method(...)`
  value, a pointer to a GC cell holding `{ code, receiver }` so the receiver
  stays alive; a virtual method binds through the vtable. Scalar representation,
  so no aggregate copy machinery is involved.
- **M4 (done):** `Result<T,E>` + `?`. A `Result` is a two-variant union, so `match`,
  payload binding and the exhaustiveness check all apply to it. `?` on a
  `Result` inside a function returning a `Result` with the same error type yields
  the `Ok` payload and returns the `Err` from the function, so a chain of
  fallible calls reads as straight-line code. Both parameters must be one word,
  which keeps every `Result` the same sixteen bytes and makes the propagated
  error a copy rather than a conversion.
- **M5 (done):** tracing GC — a conservative mark-sweep collector in the runtime (scans the C stack + spilled registers), triggered on heap growth; keeps live data, reclaims garbage.
- **M6a (done):** enums / sum types (tagged unions) + **exhaustive** `match` with payload binding (compile error if a variant is unhandled).
- **M6b (done):** generic functions via monomorphization (type inference, `T`/`T[]`/`T*` params, struct returns; no runtime generics). **Interfaces/traits** remain.
- **Match arms shared one scope,** so two arms binding the same payload name collided. Each arm is now its own scope, and `_` is accepted as a binding that is deliberately not read.
- **M6d (done):** **interfaces** — a named set of method signatures that a type
  satisfies simply by having them, so a `struct` (which has no vtable) and a
  `class` and a subclass of a different hierarchy can sit in one array and be
  called through. An interface value is a pointer to a `{ itab, receiver }` cell,
  so it passes, stores and compares like a pointer and `null` means something.
  The itab is a static array of code pointers in the interface's declaration
  order; a struct's entries point straight at its methods, a class's are
  trampolines through the vtable so a subclass still calls the override.
- **M6c (done):** opt-in `class` with vtables — inheritance, `virtual`/`override`, constructors + `base()`, `new C()` heap objects, polymorphic dynamic dispatch.
- **M7 (done):** optimizations + register allocation. Done: compile-time
  constant folding & propagation; **function inlining**; a liveness-based
  **local register allocator**;
  leaf- and immediate-operand binary ops (no temp round-trip); direct
  register/immediate compares and branch-on-flags for conditions; in-place
  compound assignment; **constant division/modulo strength reduction**
  (Granlund–Montgomery multiply-shift replacing 64-bit `idiv`, with a
  compile-time self-check that falls back to `idiv` if a magic can't be proven);
  and **loop-invariant code motion** at `-O2`. Together these turn hot loops
  fully register-resident and ~2× faster end to end, and beat `gcc -O0` on
  modulo-heavy code. Each `for` phase is a separate liveness position, so a loop
  counter can no longer share a register with a local declared in its body.
  `-O3` adds **loop unrolling**: a loop with a condition and a body of at most 24
  statements is emitted four times over, testing the condition before each copy
  so the body still runs exactly as many times as it did rolled. The last copy
  branches back to the top and the rest fall through, so three-quarters of the
  loop-back branches go and consecutive iterations sit together for the
  prefetcher; each copy carries its own continuation label, so `continue` runs
  the step of the copy it is in and `break` leaves the loop from any of them.
  `mathbench` is ~3x faster at `-O3` than at `-O2`.
  **Function inlining** runs from `-O1` up: a call to a same-file function is
  replaced by its body, as an expression when the body is a single `return` and
  as statements otherwise, following nested calls up to four levels deep. The
  body's locals take slots in the caller's frame, so the callee's frame, its
  argument setup and its call and return all go away. A call carrying an
  argument that might act or cost something (`f().add(3)`, `1/den`) is left
  alone, since substitution is textual and would repeat it. So is anything whose
  meaning depends on the frame it was written in -- a body containing a closure
  or a nested function, whose environment names that frame's slots -- along with
  recursion, externs, aggregate parameters and results, and bodies over 24
  statements.
  A **counted loop whose body only accumulates loop-invariant amounts** is
  solved rather than run: `while (i < 20000000) { sum = sum + 82; i = i + 1; }`
  becomes `sum = sum + 82 * 20000000`. This is the limit of what LICM reaches on
  its own -- it hoists the pieces, and this notices there is nothing left to run.
  It runs as a parser-level tree rewrite, so the register allocator, the folder
  and the strength reducer all see the finished statement and optimize it again.
  The pattern is narrow on purpose (constant stride, constant bound, integer
  accumulate only, no call or early exit), because a wrong rewrite is a wrong
  answer rather than a missed speedup. `loopbench` goes from 24 ms to 4 ms, which
  is within 1.3x of gcc -O2 on a loop both compilers delete entirely.
  An operand that is *already* in a register is now named directly rather than
  copied into a scratch register first, so `a + b` with both in registers is two
  instructions instead of three. Naming the operand's own register is safe for
  every operator including the shifts, and correct when both sides happen to be
  the same register (`add rbx, rbx`).
  Remaining: full three-address evaluation, which only shows up when an operand
  is itself a call — that still needs a temp, because its value has to exist
  before the operator can read it.
- **M9 (done):** **`float`** — IEEE-754 binary64, in XMM registers. Literals
  (`1.5`, `1e3`, `2E-2`), arithmetic, IEEE-correct comparison (a NaN is false
  against everything, including itself), explicit casts, float parameters and
  return values under the System V ABI, float fields, and C interop where Z's
  `float` is C's `double`. `int` widens to `float` implicitly because that
  conversion is lossless; the reverse is never implicit. Float locals stay in the
  frame rather than joining the general-purpose register pool, which costs a
  load and a store per access and buys not having to teach every optimizer pass
  about a second register class.
- **M8 (done):** developer experience — warnings (`unused-local`,
  `shadowed-local`, `unreachable`) with `-w`/`-Werror`/`-Wno-<name>`; a standard
  library (string and integer built-ins, type-checked like ordinary calls);
  literals in every radix with `_` separators plus `\xNN`/`\uXXXX` escapes;
  and DWARF debug info behind `-g`, so a debugger can break on a line, walk a
  backtrace, and read parameters.

### Planned

Five gaps, roughly in the order they start blocking real programs. Each one is
a design question before it is an amount of work; the notes say what the
question is, because picking the wrong answer is the expensive way to find out.

- **M10 (planned): collections beyond fixed arrays.** A `T[]` is a pointer, a
  length and a GC-traced allocation: fixed, append-only by hand, and the only
  aggregate container in the language. What is missing is everything a program
  actually reaches for — a growable vector, a map, a set, and slices so a
  function can take part of an array without copying it.

  The decision is value or reference. A `Vec<T>` as a `class` is one pointer,
  copied cheaply and shared on assignment, which is what everyone expects from a
  growable buffer and what makes a `Vec<Vec<T>>` behave. As a `struct` holding
  a pointer it is also one pointer, but assignment aliases the same buffer in
  two variables, which is a trap that a language with value-semantics structs
  invites badly. The language already draws this line — structs copy, classes
  share — so the answer is probably `class`, and the interesting work is making
  that choice legible rather than making the container.

  The second decision is the iteration protocol, and interfaces (M6d) have
  already answered it: a `struct` satisfies `interface Iterator<T>` by having
  `next() -> T?`, and `foreach` over a `Vec<T>` is sugar for a `foreach` over an
  iterator. That is the reason to do this after interfaces and not before — the
  collection needs nothing invented for it.

- **M11 (planned): file and I/O beyond `print`.** The language can print to
  stdout and nothing else. A program that cannot read a file cannot do anything
  worth writing, so this blocks more than its line count suggests.

  The design question is errors. `open` fails, `read` fails at EOF, a write
  fails on a full disk, and each of those is a real outcome rather than an
  exceptional one. `Result<T,E>` (M4) is the right shape, so
  `Result<File, IoError>` and `?` in a function returning it, with the payload
  carrying the errno and the path — the error type is where the usefulness is.
  A line-oriented API (`readLine`) is what most programs want and is also where
  the buffering decision lives: read whole, or read a chunk and split, and
  whether a `File` owns a buffer such that copying one is a bug.

- **M12 (planned): a string type that is not just concatenation.** `string` is
  currently a NUL-terminated `char*` with `+` for concatenation, so it has no
  length, cannot hold an embedded zero, and every append reallocates and copies
  everything. That is a C string wearing a nicer name, and it is the single
  most common thing a Z program gets wrong.

  The fix is a real value: a pointer plus a length, passed as two registers,
  comparing and hashing by content. Whether it is a `struct` (copied by value,
  which is 16 bytes and usually what you want) or stays a pointer (one word,
  but a `string` variable can be reassigned and every function takes it
  indirectly) is the question. Value semantics is the one that makes
  `s = s + "x"` correct without surprise, and Z already has the machinery for a
  16-byte value parameter. What it costs is every ABI interaction: a `string` in
  an exported function, one in a C caller, one crossing the GC boundary.

  Slice and search come with it — `s[2..5]`, `indexOf`, `contains`, `split`,
  `join` — and so does the question of what indexing means. Bytes or characters?
  UTF-8 makes those different answers, and a language that has `\uXXXX` escapes
  and no character type has already decided something it has not said out loud.

- **M13 (planned): package and module distribution.** `import "path.z"` splices
  a file's declarations into the importing one. That is fine for one directory
  and does not survive contact with anything else: there is no namespace, so
  two imported files cannot both define `helper`; nothing is private, so
  everything a library declares is a name its users must not collide with; and
  there is no version, no manifest, and no way to say where a dependency lives.

  The order matters. Visibility first (`pub`, with everything unexported by
  default) and real module scoping, because both are compiler work and both
  change what existing programs mean. Then a manifest naming dependencies with
  version constraints, a resolver, and a registry or a vendored path. The
  standard library should end up as packages, written in Z, so it is tested by
  the same machinery every other library is.

  The trap is doing the registry before the scoping. A package manager over a
  language whose imports all share one global namespace distributes name
  collisions instead of fixing them.

- **M14 (planned): error message quality.** Diagnostics carry a file, line,
  column and a caret, and the error tests pin their text — but the messages are
  written to be *correct* rather than to be *read*. A reader who mistypes a
  name gets "undefined name 'foo'" and no idea that `foo` is three characters
  away; one who passes the wrong type gets the type mismatch and nothing about
  which argument was wrong or what was expected there.

  Concretely: a "did you mean" suggestion from edit distance over the names in
  scope, which is a few lines over a symbol table the compiler already builds;
  the name of the function or method the error is inside, so a message in a
  hundred-line body is locatable; the *constraint* that was violated rather
  than only the violation ("a `struct` cannot be returned by value across the C
  boundary" is the model — it says what to do); and a note when the failing call
  is inside a loop, a lambda, or a macro-like expansion, where the span points
  at generated text. Colour on a tty, none when redirected or piped, and a
  machine-readable format so an editor can put the squiggle under the right word
  without scraping prose.

### Known miscompiles found and fixed

Recorded because each was invisible at the default optimization level, and
`make test` only ran `-O1`:

- **LICM hoisted expressions out of the loop that varied them.** The pass was
  handed a loop's body but never its step, so an induction variable looked
  invariant and `p * 2` was computed once, before the loop, and reused for every
  iteration. `nested_loops` *hung* at `-O2` and was correct at `-O1`. The pass
  now takes the step, and `make test-all` runs the whole suite at every level so
  a pass that only runs at a higher level cannot hide again.
- **`foreach` over a `T*` compiled and read unrelated memory.** The parser
  accepted a pointer, the desugaring always read `.length`, and a pointer has no
  length header — so the pointer's own address became the iteration count. It is
  now a diagnostic.
- **`\{` in an interpolated string was an error.** The lexer decoded the escape
  to a bare brace, which the parser then read as a hole delimiter.
- **A constructor or virtual method with a `float` parameter read the receiver's
  address as a double.** Both are calls with a hidden first argument, so their
  declared arguments arrive in the integer sequence — but the call site staged
  every argument with `gen_expr`, which leaves a float in `xmm0` and saved `rax`
  instead. The value came back as a denormal around `4.8e-315`. Float arguments
  are now staged with `gen_float`; a plain call still uses the ABI's two
  independent register sequences.
- **A call with two or more `float` arguments passed them all in the same
  register.** The argument assignment filled in a register *name* by pointing at
  a `char[8]` that went out of scope at the end of the loop iteration, so every
  float read back whichever name that stack slot happened to end up holding --
  `f(1.5, 2.0)` passed both in `xmm1`, and `add3(1.5, 2.0, 2.5)` returned 7.5
  instead of 6. Register names now come from a table, which is also how they
  should have been written the first time.
- **A virtual call called the receiver instead of the method.** The resolved
  target has to survive the argument placement, which uses `r11` as its scratch,
  so it is stashed in a frame temp and reloaded. The stash saved the *receiver*
  rather than the address in `r11`, so every virtual call jumped into the object
  and crashed -- and every class with a `virtual` method was broken.
- **A union variant with a `float` payload stored the wrong register.** The
  payload was written from `rax` after `gen_expr`, but a float is left in an XMM
  register, so the variant carried whatever `rax` happened to hold and read back
  as a denormal built out of an unrelated register. A variant with a struct
  payload was wrong too: the binding was loaded as eight bytes rather than
  given the payload's address.
- **A type had to be declared before it was used.** The pre-scan that lets a
  signature name a struct registered only its *name*; the fields and the size
  were filled in by the real parser, in source order. So `new Pt(3, 4)` and
  `p.x` in a function written above `struct Pt` read a struct with no fields.
  The pre-scan now gives every struct, class and enum its full layout, and the
  real parser starts the field list over when it arrives.
- **A local could not be declared with a named type.** `P r = new P(3, 4);` was
  unreachable: a statement beginning with an identifier was reported as an
  unknown type *without a lookup*, so the one spelling a struct is used with was
  the one spelling that did not work — while `int x = 5;`, a keyword, went
  through a different path. `P* q = &r;` slipped past even that and was read as
  an expression, a variable named `P`. A statement that begins with a type is now
  resolved as a declaration, which also brought back `T r = a;` inside a generic
  function.
- **A function could read a variable declared outside it.** Frame slots are
  numbered per function, so `var g = 5; int f() { return g; }` resolved `g` and
  then read f's own frame: it compiled and printed 0. Name lookup now stops at a
  function body's outermost scope, which is also what makes a nested function
  non-capturing by construction.
- **The ninth queued function replaced the first eight with garbage.**
  `add_pending` grew its array by allocating a new block and never copying the
  old contents, so a program with eight lambdas worked and one with nine crashed
  the compiler rather than failing at the point of the bug. The generic
  instantiation path shared it.

## Performance

Benchmarks live in `tests/bench/`. Timings are wall-clock, min-of-20, on a noisy
shared machine — treat as relative, not absolute.

On a **compute-bound register loop** (`loopbench`), Z's hot loop is fully
register-resident — constants folded to immediates, the counter in a callee-saved
register, the condition a direct `cmp/jge`, no memory traffic — and runs ~6×
faster than its pre-M7 codegen. Note `gcc -O2` "beats" it only by *closed-form
solving* the loop at compile time (it never executes the iterations), so that is
not like-for-like.

On a **fair loop with a loop-carried dependency** (`mixbench`: `sum = (sum*31+i) % 1000000007`),
where the compiler must actually run the loop, and where constant-modulo
strength reduction matters:

| | time |
|---|---|
  | Z `-O2` | ~89 ms |
| `gcc -O2` | ~54 ms |
| `gcc -O0` | ~148 ms |

Z beats `gcc -O0` by ~1.7x and is ~1.6x off `gcc -O2`. The loop is already fully
register-resident and its hot loop body is 6 instructions; the residual gap is
the strength-reduced modulo, which is a multiply-high-and-subtract rather than a
divide, and the lack of an inliner.

On `loopbench`, Z is at parity with gcc: both delete the loop entirely, Z in
4 ms and gcc in 3 ms, neither of which executes the 20 million iterations. This loop is deliberately
unfriendly to vectorization, so the comparison is like for like -- both compilers
execute all ten million iterations.

## Design

See `docs/LANGUAGE.md` for the grammar and type rules, and the design
rationale (focus, constraints, memory model) in the project docs.

## License

MIT. See [LICENSE](LICENSE).
