# Brine — Design

> Status: v0.x (self-hosted: the compiler is written in brine). This document is the source of
> truth for *why* brine is the way it is. Open questions are marked **[DECISION]**.

## 1. Philosophy

1. **Rules cannot be broken.** Every rule the language states is enforced either by a
   static proof or by a runtime check that can never be turned off — not in release
   builds, not with a flag. There is no `unsafe`, no `-DNDEBUG`, no `--unchecked`.
   The compiler's job is to *prove* checks away. The programmer can't *switch* them off.
2. **Fast.** Compiled ahead of time to native code. No garbage collector, no VM,
   no interpreter. Value semantics with copy-on-write keeps it simple without
   giving up performance.
3. **Natively parallel.** Parallelism is syntax, not a library: `par for`, `par [..]`,
   `par sum(..)`, `par(a, b)`. Multi-process / multi-machine (MPI-style) is a
   built-in `world` module. Data races are compile errors.
4. **Unmatched usability, for humans and agents.** Python-like syntax. One obvious
   way to write things. Every diagnostic has a stable code, a precise span, a
   human explanation, and (where possible) a machine-applicable fix. Every CLI
   command has `--json` output. `brine explain <code>` and `brine guide` are
   built in.
5. **Cross-platform.** macOS, Linux and Windows are first-class. The C
   backend emits portable C11 and uses whatever C compiler is present
   (clang or gcc; MSVC planned).
6. **Clean and easy to understand.** Small language surface. No hidden control
   flow, no implicit conversions, no aliasing surprises.

## 2. The language at a glance

```python
import geometry

struct Particle:
    pos: float
    vel: float

    fn step(ref self, dt: float):
        self.pos += self.vel * dt

fn clamp(x: int, lo: int, hi: int) -> int:
    requires lo <= hi                    # callers must prove this
    ensures lo <= result <= hi           # the compiler proves this
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x

fn main():
    let n = 1_000_000
    let xs = par [float(i) * 0.5 for i in 0..n]      # parallel map
    let total = par sum(x * x for x in xs)            # parallel reduction
    var out = [0.0] * n
    par for i in 0..n:                                # race-free parallel loop
        out[i] = xs[i] + 1.0
    print(f"total = {total:.3f}, clamp = {clamp(15, 0, 10)}")
```

### 2.1 Core rules

| Area | Rule |
|---|---|
| Bindings | `let` is immutable, `var` is mutable. No shadowing inside a function. |
| Types | Static, inferred for locals, explicit on function signatures. No implicit numeric conversion (literals adapt to context). |
| Integers | `int` = 64-bit signed. `i8..i128`, `u8..u128`. Overflow **traps** (checked arithmetic). Narrowing conversions are checked. 128-bit types need a 64-bit target, and their overflow checks are never proved away (the prover's arithmetic is itself 128-bit). |
| Floats | `float` = f64, `f32`. IEEE semantics. |
| Values | Everything has **value semantics**: `let b = a` copies and arguments are passed by value. Arrays and strings are copy-on-write and reference counted, so copies are O(1) until mutated. |
| References | Explicit at both ends: `fn fill(ref xs: [int])` is called as `fill(ref xs)`; methods that change their receiver take `ref self`. `let r = ref x` makes `r` a second name for the variable `x`. References are *second-class*: they can't be stored in structs or arrays or returned, so they can never dangle, and the compiler always knows what they point to (exclusive access is enforced: a `ref` argument cannot alias another argument). `mut x: T` on a parameter means a local copy the function may change. |
| Maps | `{K: V}` hash maps that remember insertion order. Values, like arrays (copy-on-write). Reading a missing key panics; `m.get(k)` returns `Option[V]`. Keys must have a total `==` (no floats). `par` bodies may read maps but not change them. |
| Strings | Immutable UTF-8. `len`, `s[i]` and slices count bytes (`s[i]` is a one-character `str`); `for c in s` walks characters. Character classes and case conversion are ASCII-only. |
| Slices | `xs[a:b]`, `s[a:b]` copy; out-of-range bounds panic (or are proven in range). No negative indexes. |
| Evaluation order | A statement that changes a variable (a mutating method, `ref` argument) can't read that variable anywhere else, so the order arguments are evaluated in can never change the result. |
| I/O | `read_file`, `write_file`, `file_exists`, `getenv`, `run` return `Result`/`Option`/`bool`; failures are values, never crashes. Effects that depend on order (`write_file`, `run`) aren't allowed in `par` bodies. |
| Enums | Tagged unions with named fields per variant; values are immutable and may be recursive. `match` must be exhaustive (checked at compile time, with an example of a missed value). |
| Memory | Deterministic: reference counting (atomic), freed at scope exit. No GC pauses. |
| Generics | `fn f[T](x: T)`, `struct Pair[A, B]`, `enum Option[T]`. Monomorphized: every use with new type arguments is a specialized copy, checked and proven like ordinary code. Type arguments are inferred from arguments and the expected type. |
| Errors | `Result[T, E]` and `Option[T]` from the prelude for expected failures; runtime traps with a source location (`panic`) for bugs. `x?` returns a `None`/`Err` to the caller (no implicit error conversions). `main` may return `Result[(), E]`. |

### 2.2 Proofs ("rules cannot be broken")

Two kinds of obligations:

* **Explicit** obligations are written by the programmer and **must be proven at
  compile time** or the program does not compile:
  * `requires` — precondition; proven at *every call site*.
  * `ensures` — postcondition; proven at *every return*. `result` names the return value.
  * `invariant` — loop invariant; proven on entry and preserved by every iteration.
  * `prove e` — an inline claim.
* **Implicit** obligations come from the language's own safety rules: array bounds,
  division by zero, integer overflow, narrowing conversions, `pop()` on empty arrays.
  They are **proven when possible and otherwise checked at runtime** (and the check can
  never be disabled). `brine check --report` lists every remaining runtime check so they
  can be eliminated by adding facts.

`assert e` is the bridge between the two: a runtime check (always on) that also
becomes a *known fact* for the prover afterwards. When a precondition can't be proven,
the compiler suggests adding an `assert` (runtime) or a `requires` (push the obligation
up to callers).

The prover is a built-in decision procedure for linear integer arithmetic with
booleans (symbolic execution + Fourier–Motzkin with integer tightening). It is
**sound** (never accepts a false claim) but incomplete (it may fail to prove a true
claim — then you add an `invariant`, `requires` or `assert`). Because overflow traps,
integers are reasoned about as mathematical integers: any state the program actually
reaches has exact values.

**[DECISION]** Should explicit claims that the prover can't decide be allowed to fall
back to runtime checks in some "development" mode? Current answer: **no** — that would
be a rule that can be broken. Use `assert` explicitly instead.

### 2.3 Parallelism

| Construct | Meaning |
|---|---|
| `par for i in lo..hi:` | Data-parallel loop over a thread pool. |
| `par [e for i in lo..hi]` | Parallel map / comprehension. |
| `par sum(e for i in lo..hi)` | Parallel reduction (`sum`, `product`, `min`, `max`, `any`, `all`). |
| `par(f(a), g(b))` | Fork-join of independent expressions; returns a tuple. |

**Race freedom is a compile-time rule.** Inside a `par` body, variables from the
enclosing scope are read-only, with one exception: an array declared outside may be
written *only at the loop index* (`out[i] = ...`), and then it may not be accessed at
any other index in the body. Function calls cannot receive outer variables as `ref`.
Since values have no observable aliasing, this is sufficient for data-race freedom.

Reductions are **deterministic**: the chunking depends only on the iteration count,
never on the number of threads, so `par sum` over floats gives the same bits on a
laptop and on a 128-core server.

Threads: `BRINE_THREADS=n` or `brine run --threads n` (default: all cores).

### 2.4 Multi-process / multi-machine (`world`)

```python
fn main():
    let rank = world.rank()
    let size = world.size()
    let local = par sum(float(i) for i in rank..1000000 if i % size == rank)
    let total = world.sum(local)            # allreduce
    if rank == 0:
        print(f"total = {total}")
```

Run with `brine run --ranks 4`. Every program is a valid single-rank program
(`world.size() == 1`) when run without `--ranks`.

API: `rank()`, `size()`, `barrier()`, `broadcast(root, v)`, `sum(v)`, `min(v)`,
`max(v)`, `gather_all(v) -> [T]`, `shift(v, by) -> T` (send to `rank + by`, receive
from `rank - by`, wrapping around), `exchange(xs) -> [T]` (all-to-all: `xs[j]` goes to
rank `j`). Messageable types: `int`, `float`, `bool`, `str`, and arrays of those, nested
to any depth.

**Deadlock freedom (decided: enforced at compile time).** Every `world` operation is
collective, and there is no unmatched `send`/`recv`. Point-to-point patterns are written
with `shift` and `exchange`. A program can then only hang if ranks disagree about which
calls to make, so the compiler checks that they can't (`src/uniform.rs`):

* Every value is classified as *uniform* (the same on every rank) or *varying*. Varying
  sources are `world.rank()`, `clock()`, results of `shift`/`exchange`, and C functions.
  Results of `sum`/`min`/`max`/`broadcast`/`gather_all` are uniform.
* A communication call whose control dependence is varying is an error (E0510). This
  covers `if`/`while`/`for` conditions, `and`/`or`, conditional expressions, early
  `return`/`break`/`continue`, and calls into functions that communicate.
* Arguments that steer communication must be uniform: the `broadcast` root, the `shift`
  distance, and any parameter a communicating function branches on (E0511). Function
  summaries record these requirements, so the check is modular and handles recursion.
* Communication inside `par` bodies is rejected.

Since all ranks run the same program and execute the same sequence of collectives with
the same static types, every receive has a matching send. A rank that *crashes* makes the
others stop with an error instead of hanging.

Transport: full-mesh TCP with a background receiver per peer, so sends never block.
Messages between a pair of ranks are FIFO. Multi-machine runs set `BRINE_RANK`,
`BRINE_WORLD_SIZE` and `BRINE_PEERS=host:port,...` (a `brine launch --hosts` command is
on the roadmap).

## 3. Compiler architecture

```
source ─► lexer (indentation-aware) ─► parser ─► AST
      ─► checker (names, types, mutability, race-freedom)
      ─► verifier (symbolic execution + solver; proves contracts, elides runtime checks)
      ─► C11 codegen (+ brine_rt runtime) ─► system C compiler ─► native binary
```

* **Written in brine** (`compiler/`). It builds itself; `bootstrap/brinec.c` (its own
  generated C) builds it from scratch with only a C compiler.
* **Runtime** (`runtime/brine_rt.h` + `runtime/brine_rt.c`) is embedded in the
  compiler binary: refcounted copy-on-write strings/arrays, checked arithmetic,
  thread pool, deterministic reductions, TCP world transport. Win32 and POSIX code
  paths. It is compiled once per runtime version and cached (`rt-<hash>/`).
* **Memory:** values are refcounted with atomic counts; generated code releases
  every temporary and local at scope exit. `BRINE_LEAKCHECK=1` prints the number of
  live strings/arrays at exit (the test suite requires 0).
* **Debugging:** `brine build --debug` emits `#line` directives so gdb/lldb step
  through `.br` source with brine variable names. `brine debug` launches the debugger.
* **Caching:** identical generated C skips the C compile (`.brine-hash` stamp).

### Why C as the first backend?
It's the most portable target and lets the language design move fast. C is the
*first* backend, not the only one: brine must be able to produce machine code
itself (and/or through LLVM) later. To keep that door open:

* The self-hosted compiler puts a typed, backend-neutral **brine IR** between the
  verifier and code generation (explicit control flow, explicit checks with the
  prover's "proven safe" marks, explicit refcount operations). C becomes one
  backend that reads the IR; a native x86-64/ARM64 backend reads the same IR.
* Nothing above the IR may depend on C: no C-only semantics leak into the checker
  or the verifier.
* The runtime ABI (value layouts, refcounting, panics) is specified independently
  of C, so native code and the C runtime can link together.
* The bootstrap seed (the compiler's own generated C) only exists to build the
  compiler from scratch; it doesn't tie user programs to C.

Speed note: C compiled by clang/gcc at `-O2` is already near the best that
hand-written C gets. A native backend's main wins are compile speed, no C
toolchain dependency, and things C can't express well (guaranteed tail calls,
exact overflow handling, SIMD, debug info). Running faster than C comes mostly
from what brine knows that C doesn't (no aliasing, proven-away checks, race-free
`par`), and the IR is where those facts are kept for every backend.

## 4. Tooling

| Command | Purpose |
|---|---|
| `brine new <name>` / `brine init` | Create a project (`brine.toml`, `src/main.br`, `AGENTS.md`, `.gitignore`). |
| `brine run [file] [--ranks N] [--threads N] [-- args]` | Build and run. |
| `brine build [--release\|--debug] [--emit-c]` | Build a native binary into `target/`. |
| `brine check [--report] [--typed]` | Type-check + verify, no codegen. Fastest feedback loop. `--typed` (or `typed = true` under `[check]` in brine.toml) requires a written type on every variable. |
| `brine test [filter]` | Run `test "name":` blocks, each in its own process, in parallel. |
| `brine fmt [--check]` | Canonical formatting. |
| `brine debug` | Debug build + launch gdb/lldb. |
| `brine explain E0601` | Long-form explanation of any diagnostic. |
| `brine guide` | Print the language quick reference (great for agents). |
| `brine doctor` | Diagnose the toolchain (C compiler, threads, sockets). |

Global flags: `--json` (machine-readable diagnostics, one JSON object per line),
`--color auto|always|never`, `--quiet`.

### Diagnostic codes

| Range | Area |
|---|---|
| `E01xx` | syntax: literals, indentation, operators from other languages |
| `E02xx` | names: not found, defined twice, unknown types/fields/methods, shadowing |
| `E03xx` | types |
| `E04xx` | mutability and references (`let`/`var`, `ref` parameters and arguments, aliasing) |
| `E05xx` | parallel safety (`par` rules) |
| `E0601` | a call's `requires` can't be proven |
| `E0602` | a function's `ensures` can't be proven at some `return` |
| `E0603` | a loop `invariant` might not hold when the loop starts |
| `E0604` | a loop `invariant` might not be preserved by an iteration |
| `E0605` | a `prove` claim can't be proven |
| `E0606` | something not allowed in a specification (e.g. a call with side effects) |
| `E07xx` | driver: missing module, no C compiler, no `main`, C compile/link failures |

Proof failures list the facts the prover knew at that point and the exact goal,
so the fix (a stronger `requires`, an `invariant`, or an `assert`) is usually obvious.

### Agent usability principles
* Stable error codes; every error has a primary span, a "why", and a "how to fix".
* Machine-applicable fixes in JSON (`fixes: [{line, column, end_line, end_column, replacement}]`).
* Deterministic output: same input ⇒ same diagnostics in the same order.
* No configuration needed for anything common. `brine guide` + `AGENTS.md` in every project.
* Syntax chosen to match what models already know (Python), minus the footguns.

## 5. Roadmap
brine follows semantic versioning (see `AGENTS.md`). Until 1.0 every minor release
(0.x) may change the language; 1.0 means a stable language and ABI, and there is a
lot to do before then. The order below is a plan, not a promise.

**Done (0.1):** core language, structs + methods, enums + `match`, generics,
`Option`/`Result`, tuples, arrays, maps, strings (methods, slices), f-strings,
`for a, b in`, `enumerate`, `?`, file and process I/O, modules, contracts + prover,
`par` constructs, `world`, tests, CLI, fmt, explain, guide; the self-hosted compiler.

**Backends:** brine IR and a native machine-code backend (x86-64 first, then ARM64),
so builds don't need a C compiler for generated code; then optimization passes on the
IR, SIMD, GPU offload for `par` loops, MSVC support for the C backend.

**Proofs:** generic bodies checked once against constraints (rather than per
instance), quantifiers in specs (`forall i in 0..n: a[i] >= 0`), array-content
reasoning, nonlinear facts (`i * j`), counterexamples on proof failure.

**Tools:** LSP server, `brine fix` (apply machine fixes), package manager
(`brine add`), `brine launch --hosts`.

**1.0:** stable language + ABI.

### Self-hosting (done)
The compiler was first written in Rust, then ported to brine module by module, with
every stage cross-checked against the Rust compiler (identical diagnostics, prover
reports and generated C on every file, plus mutation fuzzing). The brine compiler
builds itself to a fixed point and the Rust compiler has been retired. Its own
generated C (`bootstrap/brinec.c`) is the seed that builds it from scratch.
