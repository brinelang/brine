# Bootstrap seed

`brinec.c` is the C that the brine compiler (`compiler/`, written in brine)
generates for itself. It lets you build the compiler with only a C11 compiler:

```sh
# Linux / macOS
cc -std=c11 -O2 -fno-strict-aliasing -w -Iruntime bootstrap/brinec.c runtime/brine_rt.c -o brine -lm -lpthread

# Windows (MinGW gcc or clang)
gcc -std=c11 -O2 -fno-strict-aliasing -w -Iruntime bootstrap/brinec.c runtime/brine_rt.c -o brine.exe -lws2_32 -lshell32
```

Run these from the repository root. The result is the full `brine` tool
(`brine run`, `build`, `check`, `test`, `fmt`, ...).

After changing the compiler, rebuild it with itself and refresh the seed:

```sh
./brine build --release compiler/main.br -o brine2
./brine2 emit-c --release compiler/main.br -o bootstrap/brinec.c
```

A correct compiler reaches a fixed point: building again with `brine2`
gives the same `brinec.c`.
