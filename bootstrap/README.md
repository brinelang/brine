# Bootstrap

The compiler is written in brine, so building it takes a brine. `VERSION`
names the release that builds this one (stage 0), and `SHA256SUMS` is that
release's checksum list, so a download that doesn't match is refused.

```sh
sh scripts/dist.sh bootstrap
```

downloads stage 0 for this machine into `target/boot/`, builds the compiler
with it (`target/boot/brine1`), and has that one rebuild itself as `./brine`,
with this checkout's runtime and code generation. Set `BRINE0` to a brine
executable to use it as stage 0 instead of downloading one.

Then, after changing the compiler:

```sh
./brine build compiler/main.br -o brine
```

A correct compiler reaches a fixed point: rebuilding `brine` with itself again
gives the same compiler (`tests/integration.br` checks this).

Stage 0 is 0.9.0, which needs no C compiler on x86-64 and ARM64.
