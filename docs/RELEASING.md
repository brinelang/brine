# Changes and releases

How a change gets into `main`, and how `main` becomes a release on GitHub.
The rules for commits and versions themselves are in [AGENTS.md](../AGENTS.md).

## Changes: pull requests into `main`

`main` always builds and passes the tests on every platform we ship.

1. Work on a branch (`feat/...`, `fix/...`, `ci/...`), in Conventional Commits,
   one logical change per commit, each one passing the tests.
2. Open a pull request. CI (`.github/workflows/ci.yml`) builds brine from the
   bootstrap seed, checks formatting, runs `tests/integration.br` and packages a
   release archive on each platform (linux-x64, linux-arm64, macos-arm64, macos-x64,
   windows-x64). The archives are attached to the run as artifacts.
3. Merge when CI is green and review is done, with a **merge commit**. The
   commits keep their messages, which become the changelog, and each one passes
   the tests, so `git bisect` works. Rebase the branch onto `main` first if it is
   behind.

## Releases

A release is a version tag on `main`. Pushing the tag publishes it; nothing is
built or uploaded by hand.

1. **Pick the version** (AGENTS.md, "Versions"): in 0.x, any `feat` or breaking
   change since the last tag means the next minor version (0.3.0 → 0.4.0); only
   fixes mean the next patch version (0.4.0 → 0.4.1).
2. **Prepare the release on a branch** `release/X.Y.Z`, in one commit,
   `chore(release): X.Y.Z`:
   - set `VERSION` in `compiler/driver.br`;
   - in `CHANGELOG.md`, turn `## [Unreleased]` into `## [X.Y.Z] - YYYY-MM-DD` and
     write its notes from `git log vPREV..main --no-merges --format='%s%n%b'`:
     user-visible changes only, grouped as Added, Changed, Deprecated, Removed,
     Fixed, Security, in plain sentences; breaking changes say how to migrate.
     Leave an empty `## [Unreleased]` above it and add the compare link at the
     bottom;
   - rebuild the compiler with itself and refresh the seed:
     ```sh
     ./brine build compiler/main.br -o brine2
     ./brine2 emit-c --release compiler/main.br -o bootstrap/brinec.c
     ```
   - check that `sh scripts/release-notes.sh X.Y.Z` prints the notes.
3. **Open a pull request** for it and merge it (merge commit) when CI is green.
   CI builds from the new seed, so this also proves the seed works.
4. **Tag the merge commit** on `main` and push the tag:
   ```sh
   git switch main && git pull --ff-only
   git tag -a vX.Y.Z -m "brine X.Y.Z"
   git push origin vX.Y.Z
   ```
5. **The release workflow** (`.github/workflows/release.yml`) then:
   - checks that the tag matches `VERSION` and has a `CHANGELOG.md` section, and
     writes the notes from it (`scripts/release-notes.sh`);
   - builds and tests every platform through `ci.yml`;
   - publishes the GitHub release `brine X.Y.Z` with the archives, `SHA256SUMS` and
     build provenance attestations (tags with a `-`, like `v0.5.0-rc.1`, are marked
     as pre-releases);
   - installs it on every platform with the one-line installers and checks
     `brine --version`.

   If a step fails before publishing, fix it on `main` with a normal pull request,
   then move the tag: `git tag -d vX.Y.Z && git push origin :refs/tags/vX.Y.Z`,
   tag the new commit and push it again. Never move a tag after its release is
   published; make a patch release instead.

## Installing

The installers download the archive for the machine from the latest release
(or `BRINE_VERSION`), check it against `SHA256SUMS`, put the binary in
`~/.brine/bin` and add that to `PATH`:

```sh
curl -fsSL https://raw.githubusercontent.com/brinelang/brine/main/install.sh | sh
```

```powershell
irm https://raw.githubusercontent.com/brinelang/brine/main/install.ps1 | iex
```

`scripts/dist.sh` builds the same archives locally: `sh scripts/dist.sh bootstrap`
then `sh scripts/dist.sh package`.
