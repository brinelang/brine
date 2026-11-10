<div align="center">

<img alt="brine" src="docs/assets/banner.png" width="100%">

**A fast, parallel systems language that reads like Python and proves your code
correct before it runs.**

[![CI](https://github.com/brinelang/brine/actions/workflows/ci.yml/badge.svg)](https://github.com/brinelang/brine/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/brinelang/brine?include_prereleases&sort=semver)](https://github.com/brinelang/brine/releases)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)

[Website](https://brinelang.org) · [Docs](https://docs.brinelang.org) · [Install](#install) · [Get started](#get-started) · [Language reference](docs/QUICKREF.md) · [Examples](examples/)

</div>

---

You decide what your program must do. You write it down as **rules** in
`RULES.br`. Every build **proves** every rule, or nothing is built. That holds
whoever writes the code: you, a teammate or an AI agent.

On top of that, brine:

- **Reads like Python.** Static types, inferred locally. Structs, enums,
  `match`, generics, `Option` and `Result`.
- **Is parallel by default.** `par for`, `par [...]` and `par sum(...)` use
  every core. Data races are compile errors, and results are the same bits on
  any machine.
- **Is fast.** Native machine code, no garbage collector, no VM. It doesn't
  need a C compiler on x86-64 or ARM64.
- **Proves the details too.** The compiler proves bounds, overflow and your
  `requires`/`ensures` contracts, and removes the checks it proved. Anything it
  can't prove stops the program with a clear message. It never fails silently.

```python
fn main():
    let squares = par [i * i for i in 0..1_000_000]  # runs on all cores
    let total = par sum(float(x) for x in squares)  # same answer on any machine
    print(total)
```

## Install

**Linux and macOS**

```sh
curl -fsSL https://raw.githubusercontent.com/brinelang/brine/main/install.sh | sh
```

**Windows** (PowerShell)

```powershell
irm https://raw.githubusercontent.com/brinelang/brine/main/install.ps1 | iex
```

That's it: brine lands in `~/.brine/bin`, on your `PATH`. Run the same command to
upgrade. `brine doctor` checks your setup.

<details>
<summary>Installer options, other downloads, uninstalling</summary>

| Variable | Effect |
|---|---|
| `BRINE_VERSION=0.9.2` | install a given version (default: the latest) |
| `BRINE_INSTALL=~/.brine` | where to install (the binary goes in `$BRINE_INSTALL/bin`) |
| `BRINE_NO_MODIFY_PATH=1` | don't add brine to your `PATH` |

Prebuilt archives are on the [releases page](https://github.com/brinelang/brine/releases).
To uninstall, delete `~/.brine` and the `PATH` line the installer added.

</details>

## Get started

### 1. Make a project

```sh
brine new bank
cd bank
brine run
```

```text
     Built bank v0.1.0 [dev] in 0.27s
   Running target/dev/bank
Hello, brine!
[0, 1, 4, 9, 16, 25, 36, 49, 64, 81]
```

`brine new` creates `src/main.br`, a `RULES.br` with no rules yet (its comments
show how they look), and an `AGENTS.md` that tells coding agents how to work in
the project.

### 2. Write a rule

A rule says what must always be true. Add this one to `RULES.br`:

```python
from bank import withdraw

rule "a withdrawal never overdraws" for balance: int, amount: int:
    requires balance >= 0
    let after = withdraw(balance, amount)
    prove 0 <= after <= balance
```

It covers **every** `balance` and `amount`, not a few test cases. Approve it,
which records that you agree with it in `RULES.lock`:

```sh
brine rules approve
```

### 3. Write the code

Add `src/bank.br`, a first try that ignores the edge cases:

```python
fn withdraw(balance: int, amount: int) -> int:
    return balance - amount
```

Then make `src/main.br` use it:

```python
from bank import withdraw

fn main():
    print(withdraw(100, 30))
```

`brine run` refuses to build it, and says why:

```text
error[E0605]: cannot prove this claim
 --> RULES.br:6:11
  |
6 |     prove 0 <= after <= balance
  |           ^^^^^^^^^^^^^^^^^^^^^ the prover could not show this is always true
  |
  = note: rule "a withdrawal never overdraws" isn't proven, so nothing is built
  = note: values the prover can't rule out: balance = 0, after = -1

error[E0607]: a runtime check is left in code that must be proven
 --> src/bank.br:2:12
  |
2 |     return balance - amount
  |            ^^^^^^^^^^^^^^^^ the prover could not remove this overflow check
  |
  = note: rule "a withdrawal never overdraws" covers this function, so nothing it does may stop the program
```

Code that a rule covers can't crash either, so the overflow counts too.

### 4. Fix it

```python
fn withdraw(balance: int, amount: int) -> int:
    requires balance >= 0
    ensures 0 <= result <= balance
    if amount <= 0 or amount > balance:
        return balance  # refuse: leave the balance as it was
    return balance - amount
```

```text
$ brine run
    Proven 1 rule from RULES.br; the function they cover can't stop the program
     Built bank v0.1.0 [dev] in 0.27s
   Running target/dev/bank
70
```

### 5. Nobody gets around a rule

Say someone, or an agent, weakens the rule to make the code pass. CI and
release builds stop until the owner approves the change (a dev build only
warns, so the owner can draft rules):

```text
error[E0613]: rule "a withdrawal never overdraws" changed since it was approved
 --> RULES.br:3:1
  |
3 | rule "a withdrawal never overdraws" for balance: int, amount: int:
  | ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ not the approved version
  |
  = help: if the rules' owner agrees with the change, they run `brine rules approve` and commit RULES.lock; otherwise undo it
```

Put `RULES.br`, `rules/` and `RULES.lock` under CODEOWNERS, so only the owner
can approve. Three commands help the owner review:

| Command | What it does |
|---|---|
| `brine rules` | which rules are proven and approved |
| `brine rules --explain` | the rules in plain English, so you can check they say what you meant |
| `brine rules --receipt` | a summary for a pull request |

Rules can also limit what code may do, or forbid it from crashing:

```python
rule "pricing never touches files, the network, processes or the clock":
    effects pricing: none

rule "a bad request can never crash the server":
    proven server.handle
```

## A short tour

### Contracts

Functions state what they need (`requires`) and what they promise (`ensures`).
The compiler proves both, and removes the bounds checks it proved:

```python
fn last(xs: [int]) -> int:
    requires len(xs) >= 1  # every caller must prove this
    return xs[len(xs) - 1]  # in bounds: no runtime check
```

A caller that might pass an empty list doesn't compile. The error shows the
values that break it (`len(xs) = 0`). `brine check --report` lists the runtime
checks the prover couldn't remove.

### Parallel, without data races

| Construct | Meaning |
|---|---|
| `par for i in 0..n:` | parallel loop |
| `par [f(i) for i in 0..n]` | parallel map |
| `par sum(e for ...)` | parallel reduction (also `product`, `min`, `max`, `any`, `all`) |
| `par(f(a), g(b))` | run both at once, get a tuple |

Writing shared state from a `par` body is a compile error:

```text
error[E0501]: cannot assign to `total` inside a `par` body
 --> race.br:5:9
  |
5 |         total += xs[i]
  |         ^^^^^ would be a data race
  |
  = help: a `par for` may only write `total[i]` (its own element); to combine values use `par sum(...)`, `par [...]`, or a sequential loop
```

Reductions combine values in a fixed order, so a float `par sum` gives the same
bits on 1 thread, 32 threads or a GPU.

### GPUs, with no code changes

`par` constructs move to an NVIDIA (CUDA) or Apple (Metal) GPU on their own,
when the runtime measures that it's faster. Results don't change. There's
nothing to annotate or install beyond the driver. `BRINE_GPU=off` turns it off,
and `BRINE_GPU_LOG=1` shows where each `par` ran. Numbers are in
[bench/gpu](bench/gpu/README.md).

### Many processes, no deadlocks

```python
fn main():
    let total = world.sum(world.rank())  # every rank gets the total
    if world.rank() == 0:
        print(f"{world.size()} ranks, total = {total}")
```

```sh
brine run world.br --ranks 4
```

The compiler checks that every rank makes the same `world` calls in the same
order, so a program can't hang waiting for a message that never comes.

### WebAssembly and the browser

```sh
brine run --target wasm examples/parallel.br           # run with Node.js
brine build --target web examples/parallel.br -o demo  # a folder for any static host
```

WebAssembly builds need [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases)
(set `WASI_SDK_PATH`). They run on one thread, with the same results.

## The `brine` command

| Command | What it does |
|---|---|
| `brine new <name>` | create a project (`brine init` in an existing folder) |
| `brine run` | build and run (`--release`, `--threads N`, `--ranks N`, `--target wasm`) |
| `brine build` | build a binary (`--release`, `--debug`, `-o`, `--target wasm\|web`) |
| `brine check` | type-check and prove without building (`--report`, `--proven`) |
| `brine test` | run the `test "...":` blocks |
| `brine rules` | rule status (`approve`, `--explain`, `--receipt`) |
| `brine spec-diff` | which contracts got weaker since a git revision (`--base main`) |
| `brine fmt` | format code (`--check`) |
| `brine explain E0601` | explain an error code |
| `brine guide` | the whole language on one page |

Every error has a code, a precise location, the reason and a fix. Add `--json`
for output tools can read.

## Platforms

| Platform | Native builds | GPU |
|---|:---:|---|
| Linux x86-64, ARM64 | ✅ | NVIDIA (CUDA) |
| Windows x86-64 | ✅ | NVIDIA (CUDA) |
| macOS Apple silicon | ✅ | Apple GPU (Metal) |
| macOS Intel | ✅ | – |

On these platforms brine writes and links executables itself: no C compiler
needed. Anywhere else, it builds through C.

## Learn more

- [docs/QUICKREF.md](docs/QUICKREF.md): the whole language on one page (also `brine guide`)
- [examples/](examples/): [parallel.br](examples/parallel.br),
  [contracts.br](examples/contracts.br), [world.br](examples/world.br),
  [shapes/](examples/shapes/), [find_signal.br](examples/find_signal.br)
  (proofs and the GPU together)
- [docs/DESIGN.md](docs/DESIGN.md): philosophy, architecture and the
  [roadmap](docs/DESIGN.md#5-roadmap)
- [CHANGELOG.md](CHANGELOG.md): what changed in each release

brine is young (0.x): the language can still change between minor releases.

## Contributing

The compiler is written in brine and builds itself. To build it from source:

```sh
git clone https://github.com/brinelang/brine && cd brine
sh scripts/dist.sh bootstrap               # downloads the release brine starts from
./brine build compiler/main.br -o brine    # after a change: brine rebuilds itself
./brine test tests/integration.br          # the full test suite
```

[AGENTS.md](AGENTS.md) covers the layout, the tests and the commit conventions;
[docs/RELEASING.md](docs/RELEASING.md) covers pull requests and releases.

## License

[Apache License 2.0](LICENSE).
