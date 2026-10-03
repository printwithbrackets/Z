# Z — language specification (v2)

Z is a small, statically-typed systems language. It compiles to native x86-64
machine code. It has the shape of C++ — value types, references, templates,
RAII, zero-cost abstractions — with the sharp parts taken off.

**Status: this document specifies v2. v2 is not implemented.** The compiler in
this repository implements v1, specified in [LANGUAGE-v1.md](LANGUAGE-v1.md) and
in the "What exists today" appendix below. v2 is built in the five slices listed
in [Roadmap](#roadmap), in order, each of which is a working compiler at its end.

Everything in v2 marked **[now]** exists in the compiler today. Everything marked
**[new]** is specified here and not yet built. **[cut]** is in v1 and is being
removed. If a section has no marker, it is unchanged from v1 except where it
mentions a cut or added feature.

The five properties v2 is built for, in priority order, are:

1. **Memory safety without giving up speed.** Ownership is checked, memory is
   freed deterministically, and the cost of the guarantee is a move at run time
   or a field read at compile time — never a garbage-collection pause.
2. **Simpler syntax.** No boilerplate, no headers, no macro dialect, one way to
   say each thing.
3. **Zero-cost performance and real control.** A `Vec<T>` is a pointer, a length
   and a capacity. A `for` loop is a loop. There is no interpreter, no boxing,
   no vtable where a template would do.
4. **Fast compiles.** Monomorphization of a small function is milliseconds. A
   whole-program compile of a real program stays under a second, and this is a
   design constraint rather than an accident.
5. **Concurrency correctness.** Two threads cannot touch the same mutable value,
   because the ownership rules make the shared-mutable case unrepresentable
   rather than merely discouraged.

## What v2 drops, and why

Each of these is a thing v1 has. Each is cut deliberately, and the reason is
recorded because the reason is the design.

| Cut | Why |
|---|---|
| **Inheritance** (`class B : A`, `override`, `base()`, upcast) — **cut in slice 2** | It is the single largest source of C++ complexity for the least amount of used code. Every hierarchy is a set of types that respond to some messages, which is what `interface` already is and does without a vtable per class or an upcast that can be wrong. |
| **The garbage collector** — slice 3 | A conservative mark-sweep collector cannot see a pointer held in a register the caller spilled, so it leaks; it cannot move an object, so it gives up compaction; and its pause time is a property of the program. RAII frees at a point the programmer wrote down, has no pause, and moves nothing. The cost is that a cycle leaks, and that is stated rather than hidden. |
| **`this` extension methods** (`int Twice(this int n)`) — slice 3 | They make a bare call site's meaning depend on an invisible second program, and they are how a global namespace gets polluted. Free functions with an explicit first parameter say the same thing and can be found. |
| **Properties** (`int X { get; set; }`) — slice 3 | A property is a method pair with a name that does not say which it is. v2 uses fields and methods, and `size()` rather than `length()` follows the same rule: a name says whether it is a field or a call. |
| **Implicit conversions** except `int`→`float` | A conversion that happens without being written is one the reader has to know about to be sure of the program. The single widening is kept because it is lossless; nothing else is. |
| **`T[]` as the only aggregate** — slice 3 | A fixed-length heap array with a length header, grown by hand, is one container out of many, and it is the one nobody wants. `Vec<T>` is the aggregate, and it is a value. |

**What v2 keeps from v1, unchanged in spirit:** top-level statements, `var` and
`auto`, structs, enums with exhaustive `match`, `Result<T,E>` with `?`,
interfaces, closures, function pointers, templates by monomorphization, C
interoperability in both directions, modules, the whole diagnostics story, the
collections, and the whole optimizer.

**What v2 has that v1 did not,** as of slice 2: `auto`, `->`, the range-based
`for`, the C++ container names, and no inheritance. The first four are additive
and a v1 program still compiles. **The fifth is not, and is meant not to be** —
a program that used `class B : A` now gets a diagnostic saying inheritance is
gone and to use an interface instead. That is the one change so far that
requires editing a caller.

## The three decisions everything else depends on

### 1. Ownership: a value has one owner, and moving is written down

v1 had two answers to "who owns this memory" — the stack, or the collector —
and no way to write a type that held a resource. v2 has one: **a value is owned
by the variable that holds it, and ownership is transferred with `move`.**

```
var a = Vec<int>();        // a owns a heap buffer
a.push_back(1);
var b = move(a);            // b owns it now; a is spent
a.push_back(2);             // error: 'a' has been moved from
```

- **`move(x)` transfers and leaves the source spent.** Reading a spent variable
  is a compile error, not a runtime check. This is the one rule that makes the
  whole model work, and it is checked at compile time because a use-after-move
  is determinable from the flow.
- **Copy is deep and is called `clone`.** `var b = a.clone();` is an explicit
  copy. There is no implicit copy of an owning type, so a line of code never
  quietly costs an allocation. A type with no destructor and no owning field —
  `struct Point { int x; int y; }` — is still copied on assignment, because
  copying it is a register move and there is nothing to own.
- **A type is owning if it has a destructor or an owning field, and it is
  computed by the compiler, not declared.** `Vec<T>`, `string`, and a user's
  `struct File { int fd; ~File() { close(fd); } }` are owning because the
  compiler can see that they hold a resource. A type that needs to be a
  non-owning handle says so by holding a reference, not a value.
- **References are the borrow.** `T&` is a name for a value owned by someone
  else, checked to be alive at every use. It cannot be stored in a struct, put
  in an array, or returned. Returning a reference is only allowed for a `&self`
  parameter — the one function that is allowed to hand back a borrow of what it
  was lent, and the caller already holds the owner alive.

Why not a borrow checker: it is the most expensive single feature in this
document, and getting it wrong produces a compiler that rejects correct
programs, which is worse than one that accepts a program with a use-after-free
the programmer can see. What v2 does instead is make the *common* case
unrepresentable — one owner, explicit transfer, checked use — and leave raw
pointers in `unsafe` for the cases that need them. The alternative is honest
about its cost; see [Memory safety: what is and is not
guaranteed](#memory-safety-what-is-and-is-not-guaranteed).

### 2. Destruction: destructors run on every exit path, including `?`

**[partial]** A first cut of this section is in the compiler: the destructor
syntax, reverse-declaration-order teardown, and the `return` and scope-end paths
work today. The `?`, `break`, `continue` and panic paths do not, a type with no
destructor of its own does not tear down its owning fields, and there is no move
semantics yet, so returning an owned local frees it out from under the caller.
See [What exists today](#what-exists-today-v1-appendix) for the exact split.

A `~Type()` method runs when the value's scope ends, **in reverse declaration
order**, and on every path out of that scope: falling off the end, `return`,
`break`, `continue`, a `?` that propagates, and a panic.

That last one is the requirement with teeth. In v1, `?` was a return and there
was nothing to unwind. In v2, `?` in a function holding a `Vec` means the
vector's buffer is freed on the way out, so **the compiler must know every exit
from every function that owns a value.** That is a real analysis and it is named
here so it is not discovered as a leak in slice 3.

A type's destructor is the default: destroy each owning field, recursively. A
user-written `~Type()` runs first and then the fields are destroyed, which is
C++'s order and the one that lets a destructor use a field it is about to
release. A type with no destructor and no owning field has no teardown at all,
so `struct Point` costs nothing.

There is no `delete`. A value is destroyed by its scope ending, which is
checked rather than remembered, and there is no spelling that can be gotten
wrong. A raw allocation in `unsafe` has a `free(x)` that must appear on every
path, and that is a `Result` — `free` returns whether it freed, so a leak
becomes a value the program can check.

**Cycles leak.** A `Vec<Box>` holding boxes that hold the `Vec` back is not
collectable without tracing, and there is no tracer. A leak is reported at exit
by the runtime when it can see one, and is otherwise a leak. This is stated
here rather than discovered.

### 3. Concurrency: ownership makes the data race unrepresentable

Threads and channels, and one rule:

> **A value belongs to one thread. Sending it to another thread moves it.**

```
var chan = Chan<int>();             // unbounded channel
thread(() => {
    for (var i = 0; i < 1000; i = i + 1) {
        send(chan, i);
    }
});
var total = 0;
while (let v = recv(chan)) {        // recv yields a Result
    total = total + v;
}
print(total);                       // 499500
```

Because ownership is single-threaded by construction, there is nothing to
synchronize: two threads cannot hold the same mutable value, so there is no
data race to prevent. A shared counter is a `Chan<int>` and the channel *is* the
synchronization. This is why the concurrency work is small — it is the
ownership work, reused.

- **`Rc<T>` and `Arc<T>` are different types on purpose.** `Rc` is
  single-threaded reference counting and is not `Send`. `Arc` is atomic reference
  counting and is. Writing `Arc` in one thread and `Rc` in another is a compile
  error, so a "I only use this on one thread" note cannot rot into a race. This
  is the single most valuable line in the section.
- **A channel send is a move, checked.** Sending a value that is still owned
  elsewhere, or sending a non-`Send` value like an `Rc`, is a compile error.
- **The guarantee stops at the C boundary.** `extern` and `export` hand raw
  pointers across a boundary the compiler cannot see. A C function that writes
  through a pointer the Z side also reads is a data race the language cannot
  prevent. **`extern` declarations must be marked `unsafe` to say the pointer
  crosses the boundary unchecked**, and the compiler will not reason about the
  memory a C function retains. This is a real hole and it is the one place the
  "no shared mutable state" claim needs a footnote.

## Roadmap

Each slice ends at a working compiler. The order is a dependency order, not a
preference: slice 3's ownership rules are what make slice 4's thread-safety
argument work, so the cheap syntax work comes first and the hard semantic work
comes before the thing that depends on it.

### Slice 1 — surface, no semantic change (days)

Purely additive, so the test suite keeps passing throughout and this can be
reviewed as a diff. **Done except for the last two rows**, which need the
module system rather than the parser.

| Change | Notes | |
|---|---|---|
| `auto x = ...` | A synonym for `var`, not a replacement. Both stay. | done |
| `p->f` | Alongside `p.f`. `->` desugars to an explicit deref and reuses the same member-access path, so the two cannot disagree. | done |
| `for (auto x : v)` / `for (auto& x : v)` | Range-for over a fixed array, a `string` (as bytes), and any class with an `at(i)` — which is `Vec<T>`. Desugars to the same index loop `foreach` produces, so `break`/`continue` and every optimizer pass behave identically. | done |
| `push_back`, `pop_back`, `empty` | C++ names, on all three containers, as one-line aliases rather than second bodies. Both spellings are kept. | done |
| `T&` references | **Not a pointer with a different name.** In slice 1 `&` in a range-for makes the loop variable a *pointer* to the element, so the body writes through `*x`. A real `T&` type is part of slice 3, because it is a borrow with a lifetime and that is the same work as the ownership rules. | partial |
| `namespace`, `pub` | Module scope and visibility, unexported by default. Also the fix for v1's M13 problem, where two imported files could not both define `helper`. Needs the module system, not the parser, so it is not slice-1 work in practice. | not started |

Two bugs the range-for surfaced on the way, both of which were live before it:
`&obj.field` was parsed as a method pointer (so `&h.data[0]` reported a missing
method for a field the type had), and the test harness reported a successful
build as a compile failure when the compiler emitted more output than its
capture buffer held. See the miscompiles section of the README.

### Slice 2 — cut inheritance, keep classes (1–2 weeks) — **done**

`class B : A`, `base(...)` and `override` are gone. `class` stays, `virtual`
stays, and so do constructors, `this` and `new C(args)`. The vtable stays too:
it is how a class stored in an interface is dispatched, so a vtable with no
subclass is still a working vtable.

`interface` is now the only way to collect related types, and it already was
the way to collect *unrelated* ones. What a hierarchy bought — a base type to
upcast to, a base constructor that can be forgotten, an override slot to reuse —
is what is gone, along with the derived-to-base assignment in
`type_assignable` that could silently hand back a pointer to a prefix of an
object.

Each removed form is a **diagnostic that says what to do instead**, not a
silent unknown: a program carried over from v1 needs to be told that
inheritance is gone rather than left with an undefined method name. Three
diagnostics, three error tests.

The four tests that used inheritance (`integration`, `methodptr`,
`local_types`, `interfaces`) were rewritten against interfaces and produce
**byte-identical output**, which is the check that the cut cost nothing.
`classes.z` and `classes2.z` are new and cover the same shapes without a base
class. `lib/*.z` is untouched: none of `StringBuilder`, `Vec<T>`, `Map<K,V>` or
`Set<T>` inherited anything.

One thing worth recording: rewriting the vtable layout dropped
`m->vtable_index = slot`, so every virtual method kept index 0 and two virtual
methods on one class both dispatched through the first slot. `floats` stopped
printing and exited 1 **with no diagnostic at all**. Silence was the failure
mode — a compile emitting a call through an empty vtable slot has no way to
complain — and it was the golden tests that caught it rather than a crash.

### Slice 3 — GC to RAII, and ownership (3–4 weeks)

The largest slice and the one with the named risk. The first two bullets are
done, and the rest of the slice is waiting on them.

- **done — Delete** `z_gc_init`, the mark-sweep collector, `gc_mark`,
  `gc_mark_roots`, `gc_collect`, and the `gc_enabled` / `gc_threshold` state in
  `runtime/z_rt.c`. `z_gc_init` and `z_gc` are gone from the runtime and the
  `call z_gc_init` is gone from the generated entry function.
- **done — Replace** `z_newarray` with a plain allocator and add `z_free`.
  `z_alloc` is `malloc` with a fatal allocation failure, and `z_free` is its
  counterpart. `z_str_free` and `z_array_free` are the two header-aware
  spellings the compiler will call once ownership is tracked.
  **Nothing calls them yet**, so every allocation a program makes is still live
  at exit. That is deliberate and it is the reason the collector had to go
  before the ownership rules rather than after: a destructor that freed
  unconditionally would double-free every value a program copied, so the
  frees cannot be wired up until a copy of an owning value is a diagnostic
  instead of a silent alias.
  The swap also found a bug the collector had been hiding: both headers are
  stored immediately *before* the payload, and both allocators were writing
  them at `ptr - 16`, which was only in bounds because the collector's own
  24-byte block header left room for them. They are stored inside the block now.
- **done — `string`** keeps its `{len, cap}` header, that decision was made for
  the C boundary and is still right, and has a destructor. Copies are deep: the
  parser rewrites every store of a borrowed string into a `str_dup`, a plain
  assignment releases the value it overwrote, and the scope teardown calls
  `z_str_free`. A literal is owned by nobody and says so with `cap == 0`, so
  storing one copies no bytes and releasing one frees nothing.
  `move x` hands a value on instead of sharing it, clears the source, and makes
  reading the source a diagnostic naming the move. It is what `StringBuilder.take`
  uses to stay a handover rather than an alias, and what `return move local;` is
  for.
  A string computed but never stored is a **temporary**, and it is released when
  the statement that made it ends, so a loop of `print(a + b)` is bounded rather
  than one leak per iteration. The value is spilled to a pinned frame slot and
  released after the statement, and an expression that may not run (a ternary's
  untaken arm, the right operand of a short-circuited `&&`) records nothing,
  because both are emitted and the skipped one would leave a slot holding the
  previous statement's pointer.
  **Not yet: class objects, heap arrays and closure cells still leak**, along with
  anything reachable only through them. Those are the rest of this slice.
- **`Vec<T>`** stops being a `class` and becomes a `{data, len, cap}` struct
  with a destructor. It is no longer an aliasing trap: `var b = a` is either a
  copy or an error, never a surprise.
- **Closures** box captures in `z_box` (`runtime/z_rt.c`) and those boxes are
  freed when the closure is. A closure that outlives its frame therefore needs
  its captures to live as long as it does, which is `Rc` — the same cell, with a
  count. This is a small change to the runtime and the reason the counter
  example in [Closures](#closures) still works.
- **done — Every exit path runs destructors.** `return`, `break`, `continue` and
  `?` all destroy what the scope they leave owns. Two floors have to be resolved
  where the jump is written, never in the destructor pass: by the time the pass
  runs, every scope has been popped, so a floor read there is always "none" and
  the walk runs to the top of the function, destroying locals the jump never
  passed. A `break` stops at the scope enclosing the loop, so the loop's own body
  scope is included and the surrounding scope survives. `?` is built at the `?`
  instead of in the pass, and not only for that reason: it must see only the
  locals that exist at that point, since a local declared *after* the `?` has an
  uninitialised slot on the path the `?` takes.
- **`lib/string.z`'s `StringBuilder`** becomes a `struct` owning a `Vec<byte>`,
  which removes the `str_buf_append` special case in the runtime entirely.

### Slice 4 — threads and channels (3–4 weeks)

Depends on slice 3 and is small because of it.

- `thread(fn)` spawns, `Chan<T>` is unbounded to start, `send` and `recv` with
  `recv` returning a `Result` so a closed channel is a value rather than a
  panic.
- `Rc`/`Arc` split, with `Send` as a compile-time property computed from a
  type's fields — the same structural rule as "is this type owning".
- `extern` becomes `unsafe extern`, and a diagnostic points at every
  declaration that is not marked.
- The runtime gains a thread pool, or one thread per `thread()` call if that is
  measurably cheaper. **Decision deferred to slice 4**, because a thread-pool
  design is a throughput question and the first version's job is to be correct.

### Slice 5 — port the tests and the standard library (2 weeks, parallel)

Runs alongside 1–4 rather than after, because a golden suite that has not been
ported yet is the thing that catches slice 3's destructor paths being wrong.

- All 71 golden tests and 66 error tests rewritten to the v2 surface.
- `lib/collections.z` and `lib/string.z` become structs with destructors.
- `make test-all` at `-O0`–`-O3` stays the gate, for the reason in
  [LANGUAGE-v1.md](LANGUAGE-v1.md#optimization-levels--command-line): a pass
  that only runs at `-O2` miscompiled once already.

## Memory safety: what is and is not guaranteed

The honest version, since a guarantee that needs a footnote is a guarantee that
will be misread.

**Guaranteed, with a compile error on violation:**

- Reading a moved-from variable.
- Using a reference after the value it borrows is destroyed.
- Indexing an array or `Vec` out of range (`--bounds` is on by default in v2;
  see the command line).
- Destroying a value twice.
- Sending a value to a thread that still holds it, or sending a non-`Send` one.
- Assigning to a `const`, or initializing a `const` twice.

**Guaranteed, at run time, with a message and a non-zero exit:**

- A failed allocation.
- A channel operation on a closed channel.
- A leak, when the runtime can see one at exit.

**Not guaranteed:**

- A raw pointer's lifetime. `unsafe` gets `T*` back and the programmer owns it.
- A cycle. It leaks.
- Memory a C function retains across an `unsafe extern` boundary. The compiler
  cannot see it and does not try.
- Anything the `unsafe` block asks the language to stop checking. That is what
  it is for, and it is marked at the point of use.

**No data races** on any value the compiler tracks, with the `unsafe extern`
footnote from decision 3.

## Lexical

**[now]** Comments: `// line`, `/* block */`. Identifiers:
`[A-Za-z_][A-Za-z0-9_]*`. `_` is a digit separator in numbers and carries no
meaning.

**[now]** Integer literals: decimal, or `0x`/`0o`/`0b`. Float literals: digits,
an optional `.` and fraction, an optional `e`/`E` exponent with a sign. A `.`
begins a fraction only when a **digit** follows it, so `21.Twice()` is a member
access. A float literal too large to represent saturates rather than erroring.

**[now]** String literals: `"..."` with `\n \t \r \0 \\ \"`, `\xNN`, and
`\uXXXX` encoded as UTF-8. An unrecognized escape is an error. `[new]` Raw
strings, `r"..."`, for a path or a regex, so escaping stops being a puzzle.

**[cut]** `$"..."` string interpolation is removed. It desugars to a chain of
`+`, and a reader who sees `$"n = {x}"` has to know that to know what allocates.
**[new]** In v2, interpolation is a `format` call: `format("n = {}", x)`. It is
one call, the `{}` are not expressions in the lexer, and it is the one function
that knows how to turn a value into bytes.

```
var n = 3;
print(format("n = {}, sq = {}", n, n * n));    // n = 3, sq = 9
```

**[new]** Keywords added: `auto`, `move`, `clone`, `namespace`, `pub`,
`unsafe`, `thread`, `send`, `recv`, `Chan`, `Rc`, `Arc`, `Box`. `auto` and `move`
are in the compiler now, the rest are not. **[now]** Kept:
`int bool string float void var const new struct enum class match this if else
while for foreach in return break continue true false null extern export virtual
override fn method import`.

**[now]** Operators: `+ - * / %`, `== != < <= > >=`, `&& || !`, `& | ^ ~ << >>`,
`= += -= *= /= %= &= |= ^= <<= >>=`, `++ --`, `( ) { } [ ] ; , .`. **[new]**
Added: `->`, `:`, `~` (destructor name), `::` (namespace scope).

**[now]** `s[i]` is the byte at `i` as an `int` in `0..255`, and `s[a..b]` is
the half-open byte range with either end optional and both clamped. Lengths are
byte counts throughout: Z has no character type, so in UTF-8 one character is
several bytes. This is stated once and applies everywhere, including in the
bounds-check message.

## Types

**[now]**

| Type | Meaning | Representation |
|------|---------|----------------|
| `int` | 64-bit signed integer | machine word in `rax` |
| `bool` | `true` / `false` | 0 or 1 |
| `float` | IEEE-754 binary64 | 8 bytes; C's `double` |
| `string` | immutable bytes with a length | pointer to the bytes; `{ len, cap }` header at `ptr[-16]` |
| `T*` | raw pointer to `T` | machine word; `unsafe` only in v2 |
| `T&` | **[new]** a borrow of a `T` someone else owns | machine word; never stored |
| `T[]` | **[cut]** fixed heap array | superseded by `Vec<T>` |
| `struct S` | user-defined value type | inline in the frame; fields at byte offsets |
| `class C` | **[now]** heap object with a vtable | pointer; first word is the vtable |
| `void` | no value | — |
| `null` | the null pointer literal | `0`; assignable to any pointer |
| `fn(P...) -> R` | function pointer | machine word: the code address |
| `method(P...) -> R` | bound method pointer, from `&obj.M` | machine word: a pointer to a cell `{ code, receiver }` |
| `closure(P...) -> R` | **[now]** a closure value | pointer to a cell `{ code, env }` |

**[new]** `Vec<T>` is a struct `{ T* data; int len; int cap; }` with a
destructor. `Chan<T>` is a struct holding a queue and a lock. `Box<T>` is a
single owning heap value. `Rc<T>` and `Arc<T>` are reference-counted handles,
differing only in whether the count is atomic.

Pointers, references and arrays compose (`int**`, `int*&`). Arithmetic
(`+ - * /`) works on `int` and `float`; `%` is `int`-only. `+` concatenates
when either side is a `string` — **[cut]**, see `format` above. Comparisons
yield `bool`. `&&`/`||`/`!` are `bool`-only and short-circuit; a `float` is not
falsey, not even `0.0`.

**[now]** `& | ^ ~ << >>` and their compound forms work on the full 64-bit `int`
and wrap on overflow. Shift counts follow C. Division by zero traps.
**[new]** Overflow is a **diagnostic** in a `const` initializer and wraps at run
time, unchanged from v1 — a run-time overflow check would cost a branch on every
arithmetic operation, which is the opposite of zero-cost.

### The one conversion Z performs on its own

**[now]** `int` widens to `float` implicitly, and in no other direction. Every
`int` is exactly representable as a binary64, so the conversion cannot lose
anything. The reverse is never implicit: `(int)f` truncates toward zero, and
assigning a `float` to an `int` is a compile error.

## Structs

**[now]**

```
structDecl := "struct" IDENT "{" member* "}"
member     := type IDENT ";"                       // field
            | type IDENT "(" params ")" block      // method
            | type IDENT "=>" expr ";"             // get-only property  [cut]
            | type IDENT "{" "get" ";" ("set" ";")? "}"  // auto-property [cut]
structLit  := "new" IDENT "(" expr ("," expr)* ")"
destructor := "~" IDENT "(" ")" block
```

- Structs are value types laid out inline with padding to each field's
  alignment. `new S(a, b, ...)` initializes the real fields positionally.
- Struct assignment copies the whole value. `var q = p` is a copy — **[new]**,
  for a type with no owning field. For a type that owns something, `var q = p`
  is **[cut]** and `var q = move(p)` is the transfer.
- Functions and methods returning a struct use the SysV **hidden-pointer**
  return convention (a hidden first parameter holds the result buffer).
- Methods get an implicit `this` receiver, a hidden first pointer parameter.
  Unqualified field names inside a method resolve to `this.<field>`. Symbols
  are mangled `Struct__method`.
- **[cut]** Auto-properties. A field is a field; a computed value is a method.
  `p.x` and `p.X()` are not interchangeable, and not being able to tell which a
  name is from its spelling is the bug this removes.
- **[new]** A `~S()` destructor. Runs at scope end, after any `~S()` body and
  before the fields are destroyed.

```
struct File {
    int fd;
    File(int f) { fd = f; }
    ~File() { close_fd(fd); }         // runs, then the fields go
}
```

## `Result<T,E>` and `?`

**[now]**

```
Result<int, string> parse(string s) {
    if (s == "42") { return Ok(42); }
    return Err("not a number: " + s);
}

Result<bool, string> positive(string s) {
    var n = parse(s)?;     // an Err here returns from this function
    return Ok(n > 0);
}
```

A `Result` is a two-variant union, so `match` handles it, the exhaustiveness
check applies, and the payload is bound by name. Both sides must be one machine
word, which keeps every `Result` sixteen bytes: a tag, then one payload word.

`?` yields the `Ok` payload and returns the `Err` from the function, so a chain
of fallible calls reads as straight-line code. `?` shares a token with the
ternary and the type decides which: a `Result` is never a valid condition.

**[new]** `?` runs the destructors of everything the function owns before it
returns. This is the requirement in decision 2 and the reason a `?` in a
function holding a `Vec` is worth the compile time.

**[new]** `Result` is how every fallible runtime operation reports: `recv` on a
closed channel, `free` that did not free, and a `Box<T>` allocation that failed.

## Enums / sum types & pattern matching

**[now]** Unchanged.

```
enum Shape { Circle(int r), Rect(int w, int h), Point }
string describe(Shape s) => match s {
    Circle(r) => "circle " + r,
    Rect(w, h) => "rect " + w + "x" + h,
    Point      => "point"
};
```

A union is a tagged value: an `int` discriminant at offset 0 plus each variant's
payload. `match` dispatches on the discriminant, binds the active variant's
payload, and runs the matching arm. **The match must be exhaustive** — every
variant covered, or a `_` wildcard — or it is a compile error. Adding a variant
later makes every non-exhaustive `match` fail to compile.

## Ternary

**[now]** `cond ? a : b`, right-associative, condition must be `bool`, both
branches the same type, only the taken branch evaluated.

## Operator overloading

**[now]** A struct defines `op_<Name>` methods. **[new]** A class does too.

```
struct Vec2 {
    int x; int y;
    Vec2 op_Add(Vec2* o) { return new Vec2(x + o.x, y + o.y); }
    Vec2 op_Mul(int k)   { return new Vec2(x * k, y * k); }
    bool op_Eq(Vec2* o)  { return x == o.x && y == o.y; }
}
```

**[cut]** `operator` C++ spelling is not adopted; `op_Add` reads the same in the
grammar and does not need the lexer to know the operator set.

## Declarations

**[now]**

```
decl        := varDecl | constDecl | funcDecl | nestedFunc | statement
varDecl     := ("var" | "auto") IDENT "=" expr ";"                 [new: auto]
             | type IDENT ("=" expr)? ";"
constDecl   := "const" type IDENT "=" expr ";"
nestedFunc  := type IDENT "(" params? ")" block
type        := ("int" | "bool" | "string" | "float") "*"* "&"? "[]"*
             | "Result" "<" type "," type ">"
             | "interface" IDENT "{" ifaceMember* "}"
             | ("fn" | "method" | "closure") "(" typeList? ")" "->" type
             | "Vec" "<" type ">" | "Box" "<" type ">" | ...        [new]
funcDecl    := type IDENT "(" params? ")" block
             | type IDENT "(" params? ")" ";"
externDecl  := "unsafe" "extern" type IDENT "(" params? ")" ";"     [new: unsafe]
exportFunc  := "export" type IDENT "(" params? ")" block
params      := param ("," param)*
param       := type IDENT
```

**[now]** `var` infers the type from the initializer. Variables must be
initialized. Redeclaring in the *same* scope is an error; shadowing an outer
variable in a nested block is allowed. A statement beginning with a type is a
declaration, so `foo bar = 1;` reports `unknown type 'foo'`.

**[now]** A `const` requires an explicit type, is visible only from its
declaration onward, and occupies no frame slot. A top-level `const` may sit
next to `main`.

**[now]** A declaration without a body declares a function whose definition
appears later. `export` means the body is here and the symbol keeps the name as
written, emitted `.globl` so C can call it. Every other Z function is emitted
under a private `z$` symbol so it cannot collide with a libc name or a word the
assembler reserves.

**[now]** A function may take at most 16 parameters; past the argument registers
the surplus goes on the stack. A struct or union return spends a parameter slot
on the hidden result pointer, so such a function is capped at 15. A method is
capped at 15 because the receiver is a parameter too, 14 when the method
returns an aggregate.

**[new]** `namespace N { ... }` and `N::name`. Scoping is real, so two imported
files can both define `helper` and the program can say which it means. This is
v1's M13, which the old roadmap called out as a trap to do before any package
manager: a package manager over a single global namespace distributes
collisions instead of fixing them.

**[new]** `pub` marks a declaration visible outside its namespace; unexported
by default. An unexported name used from another namespace is a diagnostic, not
a silent pass.

## References and ownership

**[new]** This is the v2 core. See decision 1.

```
var a = Vec<int>();          // a owns a buffer
a.push_back(1);
var b = move(a);              // b owns it; a is spent
a.push_back(2);               // error: 'a' has been moved from
var c = b.clone();            // an explicit deep copy
```

- **`move(x)`** transfers ownership. The source becomes spent; reading it is a
  compile error. `move` is a keyword, not a function, so `move(x)` is never a
  call someone can overload.
- **`x.clone()`** is the explicit deep copy. There is no implicit copy of an
  owning type, so no line quietly costs an allocation. `clone` on a non-owning
  type is a copy, and is allowed.
- **A type is owning** if it has a destructor or an owning field. The compiler
  computes this, so it cannot be declared wrong. A non-owning type is copied on
  assignment.
- **`T&` is a borrow.** It cannot be stored in a struct, put in an array or
  `Vec`, captured by a closure, or returned — except from an `&self` parameter,
  where the caller already holds the owner alive. A `const T&` cannot be written
  through; the compiler tracks the constness of the borrowed place.
- **`T*` is a raw pointer, and it is `unsafe` to dereference one in v2.** It
  exists for C interop and for the two idioms that genuinely need it. A `T*` can
  dangle; that is why reading through one is inside `unsafe`.

```
unsafe {
    var p = malloc_raw(64);
    *p = 7;
    free_raw(p);              // returns a Result: it freed, or it did not
}
```

**Why `T*` is not banned outright:** C interop passes `string` as
`const char *` and `extern` functions take pointers, and a language with C
interoperability that cannot express a pointer is a language whose interop is a
special case at every boundary — which is how the 16-byte-vs-registers problem
in [Interoperating with C](#interoperating-with-c) happened. `T*` is kept and
marked, rather than kept and unmarked.

## Closures

**[now]** A lambda is written with `=>` and is an expression.

```
var add = (int a, int b) => a + b;
var counter = () => { n = n + 1; return n; };
```

A function returning a lambda declares `closure(params) -> ret`, kept distinct
from `fn` because the hidden environment changes the calling convention.
Assigning a lambda to an `int` is an error, and so is declaring a function that
returns a lambda as `fn`.

**[now]** A lambda reads any variable in scope where it is written, and the value
is copied into the closure when the lambda is created, so a closure that
outlives its frame keeps working. A captured variable is boxed, so every closure
over the same variable sees each write.

```
closure() -> int counter() {
    var n = 0;
    return () => { n = n + 1; return n; };
}
var c = counter();
print(c());   // 1
print(c());   // 2
```

**[new]** The box becomes an `Rc` cell, so it is freed when the last closure over
it is gone rather than at the next collection. The counter example above is
unchanged and its boxes are reclaimed; a cycle of closures over each other is a
leak, as it is in any reference-counted scheme.

**[cut]** A lambda may not be written inside another lambda. That limit was
there because the environment builder assumed one level; with captures in `Rc`
cells the restriction is no longer load-bearing and lifting it is a small
change.

## Pointers, arrays and `Vec<T>`

**[now]** `&x` yields a reference to an lvalue; `*p` reads and writes through a
pointer.

**[cut]** `new T[n]` and `T[]` as a fixed heap array. `Vec<T>` replaces both, and
`&a[0]` stops being the way to pass part of an array, because `Vec<T>&` is the
way and it knows its length.

**[now]** `foreach` desugars to an index-based loop, so it needs a length.
**[new]** `for (auto& x : v)` works over `Vec<T>`, `Set<T>` and `string`, and
`foreach (var x in v)` remains as sugar for it. Every one of those knows its own
length, which is what the index-based desugaring needed a header for.

```
var v = Vec<int>();
v.push_back(3);
v.push_back(4);
for (auto& x : v) { x = x * 2; }     // in place; auto& is what makes it in place
print(v[0]);                          // 6
print(v.size());                       // 2
```

`v[i]` is **always** bounds-checked in v2. **[new]** v1 had this behind
`--bounds` because an unchecked read is faster; with a memory-safe default that
is the wrong trade, so the check is always on and `--no-bounds` turns it off
where the cost is measured and the code is trusted.

### `Vec<T>`

**[new]** A `Vec<T>` is a `{ T* data; int len; int cap; }` struct with a
destructor. It is a value: `var b = a` is a compile error, and
`var b = move(a)` transfers. The v1 version was a `class`, so `var b = a` was an
alias — which is the trap a language with value-semantics structs invites.

| | |
|---|---|
| `push_back(v)` / `push(v)` | Append. Amortized O(1). |
| `pop_back()` | Remove and return the last. `Result<T, string>` on empty, not a panic. |
| `size()` | Element count. |
| `capacity()` | Allocated elements. |
| `empty()` | `size() == 0`. **[cut]** `isEmpty`. |
| `at(i)` | Bounds-checked element. |
| `reserve(n)` | Grow to hold `n`. |
| `clear()` | Empty it, keeping the buffer. |
| `clone()` | Deep copy. |
| `data()` | The raw pointer, `unsafe` to dereference. |

## Statements

**[now]**

```
statement := block | ifStmt | whileStmt | forStmt | foreachStmt | rangeFor
           | returnStmt | breakStmt | continueStmt | varDecl | exprStmt
ifStmt     := "if" "(" expr ")" statement ("else" statement)?
whileStmt  := "while" "(" expr ")" statement
forStmt    := "for" "(" (varDecl|exprStmt)? ";" expr? ";" expr? ")" statement
foreachStmt:= "foreach" ("var"|"auto") IDENT "in" expr ")" statement
rangeFor   := "for" "(" ("var"|"auto") "&"? IDENT ":" expr ")" statement  [new]
returnStmt := "return" expr? ";"
block      := "{" statement* "}"
```

`if`/`while`/`for` conditions must be `bool`. `++`/`--` desugar to `x = x ± 1`.
`break` leaves the innermost loop and `continue` starts its next iteration; in
a `for`, `continue` jumps to the step. Both are a compile error outside a loop.

**[new]** `while (let v = recv(ch)) { ... }` binds `v` for the body and is false
when `recv` yields `Err`, so a channel loop reads as a loop rather than as a
loop plus an unwrap.

## Expressions

**[now]** Precedence, loosest to tightest:

1. assignment `= += -= *= /= %= &= |= ^= <<= >>=`
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
13. primary: literals, `new C(args)`, `(` expr `)`, variable, `print(expr)`, a
    built-in, a user function, postfix `[i]`, `->`, `++`, `--`

**[now]** `&f` on a function name yields a `fn` typed by that function's
signature; calling it checks the argument count and types. `&obj.M` on a class
yields a `method` value: a pointer to a cell holding the code address and the
receiver, so the receiver stays alive as long as the pointer. A virtual method
binds through the object's vtable.

**[now]** The **intrinsics** are `abs(x)`, `min(a,b)`, `max(a,b)`,
`clamp(x,lo,hi)`, `sqrt(n)`, `sin(a)`, `cos(a)`, all on `int` and returning
`int`. `sqrt` is an exact integer root; `sin`/`cos` are fixed point with a full
turn of `1 << 30`. **[new]** `float` overloads of all of them are added, because
a language with `float` and an integer-only `sqrt` makes everyone write the
conversion by hand.

**[now]** `print` is a builtin: `print(int)`, `print(bool)`, `print(float)`,
`print(string)`. A float prints with `%g`, so `1.5` is `1.5` and `100.0` is
`100`. This is the same formatting `format` uses, so the two never disagree.

**[cut]** `+` string concatenation. **[new]** `format(fmt, args...)` and
`format_int(n)`, `format_float(f)`, `format_bool(b)`. A `string` is a value and
`s = s + x` is a copy; saying so in one call is clearer than a `+` that means
something different depending on its operands, and it gives `{}` somewhere to
live that is not the lexer.

## Functions & entry point

**[now]** A program is a sequence of top-level declarations and statements.
`import "path.z";` splices a file's declarations in ahead of the importing
file's. If a function named `main` is defined, it is the entry point; combining
it with top-level statements is an error. Otherwise top-level statements are
wrapped in a synthesized `main`.

**[new]** Top-level statements are wrapped in a synthesized `main` whose
destructors run, so a `Vec` built at top level is freed at exit.

## Threads and channels

**[new]**

```
threadName := "thread" "(" expr ")"                 // spawn, returns a Thread
chanDecl   := "Chan" "<" type ">" "(" ")"
send       := "send" "(" expr "," expr ")"
recv       := "recv" "(" expr ")"                   // -> Result<T, string>
join       := expr ".join" "(" ")"
```

```
var ch = Chan<int>();
thread(() => {
    for (var i = 0; i < 1000; i = i + 1) { send(ch, i); }
    close(ch);
});
var total = 0;
while (let v = recv(ch)) { total = total + v; }
print(total);                       // 499500
```

- **A value belongs to one thread; sending moves it.** A second send of a value
  that is still owned is a compile error, and so is sending a non-`Send` value.
- **`Send` is computed from a type's fields**, the same way owning-ness is. A
  struct is `Send` if its fields are, and an `Rc` is not `Send` while an `Arc`
  is. A user type with an `Rc` field is not `Send`, and the diagnostic says
  which field.
- **`recv` returns a `Result`**, so a closed channel is a value and the loop ends
  through the type system rather than a sentinel.
- **A channel is the synchronization.** A shared counter is a `Chan<int>`, and
  there is no `Mutex` to get wrong.
- **`Rc` is not `Send`, `Arc` is.** A `Send` value must be moved to cross a
  thread, and a `Sync` value may be shared by reference. Both are structural.
- **[new]** The guarantee stops at the C boundary. `unsafe extern` hands the
  compiler nothing; see decision 3.

## Interoperating with C

**[now]** Both directions are declared in Z source, and the linker is invoked
with whatever extra arguments follow the source file.

```
extern int c_add(int a, int b);          // body in C      [now: unsafe in v2]
export int z_triple(int v) { ... }       // body here, callable from C
```

Type mapping: Z `int` ↔ C `long`, `bool` ↔ an `int` that is 0 or 1, `string` ↔
`const char *`, aggregates ↔ a pointer to them. A struct cannot be returned by
value across the boundary: Z returns every aggregate through a hidden result
pointer, whereas the C ABI returns aggregates of 16 bytes or fewer in
registers.

**[new]** `extern` requires `unsafe`:

```
unsafe extern int c_add(int a, int b);
```

The `unsafe` is not ceremony. It is the line saying *the compiler cannot see
what this function does with the pointers it is given*, which is exactly what
is true, and it is what makes the concurrency guarantee in decision 3 hold
without a footnote: an `extern` is a place where the guarantee ends, and it says
so where it ends. A diagnostic points at every `extern` that is not marked.

## Generics (monomorphization)

**[now]** Functions take type parameters and are compiled by monomorphization:
each distinct set of concrete type arguments produces a specialized native
copy. There is no runtime generic machinery, no boxing, no vtables.

```
T max<T>(T a, T b) { if (a > b) { return a; } return b; }
U pick<T, U>(T a, U b) { return b; }
int total<T>(Vec<T>* v) { var s = 0; for (auto& x in v.*) { s = s + x; } return s; }
```

- Type arguments are **inferred** from the call site; the declaration must appear
  before its uses.
- Parameter types may be `T`, `Vec<T>`, or `T*`.
- A generic function that is never called is never emitted — which is most of
  the fast-compile property.
- The body is validated with type-parameter placeholders, then each
  instantiation is re-type-checked against the concrete types, so a bad
  instantiation is a compile error.

**[new]** Generic **types** work, not just generic functions: `Vec<T>`,
`Chan<T>`, `Box<T>`, `Rc<T>`, `Arc<T>`. v1 had generic classes already, since
`class Vec<T>` compiles; the addition is that a generic type may be returned by
value and owns through its parameters.

**[new]** Template parameter constraints, for the "polymorphism without
inheritance" job v1's inheritance did:

```
T sum<T: Addable>(Vec<T>* v) { ... }
```

**[cut]** C++'s `template<typename T>` block syntax. `T name<T>(args)` reads the
same and the grammar stays one production.

## Interfaces

**[now]** Unchanged, and after slice 2 it is the only way to collect unrelated
types.

```
interface Shape {
    int Area();
    string Name();
}

struct Square { int side; int Area() { return side * side; } string Name() => "square"; }
struct Rect   { int w; int h; int Area() { return w * h; } string Name() { return "rect"; } }

Shape a = new Square(5);
print(a.Area());        // 25
print(a.Name());        // square
```

An interface value is a **pointer** to a two-word cell, `{ itab, receiver }`, so
it is one scalar: it passes, stores, returns and compares like a pointer, and
`null` is meaningful. The itab is a static array of code pointers, one per
required method, **in the interface's declaration order** — that order is the
contract. A **struct** is copied to the heap when it becomes an interface value,
because the cell outlives the frame; a **class** is already a pointer.

A class must declare every method it offers to an interface `virtual`. A struct
has no such requirement, since its methods are reached directly. The method has
to match the signature, and a diagnostic says which of the two went wrong.

**[now]** A non-exhaustive `match` is a compile error, and adding a variant later
makes every non-exhaustive `match` fail to compile. Unchanged from v1 — the
exhaustiveness check is one of the better pieces of the front end and nothing in
v2 gives a reason to weaken it.

**[new]** An interface value holding a `struct` receiver still copies the struct
to the heap, because the cell outlives the frame the value was in. This is the
one place a `struct` becomes a heap allocation without saying so, and it stays
visible in the ABI rather than becoming a hidden box.

## Classes and virtual dispatch

**[now, minus inheritance]** A `class` is a heap-allocated reference type with a
vtable. Structs are value types; classes are reference types.

```
class Shape {
    virtual int Area() { return 0; }          // dispatched dynamically
    virtual string Name() { return "shape"; }
}
class Square {                                // no `: Shape`
    int side;
    int Area() { return this.side * this.side; }
    string Name() { return "square"; }
}
```

- Objects are allocated with `new C(args)`, which returns a `C*`. Each object
  stores a vtable pointer as its first word; `this.field` accesses instance
  fields.
- A **constructor** is a method named like the class, `C(params) { ... }`.
- `virtual` introduces a dispatch slot. Calling a virtual method dispatches
  through the receiver's vtable.

**[cut]** Inheritance, and with it `class B : A`, `override`, `base(args)`, and
derived-to-base upcast. A class can still be `virtual` and dispatch; it just
cannot be a base.

**Why:** inheritance is where C++ spends the most and gets the least. A
hierarchy is a set of related types, and `interface` says that without a
vtable-per-class, an upcast that can be wrong, a `base()` call that can be
forgotten, and the slicing problem. What is lost is a derived type that *is* a
base everywhere; what is kept is that a `virtual` method on a single hierarchy
of your own still dispatches, and that a `Vec<Shape>` of mixed `class` and
`struct` types works.

**[new]** A `class` is owning, so it has a destructor and is moved rather than
copied. `new C()` gives a `C*` in v1; in v2 a `class` is a `Box<C>` and
`Box` makes the ownership visible at the type. v1's raw `C*` becomes `unsafe`.

## Diagnostics

**[now]** Unchanged, and this is the part of v1 that is worth more than the
lexer. A diagnostic carries three things: what the reader was probably trying to
write, where they were when they wrote it, and what the constraint actually was.

- **"Did you mean"** over the names in scope, by edit distance. A wrong-case
  name always suggests, an extension outranks a one-edit match, and the candidate
  list is split by what the name is — a *type* position never suggests a
  function.
- **The enclosing function is named** in every message. A message inside a
  lambda or a monomorphized generic also says *that*, and points at the line the
  lambda started on.
- **The constraint, not only the violation.** An argument type error and an
  arity error both print the declaration they violated.
- **Cascades are suppressed.** `print(ghost)` reports the undefined name, not
  also that `print expects 'int' but got <null>`.
- **Colour** on a terminal, off when redirected or piped, forced with
  `--color`, and off under `NO_COLOR`.
- **`--error-format=human|gcc|json`.** `gcc` is one line per problem and per
  note, which is what an editor's error parser wants. `json` carries a stable
  code, an explicit span in both line/col and byte offset, and the notes inline.

**[new]** Every new safety diagnostic names the rule and the fix, in that order,
because a use-after-move with no explanation is a message people learn to
ignore. The shape is fixed:

```
error: 'a' has been moved from
  --> main.z:7:5
   |
 7 |     a.push_back(2);
   |     ^ cannot use a moved-from value
   |
note: 'a' was moved at main.z:6:5
note: assign into it instead, or use `var b = move(a);` to transfer ownership
```

## Runtime & allocator

**[now]** A small C runtime embedded in the compiler and linked into every
program. Provides `z_newarray` (heap arrays with a length header), `z_concat`,
`z_itoa`. All heap allocation goes through **a plain allocator**: `z_alloc` is
`malloc` and a failed allocation is fatal with a message, `z_free` is `free`, and
a string's header is stored inside its own block rather than in front of it.

**[cut]** The collector, in slice 3: `z_gc_init`, `gc_mark`, `gc_mark_roots`,
`gc_collect`, `gc_enabled`, `gc_threshold`. Done. What replaces it:

- `z_alloc(size)` / `z_free(p)` — a plain allocator, no collection, no roots.
  **[now]**
- `z_str_free(s)` / `z_array_free(p)` — the two header-aware spellings, which
  subtract the header before freeing and skip a literal, whose `cap` is 0.
  **[now]**, but nothing calls them yet: see [Slice 3](#slice-3--gc-to-raii-and-ownership).
- `z_box` becomes an `Rc` cell with a count.
- Destructors emitted by the compiler run on every scope exit.
- A leak report at exit for allocations the runtime can account for. A cycle is a
  leak and says so.

**Why the collector goes:** it cannot see a pointer in a register the caller
spilled, so it leaks; it cannot move an object, so it gives up compaction; and
its pause is a function of the program's live set, which means a program's
latency is a property of the heap rather than of the code. RAII frees at a point
the programmer wrote down, has no pause, and moves nothing — so a `Vec<T>` is
still a pointer, a length and a capacity, and a `for` loop over it is still a
loop. The trade is cycles, and the trade is stated above rather than discovered
in a leak profile.

## ABI

**[now]** x86-64 Linux, System V AMD64. Integers/pointers/array-ptrs in `rdi,
rsi, rdx, rcx, r8, r9`, return in `rax`. Floats in `xmm0`–`xmm7`, back in
`xmm0`. The two sequences are numbered independently, so `f(1.0, 2)` passes
`1.0` in `xmm0` and `2` in `rsi`. `rbp` is the frame pointer and the stack is
16-byte aligned at every call.

A closure value is a pointer to a cell of two words: the function, and the
environment. Calling it puts the environment in `rdi` and the declared arguments
from `rsi` up. A bound method and a constructor call take a hidden receiver the
same way.

Two consequences of the 6-register limit are enforced rather than silently
miscompiled: the parameter caps in [Declarations](#declarations), and an
`add`/`sub`/`imul`/`cmp` against a constant too wide for a sign-extended `imm32`
is routed through a register.

Locals live at `[rbp-8]` and below. When callee-saved registers are pushed for
register-allocated locals, `rbp` is rebased below them so the two regions cannot
overlap.

**[new]** A `T&` is passed as a plain pointer and is indistinguishable from a
`T*` at the ABI level, which is why its lifetime is a compile-time property and
not a runtime one. A `Vec<T>` is passed as its three fields, so by-value is a
memory copy under the hidden-pointer convention and a move is a field copy.

## Optimization levels & command line

**[now]**

```
z <run|build|asm> <file.z> [-o output] [-O0..-O3] [--bounds] [-g]
                    [-w] [-Werror] [-Wno-<name>] [linker args...]
```

| | |
|---|---|
| `-O0` | naive: every local in memory, no folding, a real `idiv`. A baseline to measure against, and the level to debug codegen with. |
| `-O1` | the default: constant folding and propagation, function inlining, a liveness-based local register allocator, leaf and immediate operand selection, branch-on-flags conditions, in-place compound assignment, and constant division/modulo strength reduction. |
| `-O2` | adds loop-invariant code motion. |
| `-O3` | adds loop unrolling. |
| `--bounds` | **[cut]** replaced by `--no-bounds`. Bounds checks are on by default in v2. |
| `-g` | emit DWARF: a line table, and a symbol table naming every function, its parameters and its frame-resident locals. |

A level only gates passes; it never changes what a program means, and the test
suite runs at all four levels (`make test-all`) because a pass that only runs at
a higher level can miscompile while the default level shows nothing wrong.
Loop-invariant code motion shipped exactly that way once.

## What exists today (v1 appendix)

The compiler in this repository implements v1. Specifically, all of the following
is real and tested today, and stays:

- top-level statements, `var`, `print`, `int`/`bool`/`string`/`float`
- the whole type system: pointers, structs with methods, enums, `Result<T,E>`
- closures, nested functions, function pointers, bound method pointers
- interfaces, classes with vtables, **`override` and inheritance** — the last of
  these is what slice 2 removes
- monomorphized generic functions and generic classes
- a plain `malloc` allocator with a `free` to match it, no collector
- modules via `import`, `extern`/`export` C interop
- the full optimizer ladder, DWARF, the diagnostics system with did-you-mean and
  three output formats
- 172 tests in all — 91 golden, 73 diagnostic, and 8 runtime, interop, warning
  and module tests — with `make test-all` green across four `-O` levels

**Destructors, partially.** `~Type()` is parsed, registered, and expanded into
calls at the end of a scope. What works, and is tested:

- teardown in reverse declaration order when a block ends
- teardown on `return`, unwinding *every* open scope, innermost scope first
- teardown per iteration for a value declared in a loop body
- teardown at the end of a void function that falls off its last statement
- both `class` (the receiver is the value) and `struct` (the receiver is the
  address of the frame slot)
- nothing at all for a type with no destructor and no owning field, and an owning
  local does not draw an unused-local warning, because the destructor call is a
  real use of it
- a function whose body owns something is declined by the inliner, so its drops
  cannot be spliced into a caller's frame

What does not work yet, and so does **not** satisfy the rest of section 2:

- a type with no destructor of its own does not destroy its owning fields; the
  recursive default teardown of this section is not implemented
- `string` is owned, copied deeply, released at scope end, released on overwrite,
  and a temporary is released at the end of the statement that made it. Class
  objects, heap arrays and closure cells are still live at exit: those frees need
  the same treatment `string` got, one type at a time, and a leak is the safe
  direction to be wrong in
- there is no move semantics for class pointers or heap arrays yet, so a plain
  copy of one is still a silent alias. `move` exists and is checked, and it
  clears the source for `string`; the other types are not wired to it
- there is no move semantics for a class pointer or a heap array, so returning
  one of those as a local still destroys the value before the caller can use it.
  `move` fixes it for `string` and is not yet wired to the others

Read [LANGUAGE-v1.md](LANGUAGE-v1.md) for the v1 specification in full, including
the sections this document does not repeat: optimization passes in detail,
inlining rules and what it declines, and the DWARF format.
