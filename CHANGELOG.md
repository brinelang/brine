# Changelog

All notable changes to brine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html) (see `AGENTS.md`).

## [Unreleased]

## [0.2.0]
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

## [0.1.0]
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
