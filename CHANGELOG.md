# Changelog

All notable changes to brine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html) (see `AGENTS.md`).

## [Unreleased]

## [0.4.0] - 2026-10-27
### Added
- `par for`, `par [...]` maps and `par` reductions run on the GPU when that's
  faster: NVIDIA GPUs through CUDA (Windows and Linux) and Apple GPUs (M1 and later)
  through Metal. Bodies that only compute qualify (arithmetic, comparisons,
  branches, loops, reads of plain data and arrays, calls to such functions), and
  nothing changes in the source. Results are bit-identical to the CPU's; a check
  that fails on the device reruns the construct on the CPU, which panics as usual.
  The GPU driver is loaded when the program runs, so there's nothing to install or
  link. `BRINE_GPU=off|on|auto` picks the policy and `BRINE_GPU_LOG=1` prints where
  each construct ran and why.
- A native AArch64 backend for Linux: dev and debug builds on 64-bit ARM Linux go
  straight to machine code, as on x86-64.
- Native debug builds: `--debug` on Windows and Linux carries DWARF line and
  variable info, so gdb and lldb break at `file.br:line` or a brine function, step
  through brine lines and show locals by their brine names and types.
- `brine debug` shows values the way brine prints them (`"hello"`, `[1, 2, 3]`,
  `Some(5)`) when gdb has Python, and steps over the runtime.
- Release builds on GitHub for Linux (x64, arm64), macOS (arm64, x64) and Windows
  (x64), with checksums and build provenance, and one-line installers:
  `curl -fsSL https://raw.githubusercontent.com/brinelang/brine/main/install.sh | sh`
  and `irm https://raw.githubusercontent.com/brinelang/brine/main/install.ps1 | iex`.
- `examples/find_signal.br`: a GPS signal search whose indexing is proved safe and
  whose search runs on the GPU.

### Changed
- **Breaking:** `par` reductions combine their elements in a fixed order: leaves of
  16 consecutive elements, each folded left to right, then a balanced tree. The
  result is the same for any thread count and on the GPU. `par sum` and
  `par product` of floats can differ from 0.3.0 in the last bits (usually closer to
  the exact result), and whether an integer `par sum` or `par product` overflows is
  decided by the new order. Programs that need the old bits must sum sequentially.
- Faster native code: local variables and floats live in registers (dev builds of
  nbody go from 7.96x C's time to 3.45x, spectral norm from 6.54x to 2.05x).
- Loops that index a local array load it once and make it unique before the loop,
  so C compilers vectorize them (matrix multiply: 8.7x C's time to 1.0x).
- Faster strings: `s += t` grows a string in place and `s[i]` is inlined.
- Faster allocation: small blocks come from brine's own allocator, and a value with
  one reference is freed without an atomic instruction (binary-trees on Windows:
  1.67x C's time to 0.67x).
- Parallel regions start and end without a lock, so short regions no longer slow
  down as threads are added.
- Division and remainder by a constant power of two compile to a shift and a mask.
- `match` on a `let` variable or a by-value parameter no longer retains and
  releases each value it visits.

### Fixed
- Programs failed to link on AArch64 Linux when clang was the C compiler (missing
  `__muloti4`).
- Float `min` and `max` of `-0.0` and `0.0` gave different results on different
  platforms; `min` now treats `-0.0` as the smaller.
- A panic's message could interleave with what other threads printed.
- The build cache could reuse an object built against an older runtime header,
  which then failed to link.
- A `BRINE_BACKEND` build could replace a default build's executable while it ran.
- `brine run --ranks N` listened on every network interface, so Windows asked to
  let each new build through the firewall; with every rank on this machine it
  listens on loopback.

## [0.3.0] - 2026-10-27
### Added
- `print` takes `sep=` and `end=`: `print(a, b, sep=", ", end="")`.
- Nested and parenthesized tuple unpacking in `for`, `let` and comprehensions:
  `for i, (k, v) in enumerate(pairs):`, `let (a, (b, c)) = t`.
- `s.contains(x)` on strings and `xs.contains(x)` on arrays (the same as `x in s`).
- Integer literals up to the largest `u128` (2^128 - 1); above `i128`'s maximum
  only a `u128` holds them.
- Native code has unwind tables (`.pdata`/`.xdata` on Windows, `.eh_frame` on
  Linux), so debuggers and profilers can walk through brine frames.

### Changed
- `upper()`, `lower()`, `strip()`, `split()` and `is_alpha`/`is_alnum`/`is_space`
  follow Unicode, as in Python (`"straße".upper()` is `"STRASSE"`). `is_digit` stays
  `0`-`9`, the digits `int()` reads.
- The compiler parses imported modules and proves functions in parallel (building
  the compiler itself: 0.85 s to 0.61 s).
- Faster native code: reference counting is inlined and values just stored aren't
  read back from memory.
- A build whose backend `BRINE_BACKEND` overrides goes to its own folder (`dev-c`),
  so it can't replace a default build's executable while that one runs.

### Fixed
- A function returning a call that returns nothing (`return f()` in a `-> ()`
  function, as in the prelude's `Result[(), E].unwrap()`) was rejected with E0301.
- Native calls whose 128-bit result is unused, and stack slots aligned to more than
  16 bytes.

## [0.2.0] - 2026-10-27
### Added
- A native x86-64 backend: dev builds on Windows and Linux go straight from the
  brine IR to machine code and a COFF or ELF object, then link with the runtime.
  The compiler builds itself in under a second (it took about 20 s through C).
  `BRINE_BACKEND=c` or `=native` picks a backend.
- The brine IR (`compiler/ir.br`), between the verifier and code generation. Both
  backends read it: x86-64, and C (for `--release`, `--debug` and other platforms).
- `os.write_bytes(path, bytes)` writes a `[u8]` file.
- `BRINE_TIMES=2` names the functions the prover is slow on.

### Changed
- C builds split the program into one translation unit per core, compiled in
  parallel and cached one by one, so an edit recompiles only what it touches
  (`--release` stays a single unit).
- Debug builds name C variables after the brine ones (`x_3`), so gdb and lldb
  show them.
- Faster checking: the deadlock pass skips programs that never communicate, and
  the prover bounds the facts it keeps where branches merge, with the same proofs
  (checking the whole compiler takes about half a second).

### Removed
- The AST-to-C generator (`codegen.br`); C comes from the IR.

## [0.1.0] - 2026-10-27
The first version.
- The language: structs and methods, enums and `match`, generics, `Option`/`Result`
  and `?`, tuples, arrays, maps, strings, f-strings, modules, `par` constructs,
  multi-process `world` programs, `test` blocks, file and process I/O (`os`).
- Contracts (`requires`, `ensures`, `invariant`, `prove`) checked at compile time by
  a built-in prover, which also removes runtime checks it can prove unnecessary.
- The `brine` CLI: `new`, `init`, `run`, `build`, `check`, `test`, `fmt`, `debug`,
  `explain`, `guide`, `doctor`, with `--json` output.
- The compiler is written in brine and builds itself; `bootstrap/brinec.c` builds it
  from scratch with only a C compiler.

[Unreleased]: https://github.com/brinelang/brine/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/brinelang/brine/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/brinelang/brine/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/brinelang/brine/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/brinelang/brine/releases/tag/v0.1.0
