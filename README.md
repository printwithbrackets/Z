# Z

A small, statically-typed systems language that compiles to native x86-64
machine code. The compiler itself is written in C11 with no dependencies
beyond a C toolchain.

**Version 0.1.1.** `z --version` reports it, and it is the tag the release is cut
from.

**Status: v2 slices 1 and 2 are built, and slice 3 has taken the collector out.**
A working end-to-end compiler: source → lexer → parser → type checker → x86-64
assembly → native binary. It has a real type system, pointers, heap arrays,
structs with methods and properties, `for`/`foreach` and the range-based `for`,
enums with exhaustive pattern matching, monomorphized generics, `Vec`/`Map`/`Set`,
classes with vtables, interfaces, a plain `malloc` allocator and a `free` to match
it, IEEE-754 `float`, closures, nested functions, a standard library, warnings,
DWARF debug info, and a register-allocating, constant-folding, strength-reducing
backend.

**Z has no inheritance.** `class B : A`, `override` and `base(...)` were cut in
slice 2, each replaced by a diagnostic that says to use an `interface` instead.
`class`, `virtual` and the vtable stay, because a class stored in an interface
is still dispatched through one.

**The collector is gone, and `string` is the first type it freed.** Slice 3
deleted `z_gc_init` and the whole mark-sweep collector and replaced `gc_alloc`
with a plain `z_alloc` over `malloc` plus a `z_free` to match it. A `string` now
owns its bytes: the compiler copies a borrowed string on every store, releases it
when its scope ends, and releases the previous value when a slot is overwritten.
`move` is how a value is handed on rather than shared, and reading a moved-from
variable is a compile error with a note pointing at the move.

A temporary string, meaning one that is computed and never stored into anything,
is released when the statement that made it ends, so `Console.WriteLog(a + b)`
inside a loop is bounded rather than one leak per iteration.

**Everything else still leaks, and that is stated rather than hidden.** A class
object from `new C()`, a heap array from `new T[n]`, a closure cell, and anything
reachable only through one of those, are all still live at exit. Leaks are the
safe direction to be wrong in, which is why the frees are wired up per type
rather than all at once.

Leaks are also the only thing the suite is clean of. All 106 golden cases, which
are the ones that produce a program to run, build and run under
AddressSanitizer with no use-after-free, no double free and no buffer overflow.
The rest of the 235 tests are diagnostics and gates, none of which runs a
program, so "the suite is clean under ASan" is a statement about those 106 and
not about the other 129. And it is a statement about the suite rather than about
the language:
a program outside it can release a captured string's cell instead of its string,
or hand an already-released field to a destructor. Both are known, and each is
written down next to the code that causes it.

**Destructors are half-built, and the half that is missing is not reachable by
accident.** A `~Type()` method runs when a value's scope ends, in reverse
declaration order. It works on scope end, on `return` (unwinding every open
scope, innermost first), per iteration in a loop body, and at the end of a void
function. It does **not** yet run on `?`, on `break` or `continue`, and it does
not tear down an owning field of a type with no destructor of its own.

**Returning a local that would be destroyed on the way out is now a compile
error, not a footgun.** `R* f() { var mine = new R(5); return mine; }` is
rejected:

```
error: cannot return 'mine' directly: its destructor would run before the caller gets it
note: 'mine' is declared here; write 'return move mine;' to hand it to the caller
```

and `return move mine;` hands it over instead, so the destructor runs once, in
the caller's scope. This covers a `class` pointer whose class has a destructor,
an array of one, and a struct that owns a field needing a drop. It does not fire
on a `string`, because a return already copies a borrowed string, nor on a return
that is not a bare identifier — `return new C(5);` and `return f();` have
nothing already-owned to hand back, nor on a **parameter**, which is a borrow
whose value is still the caller's and which this function never destroys.

That last exemption is the caller's problem, not the callee's. A returned
parameter hands back a handle that aliases the caller's own local, so the caller
has to `move` its value over at the call site:

```csharp
var r = new Tracked(1);
var h = relay(move r);      // one drop, in main's scope
```

Write `relay(r)` and both `r` and `h` drop at the end of `main`, which is the
plain-copy aliasing hazard every class handle has here and not something the
return rule introduced. `return_param` in the golden suite is the regression
test.

The language is being re-cast from C#-flavored to **C++-flavored but dumber**:
value semantics, references, RAII instead of a garbage collector, no
inheritance, and threads that cannot data-race because ownership makes sharing
unrepresentable. Slice 3 (ownership and RAII, which retires the collector) and
slice 4 (threads and channels) are what remain. See **[docs/LANGUAGE.md](docs/LANGUAGE.md)** for the v2
specification and the five slices it is built in; v1 is preserved verbatim in
**[docs/LANGUAGE-v1.md](docs/LANGUAGE-v1.md)**. To learn the language that
compiles *today*, read [docs/TUTORIAL.md](docs/TUTORIAL.md) and
[docs/LANGUAGE-v1.md](docs/LANGUAGE-v1.md); the [Roadmap](#roadmap) below is the
v1 history and what came of it.

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
./z --version              # print the compiler version and exit
```

Optimization levels are spelled like gcc's (`-O0`..`-O3`), and `-O2` turns on
loop-invariant code motion. Array *and string* indexing is range-checked by
default and `--no-bounds` turns it off, `-w` silences warnings, and `-Werror`
makes them fail the build. Anything the compiler does not recognize is passed
through to the linker.

`--no-bounds` also drops the null-pointer check on `*`, so it is not only a
throughput flag. With checks on, dereferencing a null pointer prints
`runtime error: null pointer dereference` and exits 134; with `--no-bounds` it is
a bare SIGSEGV and exit 139. The check covers the `*` operator only — `p.field`
through a null pointer is still a bare SIGSEGV either way.

```csharp
int main() {
    int* p = null;
    Console.WriteLog(*p);
}
```

A `.zignore/config.z` renames what a program writes for a global — the builtins,
the standard library, your own globals — so a codebase can be written in its own
dialect. It is found by walking up from the source file, and `--no-surface`
ignores it:

```sh
# .zignore/config.z:  say = print
./z run hello.z                  # a program in this project's dialect
./z run hello.z --no-surface     # the shipped names only
```

This is the same file that marks a project root, so a project that renames names
and a project that spans many files say so in one place.

The output path and those pass-through arguments are handed to the C toolchain
as a real argument vector, never through a shell, so a path containing a quote,
a semicolon or a space is just a path. There is a limit of 256 pass-through
arguments, and going past it is reported rather than quietly dropping the rest.

Diagnostics come in three shapes with `--error-format=human|gcc|json`, and take
colour on a terminal (`--color=auto|always|never`, and never under `NO_COLOR`).
`gcc` is one line per problem and per note, which is what an editor's error
parser wants; `json` carries a stable code, an explicit span and the notes
inline. `--no-std` compiles without the embedded standard library.

The standard library is Z source (`lib/*.z`), embedded in the compiler and
spliced ahead of every program -- so `StringBuilder` is a class, checked by the
same front end as your code, not a C function behind a builtin.

## Editor support

`vim/` holds Vim/Neovim runtime files. Point any plugin manager at that
directory, e.g. with lazy.nvim:

```lua
{ dir = "/path/to/z/vim" }
```

| file | what it does |
|---|---|
| `syntax/z.vim` | highlighting: every keyword in `KEYWORDS[]`, every radix with `_` separators, duration literals (`2h21m37`), destructor declarations, the builtins, the surface names, and `$"..."` interpolation holes |
| `ftplugin/z.vim` | comments, formatting, a brace/`match`-aware `indentexpr`, and `compiler z` |
| `ftdetect/z.vim` | `*.z` |
| `compiler/z.vim` | `makeprg` and `errorformat`, so `:make` and `:lmake` work |
| `plugin/z.vim` | the commands below |
| `autoload/z.vim` | shared helpers: running the compiler, and JSON diagnostics to quickfix |

### Errors and warnings

`:make` works, but it goes through `errorformat` and the one-line `gcc` error
format, which has no diagnostic code and flattens every note into the list as an
unrelated entry. `:Zcheck` reads `--error-format=json` instead, so each problem
carries its code and each note that names a span is a line you can jump to:

```vim
:Zcheck        " compile the current file, fill the quickfix list, open it
:Zcheck!       " same, without opening the window
:Zlmake        " into the location list instead of the quickfix list
:Zclearfix     " empty the list and remove the gutter signs
:Zsigns        " re-place the gutter signs from the current quickfix list
:Zversion      " print the compiler version
```

```
> probe.z|8| E| [unused-local] 'unused' is declared but never used
  probe.z|5| E| [return_owned_local] cannot return 'mine' directly: its destructor would run before the caller gets it
  probe.z|9| E| [undefined_variable] undefined variable 'nope'  (in function 'main')
  probe.z|4| I| [note] 'mine' is declared here; write 'return move mine;' to hand it to the caller
```

Errors, warnings and notes arrive as `E`, `W` and `I`, so `:cnext` can be filtered
with `:cdo`. Errors and warnings also get a gutter sign (`DiagnosticSignError`
and `DiagnosticSignWarn`); set `g:z_signs_error` and `g:z_signs_warning` to
change the glyph. A note that names a span — "declared here" — becomes its own
entry rather than being appended to the message, because the place it points at
is the thing worth looking at.

Two settings, both off by default:

```vim
let g:z_check_on_write = 1        " run :Zcheck after each write
let g:z_check_on_write_quiet = 0  " open the quickfix window on every save
let g:z_compiler = '/usr/local/bin/z'
```

`g:z_compiler` is used by `:Zcheck` and by `compiler/z.vim` alike. Without it
both run the `z` binary beside this checkout, so a clone needs nothing on `$PATH`.

## Quick tour

Three tiers of variable share one namespace, and where they collide the
priority is **lvar, then var, then gvar**:

```csharp
gvar base = 10;        // a global: its own storage, not a frame slot

int main() {
    var base = 99;             // a plain var, which shadows the global
    lvar what = 1;
    var what = 2;              // both may share a name; lvar wins

    Console.WriteLog(base);          // 99
    Console.WriteLog(gvar(base));    // 10, the global, named on purpose
    Console.WriteLog(what);          // 1
    Console.WriteLog(var(what));     // 2
    return 0;
}
```

`g` and `l` in front of a type name declare a global or a function-local:
`gvar lvar gint lint gbool lbool gstring lstring gfloat lfloat gFoo lFoo` are one
rule, so `gFoo` works for a `Foo` that does not exist yet. `gvar(x)`, `var(x)`
and `lvar(x)` name one tier on purpose, which is the only way to reach the
declarations the priority rule hides. A `gvar` is visible across a whole project:
put a `.zignore/config.z` marker in the project root and every `.z` file
under it compiles as one program, with no `import` needed. See
[docs/LANGUAGE.md](docs/LANGUAGE.md#three-tiers-of-variable).

```csharp
// Top-level statements — no main() boilerplate.
var total = 0;
for (var i = 1; i <= 10; i++) {   // C#-style for + ++
    total += i;
}
Console.WriteLog(total);                        // 55

// Arrays live on the heap; index with [], read .length.
var a = new int[5];
a[0] = 10;  a[1] = 20;
Console.WriteLog(a.length);                     // 5
Console.WriteLog(a[0] + a[1]);                  // 30

// foreach desugars to an index-based loop.
foreach (var x in a) { Console.WriteLog(x); }   // 10 20 0 0 0

// Pointers: & to take an address, * to deref.
var p = &a[0];
Console.WriteLog(*p);                           // 10
*p = 99;
Console.WriteLog(a[0]);                         // 99

// Functions (C#-style: return type, then name).
int fib(int n) {
    if (n < 2) { return n; }
    return fib(n - 1) + fib(n - 2);
}
Console.WriteLog(fib(15));                       // 610

// Structs (value types with fields).
struct Point { int x; int y; }
var p = new Point(3, 4);        // positional constructor
var q = p;                      // value copy
q.x = 10;                       // q.x = 10, p.x still 3
Console.WriteLog(p.x + p.y);               // 7
Console.WriteLog(q.x);                     // 10

// Methods with an implicit `this` (C# style).
struct Point { int x; int y; int Sum() { return x + y; } int Mag2() => x*x + y*y; }
var pt = new Point(3, 4);
Console.WriteLog(pt.Sum());                // 7
Console.WriteLog(pt.Mag2());               // 25

// Auto-properties and string interpolation.
struct Acct { int id; int Id { get; set; } }
var acc = new Acct(0); acc.Id = 9;
Console.WriteLog($"id = {acc.Id}");         // id = 9

// Extension methods.
int Twice(this int n) { return n * 2; }
Console.WriteLog(21.Twice());               // 42

// bool operators and string concatenation (incl. int→string).
bool ok = 3 > 2 && !false;
Console.WriteLog(ok);                            // true
Console.WriteLog("count = " + 42);               // count = 42

// Durations: a number with a unit stuck to it — h, m, s, ms, us. They run
// together, and a bare trailing number is seconds.
hold(2h21m37);                        // wait for 2h 21m 37s
hold(37);                             // same as hold(37s)
hold(500ms);                          // half a second
Console.WriteLog(2h + 21m + 37);                 // 8497 — a whole duration is an int of seconds
Console.WriteLog(1m500ms);                       // 60.5 — a sub-second part makes it a float
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
| Runtime | `runtime/z_rt.c` | `malloc`-backed allocator plus `z_free`, heap array alloc, the string representation and its library; embedded in the binary |
| Stdlib | `lib/*.z` | `StringBuilder` and the rest, in Z; embedded and spliced ahead of every unit |
| Types | `src/types.c` | pointer-based `Type` graph: scalars, pointers, arrays, structs, tagged unions, type params |
| Arena | `src/arena.c` | bump allocator — all compiler memory freed in one call |
| Diag | `src/diag.c` | `file:line:col: error: …` with a caret under the token |

Code generation uses an lvalue/rvalue split: `gen_addr` computes the address of
a variable/index/deref, `gen_expr` loads through it. Operands are staged through
rbp-relative temporary slots; calls follow the System V integer-argument
registers (`rdi, rsi, rdx, rcx, r8, r9`). Output is text assembly handed to the
system assembler — we do not write an ELF encoder.

## Roadmap

- **M0–M1 (done):** pipeline, expressions, control flow, functions, `int`/`bool`/`string`, `var`, `Console.WriteLog`, diagnostics.
- **M2 (done):** real type system, pointers (`&`/`*`), heap arrays (`new T[n]`, indexing, `.length`), `for`/`foreach`, `++`/`--`, string concatenation/`int`→string, **structs** (fields, nested, copy semantics, arrays of structs, by-pointer params), embedded runtime. **Later:** by-value struct pass/return (SysV classifier).
- **M3 (done):** C# sugar: properties, `$""` string interpolation, expression-bodied members, `operator` overloading, extension methods.
- **Surface names (done):** the spelling a program uses for a global is data
  rather than syntax, so a codebase can be written in its own dialect. A
  `.zignore/config.z` manifest maps each name a project writes to the name
  the compiler knows — builtins, standard-library types and their methods, and the
  project's own globals — and matching ignores case, so `say`, `Say` and `SAY`
  are one name. Nothing else changes: the compiler, the runtime and the emitted
  x86-64 are the same, because a surface name becomes an ordinary global before
  code generation sees it. A surface name is a *fallback*, so a project that
  declares the spelling keeps its own declaration.

  Two decisions are settled rather than open. The manifest is found by walking up
  from the source file, nearest first, and the nearest wins — a dialect is a
  property of a subtree, so a vendored project keeps its own. And **one** manifest
  governs a whole compilation rather than one per file, because `import` splices
  rather than isolates: an imported library is read in the importing project's
  dialect, which is the rule `import` already follows. `--no-surface` ignores the
  manifest and `--surface=<path>` names one, for a build that runs somewhere it
  should not.

  Not done: **keywords**. `var`, `foreach` and `match` are lexed rather than
  resolved by name, so they are not renameable yet — that is the lexer's table
  rather than a lookup site, and it is the remaining half of "every codebase looks
  like a different language". Making it work also raises a question the lookup
  sites do not: a keyword is not a fallback, so `if` would have to be genuinely
  absent rather than merely shadowed.

- **M3.5 (done):** the everyday-language layer — `const`; bitwise `& | ^ ~ << >>` and the compound forms; `break`/`continue`; the `null` literal; lexicographic string comparison; integer built-ins `abs min max clamp sqrt` and `sin`/`cos` (fixed point, a full turn of `1 << 30`); on-by-default range checking, with `--no-bounds` to turn it off.
- **M3.7 (done):** C interoperability — `extern` (implemented in C) and `export` (defined in Z, callable from C) in both directions, with linker arguments passed through. Every Z function is emitted under a private `z$` symbol, so it can no longer collide with a libc name, a runtime helper, or a word the assembler reserves.
- **M3.8 (done):** first-class function pointers — `&f` yields a value typed by
  the function's signature, `fn(params) -> ret` names the type, and calls through
  a pointer are argument-checked.
- **M3.11 (done):** **closures** — `(params) => expr` or `(params) => { ... }` as
  an expression, capturing anything in scope where it is written. A closure is a
  heap cell of `{ code, env }`; captured variables are boxed, so a closure that
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
  value, a pointer to a heap cell holding `{ code, receiver }` so the receiver
  stays alive; a virtual method binds through the vtable. Scalar representation,
  so no aggregate copy machinery is involved.
- **M4 (done):** `Result<T,E>` + `?`. A `Result` is a two-variant union, so `match`,
  payload binding and the exhaustiveness check all apply to it. `?` on a
  `Result` inside a function returning a `Result` with the same error type yields
  the `Ok` payload and returns the `Err` from the function, so a chain of
  fallible calls reads as straight-line code. Both parameters must be one word,
  which keeps every `Result` the same sixteen bytes and makes the propagated
  error a copy rather than a conversion.
- **M5 (built, then deleted):** tracing GC — a conservative mark-sweep collector in the runtime (scans the C stack + spilled registers), triggered on heap growth; kept live data and reclaimed garbage. v2 slice 3 removed it, because a conservative scan cannot see a pointer the caller holds only in a register, so it leaked, and its pause time is a property of the program rather than of the code. See [Known miscompiles](#known-miscompiles-found-and-fixed) for what deleting it exposed.
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
- **M6c (done, then cut in v2 slice 2):** opt-in `class` with vtables — inheritance, `virtual`/`override`, constructors + `base()`, `new C()` heap objects, polymorphic dynamic dispatch. The vtable, `virtual` and the constructor stay; the inheritance does not. `class B : A`, `override` and `base(...)` are now diagnostics pointing at `interface`, and the tests that used them were rewritten against interfaces with byte-identical output.
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
  Which local gets which register is decided by **loop depth, not declaration
  order**: each def and use adds `1 << (3 × depth)` to the local's weight, capped
  at six enclosing loops, and the allocator colours heaviest-first, breaking ties
  towards the earlier declaration. That is what keeps a counter read once per
  iteration in a register while a cold accumulator goes to the frame. Ordering by
  definition instead meant the accumulator won and the counter spilled, which on a
  matrix multiply was about 9% of total time — though note that matmul is **not**
  in `tests/bench/`, so that number comes from the commit that made the change and
  cannot be reproduced from this tree. An index expression also no longer marks a
  non-aggregate base address-taken, so an array or string base is register-
  eligible; the overlap test that stops two live locals sharing a register is
  unchanged. The array base is still reloaded through the frame on each access,
  which is what keeps a matrix multiply about 4× off `gcc -O2`.
  `-O3` adds **loop unrolling**: a loop with a condition, a body of at most 24
  statements, and at least two independent loop-carried recurrences is emitted
  four times over, testing the condition before each copy so the body still runs
  exactly as many times as it did rolled. The last copy branches back to the top
  and the rest fall through; each copy carries its own continuation label, so
  `continue` runs the step of the copy it is in and `break` leaves the loop from
  any of them.

  The recurrence requirement is the part that decides whether it is worth doing.
  A variable the body both reads and writes is a recurrence: its value in
  iteration *n+1* comes from iteration *n*, so copies touching it serialise
  against each other exactly as one copy did. With two or more, one copy's chain
  runs alongside another's. What makes this measurable is that unrolling is
  *instruction-neutral* here, not a win waiting on the right loop: a rolled
  iteration costs the body plus three instructions (the condition compare, its
  branch, the jump back), and an unrolled copy costs the same three, so the
  dynamic count per iteration is identical either way. Four copies buy three
  fewer loop-back jumps per four iterations and pay with three extra
  condition tests and four times the body in cache. Dependence is therefore the
  only thing that can make the extra code pay for itself, which is why a
  one-recurrence loop is left alone. On `mathbench` and `mixbench`, which are
  both `s = (s * 31 + i) % d`, that removed 26% and 11% of the emitted code at
  `-O3` and moved no timings at all -- which is what an instruction-neutral
  change is supposed to look like.
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

- **M10 (done): collections beyond fixed arrays.** `Vec<T>`, `Map<K,V>` and
  `Set<T>` live in `lib/collections.z`, written in Z and checked by the same
  front end as user code. The roadmap's open question was value or reference, and
  the answer is **reference**: each is a `class`, so a `Vec` is one pointer,
  assignment shares it, and `Vec<Vec<T>>` behaves. The alternative — a `struct`
  holding a pointer — is also one pointer, but assignment then aliases the same
  buffer in two variables, which is the trap a language with value-semantics
  structs invites badly. The language already draws the line (structs copy,
  classes share), so the interesting work was making that choice legible rather
  than making the container, and it is legible in one place: a field of class type
  is written `Map<T,bool>* inner` because the class type and its pointer are
  different types, and copying a handle by accident is a diagnostic.

  The roadmap also predicted `foreach` would be sugar for an `Iterator<T>`.
  That turned out to be wrong about the cost: an iterator is one interface cell
  and one indirect call *per element*, on every loop in every program, to buy
  generality no collection here needs. Each container is therefore iterable
  through a `foreach` that knows its own representation, which is a plain
  index loop in the container and zero overhead over the direct form. The
  interface is still there for user types that want it.

  `Map` is an open-addressed table with linear probing, one entry per occupied
  slot, rehashing to double the slot count at a load factor of 0.7. Growth
  happens *before* an insert rather than after, since a table past its load
  factor has probe sequences long enough to dominate the cost of the insert. A
  *removed* slot does not end a probe run, which is the only reason removal is
  not simply emptying. `Set<T>` is a `Map<T,bool>` and says so, and a set that can
  be asked for a value is a set that will be, so the pair of questions a set
  answers is `has` and nothing else.

  **The `any` parameter type.** A generic function cannot ask what it was
  instantiated with in a way its branches can use: `typeof` answers, but every
  branch after the test still has to type-check, and a branch calling
  `int_to_string` on a `string` does not. So the two questions that genuinely
  depend on the element type — a value's text form and a value's hash — are asked
  of the code generator through a parameter declared `any`. The generator knows
  the static type at the call site and lowers the call per type, so
  `to_text(x)` and `hash_of(x, cap)` are right for every key and element type a
  `Map` or `Set` can be instantiated with, and neither the standard library nor a
  user has to declare an interface that `int` and `string` would both have to
  satisfy. The cost is that `any` is opaque to the type checker: it checks
  nothing about its argument, which is why it is a parameter kind rather than a
  type a program can declare.

- **M11 (raw syscalls done, `Result` API blocked):** the language could print to
  stdout and nothing else. `open`, `close`, `read_byte`, `write_bytes`, `io_errno`
  and `io_eof` are builtins over raw file descriptors now, and `files` is the
  test. Descriptors rather than `FILE *`, because stdio buffers on one side of the
  boundary this language cares about: a `readLine` built on `fgetc` would pull the
  rest of the file into a buffer the program cannot see.

  Reading a *line* is now a builtin too — `read_string` from stdin, `input` for the
  prompt-then-read a console program opens with, and `read_line` from any
  descriptor — so "print to stdout and nothing else" is no longer the whole story.
  They read through the same `z_read_byte`, one syscall per byte, for the reason
  above. `stdin` is the test, and it is fed from a `.stdin` sibling because the
  harness has no terminal to type at.

  The `Result<File, IoError>` layer is now written and reaches the runtime, having
  been blocked by a compiler bug this milestone also fixed: **a method returning a
  `Result` lost its first declared parameter.** `f.w(5)` reported
  "argument 1 of 'w' expects 'F*' but got 'int'". Anything returned through memory
  gets a hidden result buffer pushed in front of the parameters, and the offset was
  computed by asking whether the return type is a `struct`. `Result` is a
  two-variant *union*, so the answer was no, the offset was one short, and the
  receiver was reported as the first parameter. A Result-returning method with no
  parameters was unaffected, which is why it survived: with nothing to misalign
  there was nothing to report. `result` is the test, and it fails on the old
  compiler.

  The error type is still the right answer to the roadmap's question, and the
  constraint that shaped it is already known: `Result<T,E>` requires each side to
  be one word, so `File` and `IoError` both have to be classes, which is what lets
  the error carry a formatted message without the `Result` growing.

- **M12 (done): a string type that is not just concatenation.** A `string` is a
  pointer to a `{ len, cap }` header followed by the bytes. Three problems, three
  fixes: `len` was a `strlen` and is now a load; a zero byte ended the string and
  is now a byte, so `"a\0bc"` has length 4, prints in full, and can be searched
  and split; and comparison was `strcmp` and is now by content and then length,
  so two strings differing only after an embedded zero are no longer equal.

  **The decision the roadmap flagged** — a 16-byte value passed in two
  registers, or a pointer — is taken as *a pointer with a header*, and the reason
  is the C boundary. A `string` that cannot cross into C is not a string in a
  language with C interoperability, and a type needing a special case at every
  ABI is one that will get the special case wrong somewhere. A header keeps
  `string` a scalar: it still travels in a register, and `extern`/`export` still
  mean what they meant. The trailing NUL is kept on every string even though
  nothing in Z reads it, which is exactly what makes a Z string valid to hand to
  a C function as `const char *`. What the C side *cannot* do is see past a zero,
  so the interop test demonstrates reading the length out of the header — the one
  thing C lacks a way to ask for.

  Indexing and slicing come with it: `s.length`, `s[i]` as a byte in `0..255`,
  and `s[a..b]` half-open with either end optional and both clamped. The
  roadmap asked what indexing means for a language with `\uXXXX` escapes and no
  character type: **bytes**, stated once in the reference and applied everywhere,
  including in the bounds-check message, which now says which kind of thing was
  indexed.

  Appending is a separate type. `s = s + x` still copies, because `s` is a value
  and growing it in place would write through to every other name bound to it —
  the runtime comment on `z_str_buf_append` is explicit that in-place growth is
  only safe where the caller owns the buffer. `StringBuilder` is a class that
  does own one, so `append` is amortized O(1) instead of O(n²) over a loop;
  `toString()` copies and `take()` does not, which is the whole difference
  between them.

  Two bugs this found that the old representation could not have had, both worth
  recording: string interning deduplicated on `strlen`, so `"a"` and `"a\0bc"`
  compared equal on their first byte and shared an id — every use of the shorter
  literal silently got the longer one's bytes; and the `.asciz` emitter walked
  the literal to its first zero, emitting a truncated body under a header that
  claimed the full length.

- **M12b (done): the standard library is Z, not C.** `lib/*.z` is embedded in
  the compiler and spliced ahead of every compilation unit, the same way the
  runtime is. `StringBuilder` is a class with methods, checked and compiled by
  the same front end a user's code goes through — so a bug in it is a bug any
  program using it would have found, rather than something only the C side can
  reach. `--no-std` compiles without it. M13 turns this into real packages.

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

- **M14 (done): error message quality.** A diagnostic now carries the three
  things that were missing: what the reader was probably trying to write, where
  they were when they wrote it, and what the constraint actually was.
  - **"Did you mean"** over the names in scope, by edit distance. A wrong-case
    name always suggests, an extension or prefix (`helper` for `main_helper`)
    outranks a one-edit match because a human would say nothing else, and the
    candidate list is split by what the name is — a *type* position never
    suggests a function, because naming something that is not a type is worse
    than saying nothing. Covers undefined variables, functions, types, methods,
    fields and match variants.
  - **The enclosing function or method is named** in every message, so a report
    in a hundred-line body is locatable. A message inside a lambda or inside a
    monomorphized generic also says *that*, and points at the line the lambda
    started on or the call that asked for the instance — because the span in
    the message points at text the reader wrote somewhere else entirely.
  - **The constraint, not only the violation.** An argument type error and an
    arity error both print the declaration they violated
    (`'add' is declared int add(int, int)`), and a non-exhaustive `match` names
    the variants it does not handle instead of counting them.
  - **Cascades are suppressed.** `Console.WriteLog(ghost)` used to report both the
    undefined name and `'Console.WriteLog' expects 'int' but got '<null>'`; the
    second is a second complaint about one mistake, and it buries the first.
  - **Colour** on a terminal, off when redirected or piped, forced with
    `--color=always|never|auto`, and off under `NO_COLOR`.
  - **`--error-format=human|gcc|json`.** `gcc` is one line per problem and one
    per note, which is what an editor's error parser already wants. `json` is
    one object per line with a stable `code`, an explicit span in both line/col
    and byte offset, and the notes inline — so an editor can put a squiggle
    under the right word without scraping prose. The Vim plugin uses the `gcc`
    form, which makes each note its own quickfix entry.

### Known miscompiles found and fixed

Recorded because each was invisible at the default optimization level, and
`make test` only ran `-O1`:

- **LICM hoisted expressions out of the loop that varied them.** The pass was
  handed a loop's body but never its step, so an induction variable looked
  invariant and `p * 2` was computed once, before the loop, and reused for every
  iteration. `nested_loops` *hung* at `-O2` and was correct at `-O1`. The pass
  now takes the step, and `make test-all` runs the whole suite at every level so
  a pass that only runs at a higher level cannot hide again.
- **LICM hoisted an expression out of a loop in any function with more than 256
  locals.** The pass keeps a fixed 256-entry table of the locals a loop body
  writes, and a slot past the end of the table could not be recorded. A read of
  an unrecorded slot tested as "not written", so an expression over one looked
  invariant and was hoisted, and every iteration reused the first value — the
  same class of bug as the one above, reached a second way. A slot the pass
  cannot track is not *provably* invariant, so it is now treated as not
  invariant. `licm_bigframe` is the regression test: a function with 300 locals
  whose arithmetic gives values that fail loudly rather than quietly agreeing with
  a miscompile.
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
- **A string temporary was released once per loop iteration instead of once.**
  `Console.WriteLog("ab" + "cd")` in a loop allocates a concatenation no variable
  ever names, and with the collector gone it had no owner at all. Spilling it to a frame slot
  and releasing it at the end of the statement fixed the leak and introduced three
  ways to free the wrong thing, each of which is a crash rather than a wrong
  answer. A slot on the temp stack is reclaimed by every assignment and call, so
  the slot holding the pointer was reused by the next expression in the same
  statement and the release read an integer: those slots are now *pinned*, and
  `temp_alloc` steps over them. An assignment's value belongs to the slot it wrote,
  not to the statement, so `buf = str_buf_new(0)` was freeing the buffer it had just
  stored. And an expression that may not run -- the untaken arm of a ternary, the
  right operand of a short-circuited `&&` -- is still *emitted*, so recording a
  temporary there left the slot holding the previous statement's pointer. Each of
  those was found by ASan on the same program, and `temporaries` is the test.
- **A string temporary in a loop's condition or step leaked once per iteration.**
  Both are generated once inside the loop but run every pass, and the release was
  emitted after the loop rather than after each iteration, so every value but the
  last was overwritten in its slot and never freed: 29 allocations of 541 bytes for
  a 29-iteration `for (var i = 0; i < 50; i = i + len(int_to_string(i)))`. A leak
  rather than a wrong free, and not reachable from a golden test, because the
  program's output is the same either way and the harness does not measure leaks,
  so it took an AddressSanitizer run to see at all.
- **A `match` arm that did not run freed a string pointer from the arm that
  did.** Both arms are emitted and one is skipped, so the skipped one left its
  frame slot holding whatever the previous statement had put there, and the
  end-of-statement release called `free` on it. On a `Result<int, string>` the Ok
  payload is an int, so a three-line program freed a small integer as a heap
  pointer. This is the third instance of one bug: a ternary's untaken arm and the
  right operand of a short-circuited `&&` were fixed when the temporary release
  landed, and `match` was the one conditional form left out. `match_arms` is the
  test, and it crashes on the compiler before this.
- **The ninth queued function replaced the first eight with garbage.**
  `add_pending` grew its array by allocating a new block and never copying the
  old contents, so a program with eight lambdas worked and one with nine crashed
  the compiler rather than failing at the point of the bug. The generic
  instantiation path shared it.
- **`Vec.pushAll` could not be called at all.** Its parameter was written
  `Vec<T>`, which names the class rather than a handle to one, so every
  argument mismatched and no vector could be passed. It was the only
  class-typed parameter in the standard library, and the only one missing the
  `*` that `Set.inner` and `Set.containsAll` both carry, and nothing in the
  library or the tests called it — so it compiled, type-checked, and was
  unreachable. The fix is the pointer. The test that finds it is
  `collections2`, which calls every declared method once, because a method
  nothing calls is a method nothing has checked.
- **A test with 300 warnings was reported as a compile failure.** The harness
  read the compiler's output into a 64KB buffer and then stopped, leaving the
  child writing into a pipe nobody was reading, so the compiler died of
  SIGPIPE and the harness read the resulting nonzero exit as "compile failed" on
  a build that had succeeded. The harness now drains the pipe after the buffer
  fills. The real failure was always the warnings, and there were 300 of them
  because `licm_bigframe` is a 300-local function on purpose.
- **`&obj.field` compiled as a method pointer, and failed.** `&` on a member
  name went looking for a method, so `&h.data[0]` reported "type Holder has no
  method 'data'" for a field the type plainly had. The parser now looks the name
  up as a field first and only goes looking for a method when it is not one, so
  the two spellings that share the token decide by lookup rather than by hope.
  `addrof_field` is the test, and it ends with a method pointer to show the
  other half of the branch still works.
- **Every string and array header was written outside its own allocation.** Both
  are `{ len, cap }` and `{ count }` headers stored immediately *before* the
  payload, and both allocators wrote that header at `ptr - 16` where `ptr` was
  whatever the allocator returned. That was in bounds only because the collector
  prefixed every block with a 24-byte `GcBlock` header and the Z header fitted in
  the slack behind it, so for the life of the collector every string's header was
  quietly stored inside the *previous* allocation. Deleting the collector (v2
  slice 3) is what turned it into a write of up to sixteen bytes past the end of
  the block. `split` is what noticed, because the empty piece between two
  separators is a fresh allocation sitting against its neighbour, so the write
  landed in the neighbour's header and `join` computed a total length in the
  terabytes. The headers are now stored *inside* the block they belong to, and
  `ownership` is the test. The lesson is the one the zeroed-array comment beside
  `z_newarray` already made: a hidden dependency on allocator slack is not a
  safety property, it is a bug that has not been reached yet.
- **A virtual method returning a struct never passed the result buffer.** A
  struct larger than a register is returned through memory, so the caller
  reserves a buffer and passes its address as a hidden first argument in `rdi`.
  The virtual call site told the argument-assignment pass that a hidden argument
  was there, so the first declared argument went to `rsi` rather than `rdi`, and
  then nothing put the buffer in the register it had vacated. The callee wrote
  the struct through whatever `rdi` happened to hold and the caller read a buffer
  nothing had written, so the first word came out right by luck and the rest was
  whatever the frame had in it. The `lea` that should have loaded the buffer was
  emitted, but after the `return` that ends that path, which made it dead code.
  `vcall_struct` is the test, and the bigger struct in it is the shape that took
  the process down rather than answering wrongly: with a string and a float
  beside the ints, the callee writes far enough from the start of the buffer for
  the stray write to reach something that matters.
- **A call through a closure or an interface claimed `rdi` twice.** A closure's
  environment and an interface's receiver are each a hidden first argument, and so
  is a struct result buffer, so a call with both has *two* leading integer-class
  arguments rather than one. Only one was counted, which put the declared
  arguments a register too early, and the receiver was then loaded into `rdi`
  after the buffer had gone there. Three places had to agree and none of them
  did. The call site now counts both, the buffer takes `rdi`, the environment
  takes `rsi` and the declared arguments start at `rdx`. The hoisted lambda never
  reserved a slot for the buffer at all, so its prologue stored `rdi` into the
  environment's frame slot and a capturing body indexed the buffer as though it
  were the capture array. And the interface trampoline read the receiver from
  `rdi` unconditionally, which dereferenced the *buffer* and jumped to whatever
  the length header held. A closure returning a struct printed zeroes or frame
  garbage, and the same call through an interface exited 139. `icall_struct` is
  the test.
- **A comment claimed the parser refused these combinations when it only refused
  bound pointers.** `gen_icall` said a closure and a bound pointer were both
  refused for a struct return, and used that as the reason the case could not
  arise. Only `TK_MPTR` is refused, in two places in `parse_postfix` and
  `parse_primary`. A closure and an interface were never refused, which is why the
  combination sat there unexercised and the comment read as a reason it was safe.
- **The closed-form loop rewrite returned wrong answers at `-O2`, four ways.**
  A counted accumulate loop is replaced by `amount * trip count`, and four things
  had to be true that the pass did not check. It did not write the induction
  variable's final value, so a read after the loop saw the value from *before* it
  -- the one observable consequence of a rewrite that is otherwise invisible, and
  it shows up exactly where a loop is normally invisible, on a counter printed
  after it. It negated the stride for a downward comparison, which made every
  genuinely downward loop look like a sign mismatch, so none was ever closed, and
  a loop whose sign disagrees with its comparison -- which is a loop that cannot
  terminate -- was given a finite trip count and a program that returned from it.
  It missed a `for` that *assigns* its loop variable rather than declaring it, fell
  through to the nearest write it could see, and counted from there: nine
  iterations where there were four. And it let a call stand as the amount, because
  it sees a call's arguments and nothing below them, so a function that answers
  differently each time was multiplied by the trip count: three lines of input gave
  6 from the loop and 3 from the closed form. The first, third and fourth are in
  `closedform`, the fourth alone in `closedform_call`, and the second in
  `down_closed` and `sign_mismatch`, which are only ever compiled.
- **Two comments in `closedform.z` described the pass by what it did rather than
  what it decided.** One said a downward loop was solved when no downward loop was
  ever closed. The other said a call in the body was not solved when a call was
  accepted for the wrong reason: it was rejected by an accident of the shape check
  rather than by a decision about calls. Both are corrected, and the tests they
  describe now hold.
- **The register allocator used statement index zero inside an addressed
  expression.** `mark_addr_taken` walked the subtree it was given at a hardcoded
  index of 0 rather than the statement's own, and the index is what the allocator
  colours `[def, use]` intervals from. A local defined at statement zero whose only
  use was inside an addressed subtree therefore had that use recorded at zero too,
  so its range was `[0, 0]`, which is a valid range, and it was given a register.
  The addressed expression then went on reading the frame slot that the register
  had made unreachable, so the index an assignment used was whatever the slot
  happened to hold. In `addr_index` that was another local's value, and
  `a[i] = t` wrote to the wrong element. The index is now the statement's own, at
  all five sites that walk an addressed subtree.

  Only the assignment site can do this. The other four also mark the subtree
  ineligible for a register, which is what keeps a variable under them in memory
  whichever index the walk used, and `addr_index` covers the two other forms an
  assignment can take to say they still work.
- **A string accumulate was compiled as integer arithmetic on a pointer.** When a
  scalar local lives in a register, `x = x + ...` does not materialize its
  address: the chain is applied straight to the register, three-address style,
  which is the point of the optimisation. The test for that shape never looked at
  the *type*, only the shape, and a string has the same shape. `s = s + t` is an
  addition rooted at a read of `s` with a variable on the right, so a string chain
  passed, and the `add` was applied to the pointer. `s` became an address rather
  than a string, and reading it back read whatever that address held: the program
  died with 139 and printed nothing. The chain now requires an `int` or a `bool`,
  which are the types whose addition is the machine's. A float has its own
  in-place path in a vector register, a string has to go through the runtime's
  concatenation because that allocates, and neither can be folded into an integer
  add. `accum_chain` is the test, and it covers the int and float cases too so
  that the optimisation is shown still firing.
- **A declaration with a written-out type never checked its initializer.**
  `parse_var_decl` recorded the declared type and kept the initializer, and
  nothing in between converted one to the other or compared them. `float f = 3;`
  compiled to a zero, because the declared type was a float slot that nothing
  wrote, so reading `f` read uninitialised memory and printed `0`. `int x =
  "hello";` compiled, and reading `x` printed the string pointer's own bytes as an
  integer. The check an assignment has always made is now made here too, in the
  same order and with the same two exceptions: an int widens to a float because
  that cannot lose a value, and a concrete value going to an interface boxes. So
  `float f = 3` is `3`, and `int x = "hello"` is a `type_mismatch`.
  `var_init` and `var_init_type` are the tests.

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

### Recursive and call-heavy code

`fibbench` does not improve above `-O1`: 305 instructions and ~0.11 s at every
level. Two separate things are going on, and it is worth keeping them apart,
because the obvious one is a dead end.

**The inliner declining recursion is correct, not a gap.** `fib(35)` is a tree of
2^35 calls, so no bounded inliner can help it; inlining the recursion is not a
missed speedup, it is an explosion. What gcc does at `-O2` is something else
entirely: it recognises `fib(n-1) + fib(n-2)` as a linear recurrence in *n* and
rewrites the recursion into a loop, which is why it goes from 0.07 s to 0.02 s
there. That is an algebraic transformation of the recursion, in the same family
as the closed-form loop pass, and it is a much larger piece of work than call
codegen.

**The cost is per-call overhead, and it is large.** `z$fib` is 27 instructions
per call, of which only 7 do the recursion. The other 20 are 6 for frame setup
and teardown, 11 for memory, and 3 jumps and moves, one of which is a dead
`jmp` to a label emitted immediately after an unconditional jump to the same
place. Most of the memory traffic is one parameter: it is spilled
unconditionally (`alloc_regs`) and read back from its slot on every use, so
`fib`'s `n` costs a store in the prologue and five loads, and a function with one
parameter and no locals still pays `sub rsp, 48`.

Keeping parameters in the callee-saved register pool was implemented and
measured, and it is **not** a win: `fib` was unchanged (min 0.11 s and 25th
percentile 0.11 s both ways, 40 interleaved runs) while the emitted code grew
from 305 to 319 instructions. The reason is that `fib`'s critical path is the
call and return sequence and the dependent branch on `n`; the parameter loads
hit L1 and were never on it, whereas the `push`/`pop` the change adds per call
are. Removing memory traffic only helps when the loop is stalled on memory, and
this one is stalled on the call. So the parameter stays in its frame.

The remaining gap to `gcc -O1` (0.11 s against 0.07 s) is not the spill at all.
gcc keeps no frame pointer, so it spends no instructions on `push rbp` /
`mov rbp, rsp` / `mov rsp, rbp` / `pop rbp`, and it forms each argument with a
single `leaq -1(%rbx), %rdi` where Z stores the value to a temporary and reloads
it. Those are per-call overheads of a different kind from the one measured
above, and neither has been tried.

One trap worth recording, because it cost this session a wrong answer.
`mark_addr_taken` used to walk the addressed expression at statement index 0
rather than the index of the statement containing it, so a local read *inside* an
addressed expression got its live range recorded at the wrong place. That was
harmless while every local's range started at its own definition, and it turned
into a miscompile the moment a parameter's range was seeded at function entry:
`inner(int* a, int i, int k)` gave its `i` a register, a later local took the
same register, and `a[i] = t4` stored through the wrong index. `frame_layout`
caught it as a diff (63 where 48 was expected). Any future work that reasons
about liveness has to thread the real index through that walk.

## Design

See **[docs/LANGUAGE.md](docs/LANGUAGE.md)** for the v2 specification: the
grammar and type rules, and the design rationale for the three decisions
everything else depends on — ownership with explicit `move`, destruction on
every exit path, and the concurrency model that ownership makes sufficient.
[docs/LANGUAGE-v1.md](docs/LANGUAGE-v1.md) is the v1 specification, which is what
the compiler in this repository implements.

The two documents are kept side by side on purpose. v2 changes the language, not
the backend, so the parts of v1 that carry over — the optimizer, the ABI, the
diagnostics system, the 106 golden tests — are still the spec for the parts that
do not change, and the diff between them is the reviewable part of the rewrite.

## License

MIT. See [LICENSE](LICENSE).
