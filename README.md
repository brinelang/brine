<div align="center">

<img alt="brine" src="docs/assets/banner.png" width="100%">

**A compiled systems language with Python-like syntax, compile-time proofs, and
parallelism built into the language: every core, the GPU, and many machines.**

[![CI](https://github.com/brinelang/brine/actions/workflows/ci.yml/badge.svg)](https://github.com/brinelang/brine/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/brinelang/brine?include_prereleases&sort=semver)](https://github.com/brinelang/brine/releases)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)

[Website](https://brinelang.org) · [Docs](https://docs.brinelang.org) · [Install](#installation) · [Tour](#a-quick-tour) · [Language reference](docs/QUICKREF.md) · [Design](docs/DESIGN.md) · [Examples](examples/)

</div>

---

brine is for programs that need to be fast, parallel and correct at the same
time. You write code that reads like Python. The compiler turns it into native
code, proves what it can about it (array bounds, overflow, your own contracts),
and spreads it across every core you have, or onto a GPU, with results that are
the same bits on any machine.

Parallelism is syntax, not a library. `par for`, `par [...]`, `par sum(...)` and
`par(a, b)` can't race: a data race is a compile error. Reductions are
deterministic, so a float sum gives the same answer on a laptop and on a
128-core server. Big constructs move to an NVIDIA or Apple GPU on their own when
that is faster, with no annotations and no change in results. And every program
can run as many cooperating processes (`brine run --ranks 4`), with deadlocks
ruled out at compile time.

```python
fn index_of(xs: [int], target: int) -> int:
    ensures -1 <= result < len(xs)  # proven by the compiler
    var lo = 0
    var hi = len(xs)
    while lo < hi:
        invariant 0 <= lo <= hi <= len(xs)
        let mid = lo + (hi - lo) / 2
        if xs[mid] == target:  # bounds check proven: removed
            return mid
        if xs[mid] < target:
            lo = mid + 1
        else:
            hi = mid
    return -1

fn main():
    let squares = par [i * i for i in 0..1_000_000]  # runs on all cores
    let total = par sum(float(x) for x in squares)  # same bits on any machine
    print(index_of(squares, 144), total)
```

## Highlights

<table>
<tr><td>⚡ <b>Natively parallel</b></td><td><code>par</code> loops, maps, reductions and fork-join, data-race free by construction. Deterministic reductions. Automatic GPU offload (CUDA, Metal). MPI-style multi-process with the <code>world</code> module.</td></tr>
<tr><td>🔒 <b>Rules can't be broken</b></td><td><code>requires</code>, <code>ensures</code>, <code>invariant</code> and <code>prove</code> are checked at compile time. Overflow, bad indexes and division by zero always stop the program with a clear message, unless the compiler proved they can't happen; then the check is removed. There is no <code>unsafe</code> and no flag to turn checks off.</td></tr>
<tr><td>🚀 <b>Fast</b></td><td>Native code, no garbage collector, no VM. Values are copy-on-write and memory is freed deterministically. Dev builds on Linux and Windows go straight to machine code; <code>--release</code> builds go through an optimizing C compiler.</td></tr>
<tr><td>🐍 <b>Easy to read</b></td><td>Python-like syntax, static types with local inference, structs, enums with <code>match</code>, generics, <code>Option</code>/<code>Result</code> with <code>?</code>. One obvious way to write things.</td></tr>
<tr><td>🤖 <b>Made for humans and agents</b></td><td>One CLI. Every error has a code, a precise span, the reason and a fix. <code>--json</code> output for tools, <code>brine explain &lt;code&gt;</code>, and <code>brine guide</code> prints the whole language on one page.</td></tr>
<tr><td>🛠️ <b>Self-hosted</b></td><td>The compiler is written in brine and builds itself. All it needs is a C compiler.</td></tr>
</table>

## Installation

### Quick install

**Linux and macOS**

```sh
curl -fsSL https://raw.githubusercontent.com/brinelang/brine/main/install.sh | sh
```

**Windows** (PowerShell)

```powershell
irm https://raw.githubusercontent.com/brinelang/brine/main/install.ps1 | iex
```

The installer puts `brine` in `~/.brine/bin` and adds it to your `PATH`. Run it
again to upgrade.

| Variable | Effect |
|---|---|
| `BRINE_VERSION=0.4.0` | install a given version (default: the latest release) |
| `BRINE_INSTALL=~/.brine` | where to install (the binary goes in `$BRINE_INSTALL/bin`) |
| `BRINE_NO_MODIFY_PATH=1` | don't add brine to your `PATH` |

Prebuilt archives are also on the [releases page](https://github.com/brinelang/brine/releases).
To uninstall, delete `~/.brine` and the `PATH` line the installer added.

### Requirements

brine needs a C compiler on your machine: `gcc` or `clang`, or MinGW on Windows.
It uses it to build the runtime, to link, and for `--release` builds. Check
your setup with:

```sh
brine doctor
```

WebAssembly builds (`--target wasm` or `web`) also need
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases): unpack it and set
`WASI_SDK_PATH` to its folder. `brine run --target wasm` runs the result with
Node.js.

GPU offload needs nothing extra at build time. If an NVIDIA driver or Apple
silicon is present when the program runs, brine uses it; otherwise everything
runs on the CPU.

### Supported platforms

| Platform | Prebuilt | Native dev builds | GPU |
|---|:---:|:---:|---|
| Linux x86-64 | ✅ | ✅ | NVIDIA (CUDA) |
| Linux ARM64 | ✅ | ✅ | NVIDIA (CUDA) |
| Windows x86-64 | ✅ | ✅ | NVIDIA (CUDA) |
| macOS Apple silicon | ✅ | via C | Apple GPU (Metal) |
| macOS Intel | ✅ | via C | – |

"Native dev builds" means the compiler writes machine code itself (the whole
compiler builds in under a second). Everywhere else, and for every `--release`
build, it goes through C.

Every platform can also build WebAssembly, for WASI runtimes and for browsers.

### Your first project

```sh
brine new hello
cd hello
brine run
```

```text
     Built hello v0.1.0 [dev] in 2.03s
   Running target/dev/hello
Hello, brine!
[0, 1, 4, 9, 16, 25, 36, 49, 64, 81]
```

### Build from source

The compiler is written in brine, so the first build starts from its own
generated C, the bootstrap seed (see [bootstrap/README.md](bootstrap/README.md)):

```sh
git clone https://github.com/brinelang/brine && cd brine
# Windows: brine.exe and -lws2_32 -lshell32 instead of -lm -lpthread -ldl
cc -std=c11 -O2 -fno-strict-aliasing -w -Iruntime bootstrap/brinec.c runtime/brine_rt.c -o brine -lm -lpthread -ldl
./brine build compiler/main.br -o brine2   # brine rebuilding itself
```

## A quick tour

### Parallel loops, maps and reductions

```python
fn collatz_len(n: int) -> int:
    var x = n
    var steps = 0
    while x != 1:
        x = x / 2 if x % 2 == 0 else 3 * x + 1
        steps += 1
    return steps

fn main():
    let n = 1_000_000
    let lens = par [collatz_len(i) for i in 1..=n]       # parallel map
    let total = par sum(float(l) for l in lens)          # parallel reduction
    let longest = par max(l for l in lens)
    print(f"average steps: {total / float(n):.2f}, longest: {longest}")

    var out = [0.0] * 8
    par for i in 0..8:                                   # parallel loop
        out[i] = sqrt(float(i))

    let (a, b) = par(collatz_len(27), collatz_len(97))   # fork-join
```

| Construct | Meaning |
|---|---|
| `par for i in lo..hi:` | data-parallel loop over a thread pool |
| `par [e for i in lo..hi]` | parallel map |
| `par sum(e for ...)` | parallel reduction: `sum`, `product`, `min`, `max`, `any`, `all` |
| `par(f(a), g(b))` | fork-join of independent expressions, returns a tuple |

Threads default to all cores; `--threads n` or `BRINE_THREADS=n` changes that.

### Data races are compile errors

Inside a `par` body, outer variables are read-only, except that a `par for` may
write its own element of an outer array (`out[i]`). Break that rule and the
program doesn't compile:

```python
fn main():
    let xs = [1, 2, 3, 4]
    var total = 0
    par for i in 0..len(xs):
        total += xs[i]
    print(total)
```

```text
error[E0501]: cannot assign to `total` inside a `par` body
 --> race.br:5:9
  |
5 |         total += xs[i]
  |         ^^^^^ would be a data race
  |
  = note: `total` is shared by all parallel iterations
  = help: a `par for` may only write `total[i]` (its own element); to combine values use `par sum(...)`, `par [...]`, or a sequential loop
```

### Same bits on every machine

Reductions combine elements in one fixed order that depends only on how many
there are, never on how many threads run. `par sum` over floats gives the same
result on 1 thread, 32 threads or a GPU, and it is usually more accurate than a
left-to-right sum. Integer reductions still trap on overflow.

### GPUs, with no code changes

A `par for`, `par [...]` or `par` reduction can run on an NVIDIA GPU (CUDA) or an
Apple GPU (Metal). There is nothing to annotate and nothing to install beyond the
driver:

* **The compiler decides what can run there.** Bodies that use numeric code,
  loops, structs and array reads and writes become GPU kernels.
* **The runtime decides when.** It measures each construct on the CPU, estimates
  the GPU's time including copies, and only uses the GPU when that's faster.
* **The results don't change.** Kernels follow the CPU's floating-point rules
  exactly (no fused multiply-adds, exactly rounded `/` and `sqrt`). If a check
  fails on the GPU (overflow, bounds, a contract), the construct reruns on the CPU
  and panics with the usual message.

```sh
brine run examples/find_signal.br --release              # GPU when it pays off
BRINE_GPU=off brine run examples/find_signal.br --release   # CPU only
BRINE_GPU_LOG=1 brine run examples/find_signal.br --release # where each `par` ran, and why
```

On an RTX 4090, an f32 n-body step runs about 8x faster than on all 32 threads
of a Ryzen 9 9950X3D. See [bench/gpu](bench/gpu/README.md) for the numbers,
including the cases where the CPU wins and `auto` keeps the work there.

### WebAssembly and the web

The same programs compile to WebAssembly, with the same checks and the same
results:

```sh
brine run --target wasm examples/parallel.br           # a .wasm module, run with Node.js
brine build --target wasm examples/parallel.br         # parallel.wasm, for any WASI runtime
brine build --target web examples/parallel.br -o demo  # demo/: index.html, brine.js, program.wasm
python3 -m http.server -d demo                         # then open http://localhost:8000
```

A web build is a folder you can put on any static host. `index.html` shows what
the program prints; replace it with your own page (rebuilding keeps it) and call
`runInWorker` from `brine.js`. It runs the program in a Web Worker, so the page
keeps drawing and the output shows up as it is printed (`run` takes the same
arguments and runs it on the calling thread, e.g. in a worker of your own):

```js
import { runInWorker } from "./brine.js";
const code = await runInWorker("program.wasm", { args: ["10"], stdout: (text) => show(text) });
```

WebAssembly runs on one thread, so `par` constructs run in order (with the same
results) and `world` programs run as one rank. In the browser, a program has no
files and can't start processes.

### Many processes, no deadlocks

The `world` module gives every program MPI-style ranks and collectives:

```python
fn main():
    let rank = world.rank()
    let size = world.size()

    var local = 0
    for i in 0..1_000_000:
        if i % size == rank:
            local += i
    let total = world.sum(local)                 # every rank gets the total

    let all = world.gather_all(rank * rank)      # everyone gets everyone's value
    if rank == 0:
        print(f"{size} ranks, total = {total}, squares = {all}")
```

```text
$ brine run world.br --ranks 4
4 ranks, total = 499999500000, squares = [0, 1, 4, 9]
```

Every communication is collective, and the compiler checks that all ranks make
the same calls in the same order, so a brine program can't hang waiting for a
message that never comes:

```python
fn main():
    if world.rank() == 0:
        let total = world.sum(1)
        print(total)
```

```text
error[E0510]: some ranks might not reach this communication
 --> dead.br:3:21
  |
3 |         let total = world.sum(1)
  |                     ^^^^^^^^^^^^ calls `world.sum`
2 |     if world.rank() == 0:
  |        ----------------- this may differ between ranks
  |
  = note: every rank must make the same `world` calls in the same order, otherwise the program could hang
  = help: move the call out of the rank-dependent branch or loop; compute rank-specific values first, then communicate on every rank
```

### Contracts the compiler proves

```python
fn clamp(x: int, lo: int, hi: int) -> int:
    requires lo <= hi                 # every caller must prove this
    ensures lo <= result <= hi        # the compiler proves this
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x

fn last(xs: [int]) -> int:
    requires len(xs) >= 1
    return xs[len(xs) - 1]            # in bounds: no runtime check

fn main():
    let xs: [int] = []
    print(last(xs))
```

```text
error[E0601]: cannot prove the precondition of `last`
  --> proof.br:16:11
   |
16 |     print(last(xs))
   |           ^^^^^^^^ this call might violate `requires len(xs) >= 1`
11 |     requires len(xs) >= 1
   |              ------------ precondition declared here
   |
   = note: with the values used here, this is always false
   = help: add an `assert` before the call to check it at runtime, or a `requires` to this function so its callers must prove it
```

`brine check --report` lists every runtime check the prover couldn't remove, so
you can add the facts that remove them:

```text
$ brine check examples/contracts.br --report
   Checked contracts in 0.00s — 9 runtime checks proven unnecessary, 0 remain
```

### Structs, enums and errors as values

```python
struct Point:
    x: float
    y: float

    fn dist(self, other: Point) -> float:
        let dx = self.x - other.x
        let dy = self.y - other.y
        return sqrt(dx * dx + dy * dy)

enum Shape:
    Circle(radius: float)
    Square(side: float)
    Dot

    fn area(self) -> float:
        match self:                    # must cover every case
            case Shape.Circle(r):
                return 3.14159 * r * r
            case Shape.Square(s):
                return s * s
            case Shape.Dot:
                return 0.0

fn parse_bit(s: str) -> Result[int, str]:
    if s == "0":
        return Ok(0)
    if s == "1":
        return Ok(1)
    return Err(f"not a bit: {s}")

fn load(path: str) -> Result[int, str]:
    let text = read_file(path)?        # an Err goes back to the caller
    return parse_bit(text.strip())
```

### Tests are part of the language

```python
test "clamp stays in range":
    assert clamp(15, 0, 10) == 10
    assert clamp(-5, 0, 10) == 0
```

```text
$ brine test examples/contracts.br
test clamp stays in range ... ok
test binary search finds every element ... ok

test result: ok. 2 passed; 0 failed; finished in 0.03s
```

## The `brine` command

| Command | What it does |
|---|---|
| `brine new <name>` / `brine init` | create a project |
| `brine run [file] [-- args]` | build and run (`--release`, `--threads N`, `--ranks N`, `--target wasm`) |
| `brine build` | build a binary (`--release`, `--debug`, `-o`, `--emit-c`, `--target wasm\|web`) |
| `brine check` | type-check and prove without building (`--report` lists remaining runtime checks) |
| `brine test [filter]` | run `test "...":` blocks in parallel |
| `brine fmt` | format sources (`--check`) |
| `brine debug` | debug build, then launch gdb or lldb |
| `brine explain E0601` | explain an error code |
| `brine guide` | the whole language on one page |
| `brine doctor` | check your toolchain |

Building, checking and testing take `--json` for machine-readable output, one
JSON object per line. New projects come with an `AGENTS.md`, so coding agents know how to work in
them from the start.

## Examples

| Example | What it shows |
|---|---|
| [hello.br](examples/hello.br) | the smallest program |
| [parallel.br](examples/parallel.br) | `par` maps, reductions, loops and fork-join |
| [contracts.br](examples/contracts.br) | `requires`, `ensures`, `invariant`, and a binary with no runtime checks left |
| [world.br](examples/world.br) | multi-process: collectives, ring messages, all-to-all |
| [shapes/](examples/shapes/) | structs, enums, methods, `match` and modules |
| [find_signal.br](examples/find_signal.br) | finding GPS satellites in noise: proofs and the GPU together |

## Documentation

* [brinelang.org](https://brinelang.org): the brine website
* [docs.brinelang.org](https://docs.brinelang.org): the documentation online
* [docs/QUICKREF.md](docs/QUICKREF.md): the whole language on one page (also `brine guide`)
* [docs/DESIGN.md](docs/DESIGN.md): philosophy, architecture, roadmap and open decisions
* [CHANGELOG.md](CHANGELOG.md): what changed in each release

## Status

brine is young (v0.x): the language and tools still change between minor
releases. It runs on Linux, macOS and Windows, and its compiler, written in
brine, builds itself to a fixed point on every CI run.

Next on the [roadmap](docs/DESIGN.md#5-roadmap): Mach-O output for native dev
builds on Apple silicon, a built-in linker so builds don't need a C toolchain,
keeping arrays on the GPU between constructs, AMD and Intel GPUs through Vulkan,
quantifiers in contracts, an LSP server and a package manager.

## Contributing

Contributions are welcome. [AGENTS.md](AGENTS.md) explains how the repository is
laid out, how to build and test the compiler, and the commit conventions;
[docs/RELEASING.md](docs/RELEASING.md) covers pull requests and releases. The
full test suite runs with:

```sh
./brine2 test tests/integration.br
```

## License

brine is licensed under the [Apache License 2.0](LICENSE).
