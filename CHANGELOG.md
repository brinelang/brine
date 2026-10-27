# Changelog

All notable changes to brine. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html) (see `AGENTS.md`).

## [Unreleased]

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
