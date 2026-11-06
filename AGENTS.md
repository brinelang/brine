# AGENTS.md — working on brine itself

brine is a verified, natively parallel systems language with Python-like syntax.
This repository is the language: the compiler (written in brine), its C runtime,
the docs and the tests. Read `docs/DESIGN.md` for the why and `docs/QUICKREF.md`
(`brine guide`) for the language.

## Layout
- `compiler/` — the compiler and CLI, in brine. `main.br` is the entry point
  (lexer → parser → check → uniform → verify (prover) → lower (to the IR in `ir.br`)
  → `x64.br` / `a64.br` (x86-64 / AArch64 machine code, `obj.br` writes the
  object) or `ir_c.br` (C)
  → link). `driver.br` picks the backend and runs the system toolchain.
- `runtime/` — the C runtime (`brine_rt.h`, `brine_rt.c`), embedded in the compiler.
- `bootstrap/` — the released brine that builds this one (`VERSION`, `SHA256SUMS`).
- `tests/integration.br` — end-to-end tests; `tests/programs/` (programs and their
  expected `.out`), `tests/syntax/` (lexer/parser edge cases with expected output).
- `compiler/dev_solver.br`, `compiler/dev_diag.br`, `compiler/dev_native.br` — unit
  tests for the prover, the diagnostics renderer and the x86-64 backend.

## Build
```sh
# once: downloads the release in bootstrap/VERSION, which builds ./brine
sh scripts/dist.sh bootstrap
# after changing the compiler: brine rebuilds itself in place
./brine build compiler/main.br -o brine
```
If a change breaks the compiler, bootstrap again, or build from the last commit
that worked. (On Windows the replaced one stays as `brine.exe.old`
until the next build, since a running program can't be deleted there.)

A new builtin that the compiler itself uses needs two steps: build the compiler with
the builtin added but not yet used (with the old `brine`), then use it and rebuild
with that new compiler.

## Before every commit
```sh
./brine fmt --check compiler tests/integration.br tests/update_syntax.br
./brine test tests/integration.br        # from the repository root; ~30 s
```
`tests/integration.br` also runs the prover and diagnostics unit tests and checks
that the compiler rebuilds itself to a fixed point. If you changed the lexer or
parser on purpose, regenerate `tests/syntax/` with
`./brine run tests/update_syntax.br -- ./brine` and review the diff.

## Commits: Conventional Commits
`type(scope): summary` in the imperative, lower case, no final period, under ~72
characters. A body explains why, when that isn't obvious.
- types: `feat` (new behavior), `fix` (bug fix), `perf` (faster, same behavior),
  `refactor`, `docs`, `test`, `build` (bootstrap, toolchain, install scripts), `ci`
  (GitHub workflows, `scripts/`), `chore` (releases, tidying).
- scopes (optional): `lexer`, `parser`, `check`, `verify`, `solver`, `ir` (the IR
  and lowering), `native` (x86-64, AArch64 and objects), `c` (the C backend), `driver`,
  `runtime`, `cli`, `fmt`, `tests`, `docs`, `bootstrap`.
- A change that breaks existing programs or command-line use: `feat(check)!: ...`
  plus a `BREAKING CHANGE: ...` paragraph saying what breaks and how to migrate.
- One logical change per commit; the tests pass at every commit.

## Versions: Semantic Versioning
The version is set in one place: `VERSION` in `compiler/driver.br`.
- We are in 0.x: the language is not stable yet. A release with new features or
  breaking changes bumps the minor version (0.1.0 → 0.2.0); a release with only
  fixes bumps the patch version (0.2.0 → 0.2.1).
- Move one step at a time. 1.0.0 means a stable language and ABI; there is a lot to
  do before then (see the roadmap in `docs/DESIGN.md`), so never jump ahead, and
  only the maintainer decides when 1.0 happens.
- Releases are their own commit, `chore(release): X.Y.Z`: bump `VERSION`, add the
  release notes to `CHANGELOG.md` (from the commit messages since the last release),
  Once it is merged, tag the merge commit `vX.Y.Z` and
  push the tag: the release workflow builds, tests and publishes it.
  [docs/RELEASING.md](docs/RELEASING.md) has the steps.

## Pull requests and CI
Changes reach `main` through pull requests, merged with a merge commit once CI
(`.github/workflows/ci.yml`: bootstrap, `fmt --check`, the integration tests and
packaging on Linux, macOS and Windows) is green. See
[docs/RELEASING.md](docs/RELEASING.md).

## Generated files
Don't commit anything that can be regenerated from the sources (build output,
caches, generated C, `target/`).

## Bootstrap
The compiler must build with the release in `bootstrap/VERSION` (stage 0). To use
a language feature that release can't compile in `compiler/`, release first, then
move `bootstrap/VERSION` to the new release and copy its `SHA256SUMS` into
`bootstrap/` (`build` commit), then use the feature.

## Style
- Run `brine fmt`. Comments are short and plain; say why, not what.
- The compiler has no dependencies: on x86-64 and AArch64 it writes and links
  executables itself. A C compiler is only for other platforms, wasm (wasi-sdk) and
  `BRINE_BACKEND=c`. Keep it that way.
- Error messages follow `docs/DESIGN.md` §4: a code, a primary span, why it's wrong
  and how to fix it, and an entry in `compiler/explain.br` for every new code.
