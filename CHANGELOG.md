# Changelog

All notable changes to brine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html) (see `AGENTS.md`).

## [Unreleased]

## [0.7.0] - 2026-10-29
### Added
- `brine spec-diff [--base <rev>]` compares every `requires`, `ensures` and
  struct `invariant` with a git revision (default `HEAD`) and says which ones
  got weaker or stronger. The prover decides this, not a text diff, so the same
  spec written another way is not a change. It exits 1 when a function promises
  less or asks more of its callers, and counts the runtime checks left before
  and after. Run it in CI against the base branch to catch a change that makes
  the proofs pass by proving less.
- Struct invariants: `invariant` lines in a struct state facts about `self` that
  every value of the type keeps. They are proven where a value is built and
  where a `ref self` method returns, and known everywhere else, so functions
  that take the type need no `requires` for them. Only the struct's own methods
  may change its fields (E0406). E0609 reports an invariant that can't be
  proven. Generic structs can't have invariants yet (E0112).
- Failed proofs show values the prover can't rule out (`len(xs) = 0`), so you
  can tell a bug in the code from a fact the prover is missing.
- `proven = true` under `[check]` in brine.toml (or `--proven`) makes every
  runtime check the prover can't remove an error (E0607), so the program can't
  stop on a failed bounds, overflow, division, conversion or map-key check, or
  on an `assert`. `test` blocks are exempt.

### Changed
- A function whose `requires` can never all hold is an error (E0608). Before,
  every claim in its body was proven without meaning anything. Fix the
  `requires` that is wrong.
- `brine check --report` also lists the `assert`s outside `test` blocks, since
  they are runtime checks too.
- Building the same program to a second output reuses the objects already
  compiled for the first, instead of compiling them again.

### Fixed
- A failed proof in generic code is reported once, not once for each instance.

## [0.6.0] - 2026-10-28
### Added
- Reading input: `input()` / `input(prompt)` returns the next line of stdin as an
  `Option[str]` (`None` at its end) and `read_stdin()` all of it. Binary files:
  `os.read_bytes(path)` and `os.append_bytes`, and `str.from_utf8(bytes)` checks
  and converts. `append_file(path, text)` appends without rewriting the file.
- Parsing that doesn't panic: `s.to_int()` and `s.to_float()` return an `Option`
  (`s.to_int[u8]()` for other widths). `int()` and `float()` accept `_` between
  digits, like literals.
- Math: `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `exp2`, `log2`,
  `log10`, `cbrt` and `hypot`, on `float` and `f32`; `pow` on `f32`. Float bits:
  `x.to_bits()`, `float.from_bits(u)`, `f32.from_bits(u)`.
- Integer methods: `wrapping_add`, `wrapping_sub`, `wrapping_mul`, `wrapping_neg`,
  `rotate_left`, `rotate_right`, `count_ones`, `leading_zeros` and
  `trailing_zeros`. Compound assignments `&=`, `|=`, `^=`, `<<=` and `>>=`. A
  literal takes the type it's converted to (`u64(0xcbf29ce484222325)`).
- `s.find(sub, start)` and `xs.index(x, start)`; `xs + ys` and `xs += ys` join
  arrays; `s * n` repeats a string.
- Format specs `{x:g}` and thousands separators `{n:,}` (also `{x:>12,.2f}`).
- `flush()`, and `os.thread_count()` (the threads `par` uses).
- Ordering: `<`, `<=`, `>`, `>=` compare tuples and structs part by part and enums
  without data in declaration order, so `sort()`, `min`/`max` and
  `min(... for ...)` work on them.
- Constants: array constants (`const TABLE = [1, 2, 3]`), `case NAME:` matches a
  constant's value, and enums without data convert to and from `int`
  (`int(color)`, `Color.from_int(i)` returns an `Option`).
- Syntax: multi-line strings (`"""..."""`, also f-strings), string literals inside
  f-string fields (`f"{d["k"]}"`), names bound in `|` patterns
  (`case A(x) | B(x):`), tuple assignment (`a, b = b, a % b`), `T(x)` in generic
  code, and `test` and `case` as ordinary names outside their statements.
- `import ..lib` imports from the parent directory (one `.` per level), and
  `from b import Thing` takes a name `b` imported itself.
- `let t = ref self.field` names a field; different fields of one value can be
  passed as separate `ref` arguments, and nested calls like `t.f(t.g(x))` are
  allowed when their order is fixed by the data.
- `main` may return `Result[int, E]`: `Ok(n)` exits with `n`.
- `first(...)` and `par first(...)`: the lowest-index `Some` of a generator of
  options, the same at every thread count; `par first` stops the work after a hit.
- `par for` may write `out[k * i + c]` (constants `k >= 1` and `c`, all writes of
  `out` with the same `k`), so red-black and strided sweeps run in place.
- GPU: 128-bit integer code runs in CUDA kernels.
- `extern` functions can take `str` arguments (C gets a NUL-terminated copy).
- `world`: `world.any`/`world.all`, array `sum`/`min`/`max`, edge shifts and
  narrow numeric messages.
- `brine test` stops a test that runs longer than `--timeout` (300 s by default);
  `# fmt: off` / `# fmt: on` keep lines as written; `--help`/`-h` on every command.

### Changed
- **Breaking:** `par for x in xs: out[x] = ...` is rejected: the elements of `xs`
  may repeat, so two iterations could write the same element at the same time.
  Loop over a range instead (`par for i in 0..len(xs)`).
- **Breaking:** a `par` body can't call a function that writes files, runs
  processes or reads stdin, however deep the call (these were only caught when
  written in the body itself). Do the effect before or after the `par`.
- **Breaking:** the `world` deadlock checker is now sound: it rejects programs it
  used to accept that could hang, such as a collective in an `assert` message, an
  `exit()` under a rank-dependent condition, or a builtin result used as if it
  were the same on every rank. It also accepts more correct programs: it follows
  values through assignments and tracks tuple elements, struct fields and array
  lengths separately.
- **Breaking:** the prover no longer accepts several false claims: what a
  `while` condition or a mutating array method changes, effects of branches,
  comprehension bodies and loop invariants are now modeled. Fix the `prove`,
  `requires`/`ensures` or invariant it reports, or add a runtime `assert`.
- A rank removes `BRINE_RANK`, `BRINE_WORLD_SIZE` and `BRINE_PEERS` from its
  environment after reading them, so programs it starts don't join its world.
  Use `world.rank()` instead of reading the variable.
- `brine fmt` breaks long lines at operators and inside nested brackets, so
  `fmt --check` may ask to reformat files formatted by 0.5.
- stdout is written line by line to terminals and pipes (still buffered to
  files), so line protocols over pipes work.
- `sin`, `exp`, `pow` and the other math functions give the same bits on every
  target (musl's code is built in), and generated C is compiled with
  `-ffp-contract=off`, so float results no longer depend on the C compiler.
- The prover knows more: bounds of products, remainders, bit operations, shifts
  and bit counts, lengths of `xs + ys` and `s * n`, ranges of literal arrays, and
  loops and calls forget only the fields they change. It is much faster on
  branchy loops (1.45 s to 0.01 s on one test), says when it gave up instead of
  "not proven", and its notes show only the parts of a claim it couldn't prove.
- Faster code:
  - Small enums that can't contain themselves (`Option[int]`, `Result`, small
    payloads) are stored in place instead of in heap boxes.
  - `for x in xs`, returning part of a parameter, `let`/`var` copies of fields,
    `x = f(x)`, `out[i] = f(out[i])` in a `par for`, and the take idiom borrow or
    move instead of copying.
  - Release builds check `a[j + x]` once before a loop that calls no
    functions instead of every iteration, so C compilers vectorize it (ikj
    matmul: 31 to 74 GFLOP/s).
  - Reference counts use plain instructions while no other thread runs.
  - Small local `[x] * n` arrays live in the stack frame.
  - x86-64 dev builds share registers, use jump tables for `match` and inline
    small functions (a ray tracer: 113 s to 45 s).
  - `par sum` over a few expensive elements uses all the threads; `par any` and
    `par all` stop starting work once the answer is known.
  - A GPU `par for` no longer uploads arrays it only writes; with `--ranks`, the
    k-th rank on a machine uses its k-th GPU.
  - `world` collectives take log2(ranks) rounds (sum at 8 ranks: 133 to 51 us),
    and each rank gets its share of the cores instead of all of them. A float
    `world.sum` adds in a tree: still the same on every run, but its last digits
    can differ from 0.5's rank-by-rank sum.
  - WebAssembly builds use SIMD.

### Fixed
- Crashes in `brine run` and `brine test` are reported with the reason instead of
  exit code 0; a stack overflow is a panic, `main` has a 64 MB stack, and freeing
  a deep structure no longer overflows the stack.
- `world` messages carry their call, so mismatched collectives panic with both
  call sites; a failing rank is named, and only the first panic is shown.
- GPU kernels were rejected as "using a global" when they used stack slots; the
  GPU log now gives the exact reason a construct stays on the CPU.
- A string slice that cuts a character in two panics; `unwrap()` on an `Err`
  shows the error; `nan` and `inf` print the same everywhere.
- By-value arguments that the same call changes through a `ref` are copied first.
- The smallest `i128` literal is accepted.
- A call's other arguments can't change what one of its `ref` arguments refers
  to, even inside a comprehension (E0405): `f(ref xs[0], sum(g(ref xs) for ...))`
  passed checks and broke `f`'s precondition at run time.
- WebAssembly programs run on a worker thread with a 256 MB stack, so deep
  recursion works as natively, and `--target web` programs run in a Web Worker.
- Many error messages: operators not defined for a type, unclosed strings (one
  error instead of a cascade), imports (where they looked), and help that
  suggested code which would have changed the program.

## [0.5.1] - 2026-10-28
### Fixed
- WebAssembly modules no longer carry the debug info of wasi-sdk's C library, which
  was most of their size: a release hello world goes from 398 KB to 36 KB, so pages
  that load brine programs download about a tenth as much. `--debug` builds keep it.

## [0.5.0] - 2026-10-28
### Added
- WebAssembly: `brine build --target wasm` makes a WASI module (wasm32-wasip1) and
  `brine build --target web` makes a folder for the browser (the module, a small
  loader `brine.js` and a starter page that rebuilds keep). `brine run` and
  `brine test` run WASI modules with Node.js, or with `$BRINE_WASM_RUNNER`. It needs
  wasi-sdk (`WASI_SDK_PATH`). On WebAssembly programs run on one thread and one
  `world` rank (`par` runs in order with the same results); processes, sockets and
  the GPU fail with a clear message, and the browser has no files.

### Changed
- Much faster release builds: on the benchmarks, `--release` now takes 0.28x to
  0.67x C's time (`gcc -O2`) on seven of eight programs, and ties C on wordfreq.
  Output and panics are unchanged; every fast path checks its conditions at run
  time or falls back to the code as written.
  - x86-64 release programs carry plain, AVX2 (x86-64-v3) and AVX-512 (x86-64-v4)
    copies and pick the newest one the CPU supports, so executables still run on
    any x86-64 machine. `BRINE_CPU=base` or `v3` caps the level.
  - The prover removes more overflow checks: compound assignments (`i += 1`) and
    loop counters.
  - Loops whose inputs are small run a copy without overflow checks, so C
    compilers vectorize them.
  - Independent loop iterations run four at a time (mandelbrot: 0.59 s to 0.43 s).
  - Local `[bool]` arrays are kept as bits and small integer arrays as bytes;
    reverse, rotate and copy loops run as one operation (fannkuch: 1.6 s to
    0.96 s).
  - Small fixed-size float array loops run as vector kernels with bit-identical
    results (nbody: 0.71 s to 0.49 s).
  - Faster strings and maps: inlined map lookups, one lookup for
    `if k in m: m[k] += v`, in-place byte appends for `s += t[i]`, and string
    buffers reused across loop iterations (wordfreq: 77.6 ms to 17.6 ms).
- Programs start 2 ms faster on Windows (3.0 ms for an empty program, from 5.0 ms).
- `BRINE_LEAKCHECK` is the only thing that counts live objects now, so allocation
  no longer pays for a locked counter.

### Fixed
- `brine test` reported zero tests and success when listing the tests failed.

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

[Unreleased]: https://github.com/brinelang/brine/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/brinelang/brine/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/brinelang/brine/compare/v0.5.1...v0.6.0
[0.5.1]: https://github.com/brinelang/brine/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/brinelang/brine/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/brinelang/brine/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/brinelang/brine/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/brinelang/brine/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/brinelang/brine/releases/tag/v0.1.0
