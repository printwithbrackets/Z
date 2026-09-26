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
vtables and inheritance, a tracing garbage collector for the heap, and a
register-allocating, constant-folding, strength-reducing backend. See
[TUTORIAL.md](docs/TUTORIAL.md) to learn the language and [Roadmap](#roadmap) for
what's next.

## Build & test

```sh
make          # build ./z
make test     # golden + diagnostic test suite
make clean
```

Requires `cc` (gcc or clang) and a Unix `as`/`ld` via the system toolchain.
Code is compiled with `-std=c11 -Wall -Wextra -Wpedantic -Werror`.

## Usage

```sh
./z run   hello.z          # compile to a temp binary and run it
./z build hello.z -o hello # compile to ./hello
./z asm   hello.z          # print the generated x86-64 assembly
```

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
  a pointer are argument-checked. Not closures: there are no nested functions.
- **M3.9 (done):** modules — `import "path.z";` splices a file's declarations
  into the importing one, resolved relative to it, de-duplicated, cycle-checked,
  and with per-file diagnostics.
- **M3.10 (done):** bound method pointers — `&obj.M` yields a `method(...)`
  value, a pointer to a GC cell holding `{ code, receiver }` so the receiver
  stays alive; a virtual method binds through the vtable. Scalar representation,
  so no aggregate copy machinery is involved.
- **M4:** richer checker, `Result<T,E>` + `?`.
- **M5 (done):** tracing GC — a conservative mark-sweep collector in the runtime (scans the C stack + spilled registers), triggered on heap growth; keeps live data, reclaims garbage.
- **M6a (done):** enums / sum types (tagged unions) + **exhaustive** `match` with payload binding (compile error if a variant is unhandled).
- **M6b (done):** generic functions via monomorphization (type inference, `T`/`T[]`/`T*` params, struct returns; no runtime generics). **Interfaces/traits** remain.
- **M6c (done):** opt-in `class` with vtables — inheritance, `virtual`/`override`, constructors + `base()`, `new C()` heap objects, polymorphic dynamic dispatch.
- **M7 (in progress):** optimizations + register allocation. Done: compile-time
  constant folding & propagation; a liveness-based **local register allocator**;
  leaf- and immediate-operand binary ops (no temp round-trip); direct
  register/immediate compares and branch-on-flags for conditions; in-place
  compound assignment; and **constant division/modulo strength reduction**
  (Granlund–Montgomery multiply-shift replacing 64-bit `idiv`, with a
  compile-time self-check that falls back to `idiv` if a magic can't be proven).
  Together these turn hot loops fully register-resident and ~2× faster end to
  end, and beat `gcc -O0` on modulo-heavy code. Each `for` phase is a separate
  liveness position, so a loop counter can no longer share a register with a
  local declared in its body. Remaining: loop-invariant code motion,
  three-address-code in-place evaluation, loop unrolling.

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
  | Z | ~95 ms |
| `gcc -O2` | ~59 ms |
| `gcc -O0` | ~108 ms |

Z now beats `gcc -O0` and is ~1.6× off `gcc -O2`; the residual gap is
loop-invariant code motion, three-address-code in-place evaluation, and loop
unrolling.

## Design

See `docs/LANGUAGE.md` for the grammar and type rules, and the design
rationale (focus, constraints, memory model) in the project docs.

## License

MIT (add a LICENSE file to formalize).
