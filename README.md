# brine

[![CI](https://github.com/brinelang/brine/actions/workflows/ci.yml/badge.svg)](https://github.com/brinelang/brine/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/brinelang/brine?include_prereleases&sort=semver)](https://github.com/brinelang/brine/releases)

A compiled systems language with Python-like syntax, compile-time proofs, and
parallelism built into the language.

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

* **Rules can't be broken.** `requires`, `ensures`, `invariant` and `prove` are
  checked at compile time. Overflow, bad indexes and division by zero always stop
  the program with a clear message, unless the compiler proved they can't happen.
  In that case the check is removed.
* **Natively parallel.** `par for`, `par [...]`, `par sum(...)` and `par(a, b)` are
  data-race free by construction, and reductions are deterministic. Big ones run
  on an NVIDIA GPU (CUDA) or an Apple one (Metal) when that is faster, with the
  same results.
* **Multi-process built in.** The `world` module gives every program MPI-style
  ranks, messages and collectives. `brine run --ranks 4`.
* **Fast.** Compiles to native code through C. There is no garbage collector, and
  values are copy-on-write.
* **Easy for people and agents.** One CLI and readable errors. `--json` gives
  machine-readable output, `brine explain` explains every error code, and
  `brine guide` prints the whole language on one page.

## Install

Linux and macOS:

```sh
curl -fsSL https://raw.githubusercontent.com/brinelang/brine/main/install.sh | sh
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/brinelang/brine/main/install.ps1 | iex
```

The installers put `brine` in `~/.brine/bin` and add it to your `PATH`; run them
again to upgrade. Set `BRINE_VERSION=0.4.0` first to install a given version. There
are builds for Linux (x64, ARM64), macOS (Apple silicon, Intel) and Windows (x64),
also on the [releases page](https://github.com/brinelang/brine/releases).

brine needs a C compiler (`gcc` or `clang`, or MinGW on Windows): it builds the
runtime, links, and compiles `--release` builds. `brine doctor` checks it.

```sh
brine new hello && cd hello
brine run
```

### From source

The compiler is written in brine; build it once from the bootstrap seed (see
[bootstrap/README.md](bootstrap/README.md)):

```sh
# from the repository root (Windows: add .exe and use -lws2_32 -lshell32)
cc -std=c11 -O2 -fno-strict-aliasing -w -Iruntime bootstrap/brinec.c runtime/brine_rt.c -o brine -lm -lpthread -ldl
```

| Command | What it does |
|---|---|
| `brine new <name>` / `brine init` | create a project |
| `brine run [file] [-- args]` | build and run (`--release`, `--threads N`, `--ranks N`) |
| `brine build` | build a binary (`--release`, `--debug`, `-o`, `--emit-c`) |
| `brine check` | type-check and prove, without building (`--report` lists remaining runtime checks) |
| `brine test [filter]` | run `test "...":` blocks in parallel |
| `brine fmt` | format sources (`--check`) |
| `brine debug` | debug build and launch gdb/lldb |
| `brine explain E0601` | explain an error code |
| `brine guide` | the language quick reference |
| `brine doctor` | diagnose the toolchain |

## Learn more

* [docs/QUICKREF.md](docs/QUICKREF.md): the whole language on one page (`brine guide`)
* [docs/DESIGN.md](docs/DESIGN.md): philosophy, architecture, roadmap, open decisions
* [examples/](examples/): hello, contracts, parallel, world (multi-process), shapes (structs, enums, modules),
  find_signal (proofs and the GPU together)

## Status

brine is young (v0.x): the language and tools still change. The compiler is
written in brine (`compiler/`) and builds itself; `bootstrap/brinec.c` is its own
generated C, used to build it from scratch. Dev builds on x86-64 Windows and Linux and on ARM64 Linux
go straight to machine code (the whole compiler builds in under a second);
`--release` builds and other platforms go through C. The runtime is written in C
(`runtime/`). It runs on Windows, Linux and macOS.

`brine test tests/integration.br` (from the repository root) runs the end-to-end
suite: the examples and `tests/programs/`, expected errors, the formatter, the
lexer and parser on edge cases, and the compiler rebuilding itself to a fixed point.
See [AGENTS.md](AGENTS.md) for how to work on brine itself.
