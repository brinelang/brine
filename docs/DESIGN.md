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
5. **Cross-platform.** macOS, Linux and Windows are first-class. Code goes
   through brine's own IR to native x86-64 and AArch64 backends (quick dev
   builds on Windows and Linux) or to portable C11 for whatever C compiler is present
   (optimized builds, every other platform; clang or gcc, MSVC planned).
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
| Strings | Immutable UTF-8. `len`, `s[i]` and slices count bytes (`s[i]` is a one-character `str`); `for c in s` walks characters. Case conversion and character classes follow Unicode (`is_digit` is `0`-`9`). |
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
| `par first(e for i in lo..hi)` | Parallel search: the lowest-index `Some` of `Option` values, or `None`. |
| `par(f(a), g(b))` | Fork-join of independent expressions; returns a tuple. |

**Race freedom is a compile-time rule.** Inside a `par` body, variables from the
enclosing scope are read-only, with one exception: in a `par for` over a range, an
array declared outside may be written *only at the loop index* (`out[i] = ...`), and
then it may not be accessed at any other index in the body. Strided writes
generalize this: `out[k * i + c]` with constants `k >= 1` and `c`, the same `k` for
every write of `out` and offsets `c` that aren't a multiple of `k` apart, never hit
another iteration's element (k·i + c = k·j + c' needs k | c' - c), and reads at
offsets that no write has modulo `k` touch elements no iteration writes. Function calls cannot receive outer variables as `ref`.
Since values have no observable aliasing, this is sufficient for data-race freedom.

Reductions are **deterministic**: they combine in one fixed order that depends only
on the number of elements, never on the number of threads, so `par sum` over floats
gives the same bits on a laptop and on a 128-core server. The elements are split into
*leaves* of 16 consecutive elements; each leaf is folded left to right, starting from
the reducer's start value (`0`, `1`, ...); then the leaves are combined by a balanced
binary tree, level by level: leaves 0 and 1, 2 and 3, ..., then those results in pairs,
and so on (a node without a partner moves up a level unchanged; a leaf whose elements
were all filtered out doesn't take part). Integer `sum` and `product` trap on overflow
of any partial result in this order. Besides being parallel, the tree loses less
precision than a left-to-right sum. Only the scheduling depends on the machine: when
there are few leaves for the threads (a `par sum` of a few expensive calls), each
element runs as a task of its own and the leaves are folded from their values
afterwards, in the same order. `any` and `all` stop starting elements once one
decides the result, like their sequential loop; every element before it still runs.
`par first` is the same search with a deterministic answer: the elements are split
into chunks, each keeps its first `Some`, and the lowest chunk's wins; chunks after
one that found a value stop.

Threads: `BRINE_THREADS=n` or `brine run --threads n` (default: all cores).

**GPUs.** A `par for`, `par [...]` or `par` reduction can also run on a GPU (NVIDIA's
through CUDA, Apple's through Metal), with no annotation and no change in meaning:
the results are the same bits as on the CPU, and a check that fails on the device
panics exactly as it would on the CPU.
* *Which constructs:* the compiler decides. A body can run on a GPU when it (and
  every function it calls) uses only int and float arithmetic up to 64 bits,
  comparisons, branches, loops, structs and tuples, reads of captured scalars and of
  arrays (also arrays of arrays and of structs), writes to `out[i]`, `sqrt`, `abs`,
  `floor`, `ceil`, `round`, `min` and `max`. It can't allocate (no strings, maps,
  enums with payloads or new arrays), print, use `world`, call function values, or
  call `sin`, `cos`, `exp`, `log`, `pow` and the like, whose last bit depends on
  the C library. Apple GPUs have no f64 and Metal has no recursion, so on macOS
  those stay on the CPU too. `BRINE_GPU_LOG=1` says why a construct stayed on the
  CPU.
* *When:* the runtime decides, per launch. It skips constructs under 4096 elements
  and nested ones, and otherwise compares the CPU's time (estimated by running the
  first elements for about 100 µs, then measured) with the device's (guessed from
  that, then measured), plus copies and launches. It uses the device when that is
  at least 25% faster. Getting the device ready takes 0.1-0.7 s, so a construct that
  would gain only later starts it in the background and stays on the CPU until it
  is ready. `BRINE_GPU=off` never uses a GPU, `BRINE_GPU=on` uses one whenever it
  can.
* *How:* the compiler turns the lifted body of each eligible construct into a
  kernel, and the program carries the module: PTX (`compiler/ptx.br`), or Metal
  Shading Language source on macOS (`compiler/msl.br`). The runtime loads the CUDA
  driver (`nvcuda.dll`, `libcuda.so.1`) or the Metal framework when it first needs
  it, and the driver compiles the module for the device. Without a driver or a
  device, programs run on the CPU as before; there is no build-time dependency on
  CUDA or Metal (Metal is reached through the Objective-C runtime, loaded with
  `dlopen`).
* *Same results:* the kernels use the CPU's semantics, not the device's defaults:
  every float operation rounds to nearest even with no fused multiply-add, `/` and
  `sqrt` are exactly rounded, and integer division, conversions and shifts follow
  the runtime's rules. A reduction runs the same leaves and the same tree (each
  launch reduces an aligned subtree of up to 2^24 leaves; the CPU folds the
  subtrees' results). Checks (overflow, bounds, division by zero, contracts) set a
  flag instead of panicking; when it is set, the device's results are dropped and
  the construct reruns on the CPU, which panics with the usual message. Apple GPUs
  flush subnormal f32 values to zero, so on Metal a subnormal operand or result
  sets the flag too, and the construct reruns on the CPU. Results are
  only copied back after a launch succeeded, and a body may only write its own
  elements (`out[i]`), so rerunning is safe.
* *Copies:* every launch copies the arrays the body reads to the device and the ones
  it writes back (arrays are values, so nothing else can see them in between). The
  results of a `par [...]`, and an array that a `par for i in lo..hi` body writes as
  `out[i] = ...` in every iteration (a top-level statement, no `continue` before it)
  and never reads, aren't copied to the device, and only the elements the device
  wrote come back.
  Keeping arrays on the device between constructs is future work, so constructs
  that do little work per byte (a plain sum of an array) stay on the CPU.

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
calls to make, so the compiler checks that they can't (`compiler/uniform.br`):

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
      ─► uniform (deadlock freedom for `world`)
      ─► verifier (symbolic execution + solver; proves contracts, elides runtime checks)
      ─► lowering to brine IR (lower.br)
      ─► x86-64 machine code (x64.br, COFF/ELF objects)    dev and --debug builds on x86-64 Windows/Linux
       └► AArch64 machine code (a64.br, ELF objects)        dev and --debug builds on AArch64 Linux
       └► C11 (ir_c.br) ─► system C compiler               --release, other platforms
      ─► link with the brine_rt runtime ─► native binary
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
* **Debugging:** on x86-64 Windows and Linux and on AArch64 Linux, `brine build --debug` is native with
  DWARF (`dwarf.br`): lines, brine function names (`Point.norm`), and every local
  and parameter by its brine name and type, in its scope. Debug code keeps each
  value in its stack slot (no register allocation, no value caching), so `print`
  is always right. `brine debug` launches gdb (or lldb) and loads
  `runtime/brine_gdb.py`, which prints values the way brine does (`"hello"`,
  `[1, 2, 3]`, `Shape.Rect(w=2, h=3)`) and skips the C runtime when stepping.
  Elsewhere, and with `BRINE_BACKEND=c`, `--debug` goes through C with `#line`
  directives (variables then show as C names, `x_3`).
* **Caching:** objects are named by a hash of what they're made from. C output is
  split into one translation unit per core (by a hash of each function's name, each
  unit declaring only what it uses), compiled in parallel; an edit recompiles only
  the units it touches. An unchanged program skips the link (`.brine-hash` stamp).
* **CPU levels:** an x86-64 `--release` build compiles the program three times,
  for plain x86-64, x86-64-v3 (AVX2) and x86-64-v4 (AVX-512), and
  `runtime/brine_cpu.c` runs the newest one the machine supports. The executable
  runs anywhere and still uses wide vectors where there are some; float results
  are the same in all three (no fused multiply-adds). `BRINE_CPU=base` or `=v3`
  caps the choice at run time.
* **Parallel:** the compiler uses `par` itself: imported modules are parsed a wave
  of imports at a time, and the prover takes every function on its own. Results
  are combined in a fixed order, so the output doesn't depend on the thread count.
* **Timing:** `BRINE_TIMES=1` prints the time of each phase; `BRINE_TIMES=2` also
  names the functions the prover is slow on. `BRINE_BACKEND=c` or `=native` picks
  the backend (a build with `BRINE_BACKEND` set gets its own output folder).

### Backends
The **brine IR** (`compiler/ir.br`) sits between the verifier and code generation:
virtual registers with machine kinds (`i8`..`u128`, `f32`, `f64`, `ptr`, `bool`),
explicit control flow, explicit checks (only those the prover couldn't remove),
explicit reference counting, stack slots and data with relocations. Every backend
reads it:

* **x86-64** (`x64.br`, objects by `obj.br`): writes machine code and a COFF
  (Windows) or ELF (Linux) object directly, in well under a second for the whole
  compiler. It's the default for dev and debug builds there (debug builds add
  DWARF and skip the optimizations). The runtime is still C,
  compiled once and cached, and the system C compiler links.
* **AArch64** (`a64.br`): the same design for 64-bit ARM, writing ELF objects
  (Linux) through `obj.br`, with .eh_frame unwind info and the same DWARF.
* **PTX** (`ptx.br`) and **Metal** (`msl.br`): GPU kernels for the `par`
  constructs that can run on a GPU (§2.3), next to the machine or C code for the
  CPU. `ptx.br` also decides which constructs are eligible, for both. Metal has no
  `goto`, so `msl.br` runs a function with labels as a `switch` in a loop.
  Lowering marks each construct with a descriptor (`brine_kernel`: kernel name, the
  layout of its captured context, a cost estimate) and the runtime picks a side per
  launch.
  `BRINE_DUMP_PTX=file` writes the module (PTX or MSL) when building.
* **C** (`ir_c.br`): one statement per IR instruction. gcc/clang at `-O2`/`-O3`
  make it as fast as hand-written C, so it serves `--release` and every
  platform without a native backend (with `#line` for `--debug` there). It's also the bootstrap
  seed (`bootstrap/brinec.c`).

Rules that keep this working:
* Nothing above the IR may depend on a backend: no C-only semantics leak into the
  checker or the verifier.
* The runtime ABI (value layouts, refcounting, panics, `brine_x_*` entry points for
  helpers that are `static inline` in C) is fixed, so native code and the C
  runtime link together.
* Every backend passes the same end-to-end suite, and the compiler built by each
  one builds itself to the same object.

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

**Done (0.2):** brine IR; native x86-64 backend (COFF and ELF); C from the IR, in
parallel translation units with per-unit caching.

**Backends:** Mach-O objects, so macOS on Apple silicon gets native dev builds; CodeView debug info (for Visual Studio/WinDbg);
register allocation and optimization passes on the IR; a built-in linker and a
prebuilt runtime, so builds don't need a C toolchain at all; SIMD, MSVC support for
the C backend. GPUs (§2.3): arrays kept on the device between constructs, `par(...)`
forks and nested constructs.

**WebAssembly:** `--target wasm` and `web` compile the generated C with wasi-sdk
(wasm32-wasip1); the runtime keeps its 64-bit memory layout, with pointers in
8-byte slots. Next: threads (wasi-threads, Web Workers), so `par` runs in parallel
there too; files in the browser; and a playground page running the compiler
itself, which already builds for WebAssembly.

**TODO: AMD and Intel GPUs**, once there is hardware to test on: Vulkan compute
with SPIR-V kernels (the driver loaded at run time, as CUDA is), exactly rounded
`/` and `sqrt` in the kernels, a capability check for denormals and f64, and a
self-test kernel at startup that turns the GPU off if its results differ from the
CPU's. NVIDIA and lavapipe (Mesa's CPU Vulkan) can test most of it, but not each
vendor's own shader compiler, so it waits for real devices. Intel Macs' AMD and Intel
GPUs are part of it: Metal is used only on Apple GPUs for now.

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
