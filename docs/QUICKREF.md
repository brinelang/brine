# brine quick reference

brine is a compiled, verified, natively parallel language with Python-like syntax.
Blocks use `:` plus 4-space indentation. Comments start with `#`.

## Programs and commands

```
brine new hello && cd hello    # create a project (brine.toml, src/main.br, AGENTS.md)
brine run                      # build + run          brine run file.br -- args
brine check                    # type-check + prove   brine check --report
brine check --typed            # also require a written type on every variable
brine test                     # run test blocks      brine test "name filter"
brine fmt                      # format               brine fmt --check
brine run --ranks 4            # 4 cooperating processes (world module)
brine debug file.br            # debug build in gdb/lldb: break file.br:12, step, print x
brine explain E0601            # explain any error code
brine run --target wasm        # WebAssembly, run with Node.js (needs wasi-sdk, $WASI_SDK_PATH)
brine build --target web -o d  # d/: index.html + brine.js + program.wasm, for a browser
```
Add `--json` to any command for one JSON object per line. `brine fmt` leaves
lines between `# fmt: off` and `# fmt: on` comments (and number tables written
in rows) as they are.
WebAssembly runs on one thread (`par` runs in order, same results) and as one
`world` rank; in a browser there are no files or processes.

## Values and types

| Type | Examples |
|---|---|
| `int` (= `i64`), `i8 i16 i32 i128 u8 u16 u32 u64 u128` | `42`, `1_000_000`, `0xff`, `0b1010` |
| `float` (= `f64`), `f32` | `3.14`, `1e-9` |
| `bool` | `true`, `false` |
| `str` | `"hi\n"`, `f"x = {x}, pi = {pi:.3f}"` |
| arrays `[T]` | `[1, 2, 3]`, `[0.0] * n`, `[i * i for i in 0..n if i % 2 == 0]` |
| maps `{K: V}` | `{"ann": 31, "bob": 27}`, `var m: {str: int} = {}` |
| tuples `(A, B)` | `(1, "one")`, `pair.0` |
| structs | `Point(x=1, y=2)` or `Point(1, 2)` |

* No implicit conversions. Convert explicitly: `float(i)`, `int(x)` (truncates),
  `i32(n)` (checked), `str(v)`, `int("42")` (parses), `float("2.5")`.
* Literals adapt to context: `let b: u8 = 200`, `let f: f32 = 1`,
  `let big: i128 = 10_000_000_000_000_000_000_000`. Integer literals go up to
  `u128`'s maximum (2^128 - 1); above `i128`'s maximum only a `u128` holds them.
* `i128` and `u128` need a 64-bit target (they're a GCC/Clang feature). The prover
  can't remove their overflow checks; those always run.
* Integer overflow, out-of-range conversions, division by zero and
  out-of-bounds indexing stop the program with a clear message (a *panic*),
  unless the compiler proved they can't happen, in which case the check is removed.
  Only the first panic is reported, even when many `par` threads fail at once.
* Every thread, `main` included, has a 64 MB stack; recursion deeper than that
  panics with "stack overflow (recursion too deep)". WebAssembly engines allow
  less (about 10,000 to 100,000 calls, depending on the frames).
* Integer `/` rounds down like Python's `//` (`-7 / 2 == -4`), and `%` takes the sign of
  the right side (`-7 % 2 == 1`). Use `float(a) / float(b)` for a fractional result.
* Bit operators on integers: `&` (and), `|` (or), `^` (xor), `~` (not), `<<`, `>>`.
  Unlike `+` and `*`, `<<` doesn't check for overflow: bits shifted past the type's
  width are dropped (`u64(1) << 63` is 2^63, `3 << 63` is `-2^63`). `>>` keeps the
  sign of signed types (`-8 >> 1 == -4`) and fills unsigned ones with zeros. A shift
  by a negative amount or by the type's width or more panics.
* Values are **values**: `let b = a` copies (cheaply, copy-on-write). Changing `b`
  never changes `a`. To share a variable on purpose, use `ref` (see Functions).

## Bindings

```python
let x = 10              # immutable
var total = 0           # mutable
total += x
let (q, r) = (17 / 5, 17 % 5)   # tuple unpacking
let (a, (b, c)) = (1, (2, 3))   # nests
u, v = v, u % v         # assign several `var` places at once; parentheses optional
xs[i], xs[j] = xs[j], xs[i]     # every value is computed first, then stored
lo, _ = bounds          # `_` drops a value
var names: [str] = []   # annotate when the type can't be inferred
let n: i32 = 7          # ...or whenever you like
for i: int in 0..3:     # loop variables too: `for k: str, v: int in m.items():`
    total += i
const LIMIT = 100       # compile-time constant (top level)
```
In `x, y = ...` the places can't overlap, and an index can't read a variable
the same line assigns (`i, xs[i] = ...` is error E0405): the result would
depend on the order of the stores.

Types are optional, Python style. To require them, put `typed = true` under
`[check]` in brine.toml (or pass `--typed`): then every `let`, `var` and `for`
variable needs a written type (error E0337 tells you which one to add).
No shadowing: a name can be declared only once per function. `_` ignores a
value (`let (q, _) = ...`, `for _ in 0..n:`); it may repeat and can't be read.

## Control flow

```python
if x < 0:
    print("negative")
elif x == 0:
    print("zero")
else:
    print("positive")

let sign = "neg" if x < 0 else "non-neg"   # conditional expression

for i in 0..n:          # 0, 1, ..., n-1        (also: 0..=n, range(a, b, step))
    pass
for x in xs:            # iterates over a snapshot of xs
    pass
for i, x in enumerate(xs):  # unpack tuples: (0, xs[0]), (1, xs[1]), ...
    pass
for i, (k, v) in enumerate(pairs):  # nested tuples too (also in comprehensions)
    pass
for c in "héllo":       # each UTF-8 character, as a one-character str
    pass
while k < n:
    k += 1              # break / continue work as usual
```
Comparisons chain: `0 <= i < n`. Logic is `and`, `or`, `not`.

## Functions

```python
fn area(w: float, h: float) -> float:
    return w * h

fn fill(ref xs: [int], v: int):      # `ref` param: changes the caller's variable
    for i in 0..len(xs):
        xs[i] = v

fn countdown(mut n: int):            # `mut` param: a local copy the function may change
    while n > 0:
        n -= 1

fn main():
    var data = [0] * 10
    fill(ref data, 7)                # the caller says `ref` too
    let d = ref data                 # `d` is another name for `data`
    d[0] = 1                         # changes `data`
```
Arguments are passed by value (cheaply: arrays and strings are copy-on-write).
References can't be stored in structs or arrays or returned, so they never dangle.
`main` may return `int` (the exit code) or a `Result` (see below). Parameter types are required;
local types are inferred.

## Structs and methods

```python
struct Particle:
    pos: float
    vel: float

    fn step(ref self, dt: float):    # method that modifies its receiver
        self.pos += self.vel * dt

    fn energy(self) -> float:
        return 0.5 * self.vel * self.vel

    fn at_rest(pos: float) -> Particle:   # no `self`: call as Particle.at_rest(1.0)
        return Particle(pos=pos, vel=0.0)

var p = Particle(pos=0.0, vel=2.0)
p.step(0.1)
print(p, p.energy())                 # Particle(pos=0.2, vel=2.0) 2.0
```

## Enums and `match`

```python
enum Color:            # no data: a plain tag
    Red
    Green

enum Shape:
    Circle(radius: float)
    Rect(w: float, h: float)
    Empty

    fn area(self) -> float:          # enums can have methods
        match self:
            case Shape.Circle(r):
                return 3.14 * r * r
            case Shape.Rect(w, h):
                return w * h
            case Shape.Empty:
                return 0.0

enum Expr:                           # recursive enums are fine
    Num(value: int)
    Add(left: Expr, right: Expr)

let s = Shape.Rect(w=2.0, h=3.0)     # positional or keyword fields
print(s, s.area(), s == Shape.Empty) # Shape.Rect(w=2.0, h=3.0) 6.0 false
```

Patterns: `_`, a name (captures), int/str/bool literals, tuples `(a, 0)`,
alternatives `1 | 2 | 3`, and variants `Shape.Rect(w, _)` / `Expr.Add(left=l)`
(nested to any depth). A guard adds a condition: `case x if x < 0:`.
Alternatives may bind names when each binds the same ones with the same types:
`case Shape.Circle(n) | Shape.Square(n):` (E0321 otherwise). With a guard,
each alternative is tried in turn: `case (x, _) | (_, x) if x > 5:` matches `(1, 9)`.
Every `match` must handle every value (error E0320 shows one it misses);
end with `case _:` when listing values isn't possible. Variant data is read with
`match`, never with `.field`.

## Generics, `Option` and `Result`

```python
struct Pair[A, B]:
    first: A
    second: B

fn first[T](xs: [T]) -> Option[T]:     # type parameters in brackets
    if len(xs) == 0:
        return None
    return Some(xs[0])

fn parse_bit(s: str) -> Result[int, str]:
    if s == "0":
        return Ok(0)
    if s == "1":
        return Ok(1)
    return Err(f"not a bit: {s}")

let p = Pair(first=1, second="one")     # type arguments are inferred...
let q = Pair[int, str](first=2, second="two")   # ...or written out
let none: Option[int] = None           # `None` alone needs a known type
match first([3, 4]):
    case Some(x):
        print(x)
    case None:
        print("empty")
print(parse_bit("1").unwrap(), parse_bit("x").unwrap_or(-1))   # 1 -1
```

`x?` unwraps an `Option` or `Result`: it gives the value inside `Some`/`Ok`, or
returns the `None`/`Err` from the current function at once (the function must
return `Option[...]`, or `Result[..., E]` with the same `E`):

```python
fn load(path: str) -> Result[int, str]:
    let text = read_file(path)?          # Err(...) is returned to the caller
    return parse_bit(text.strip())

fn main() -> Result[(), str]:           # an Err prints "error: ..." and exits with 1
    print(load("bit.txt")?)
    return Ok(())
```
`main` may also return `Result[int, E]`: `Ok(n)` exits with code `n`.

`Option[T]` (`Some(value)`, `None`) and `Result[T, E]` (`Ok(value)`, `Err(error)`)
are always available. Methods: `is_some()`, `is_none()`, `is_ok()`, `is_err()`,
`unwrap()` (panics on `None`, and on `Err` with the error), `unwrap_or(default)`, `ok()` (Result to Option).
Each use of a generic function or type with new type arguments makes a
specialized copy, so generic code is as fast as hand-written code. A generic
function's body is checked for each set of type arguments it's used with:
`largest[T]` that compares with `>` works for `int` and `str`, and is an error
for a struct. In the same way, `T(x)` converts to what `T` stands for, like
`int(x)` or `f32(x)`: `fn mean[E](xs: [E]) -> E` can return `total / E(len(xs))`.

Methods can have type parameters of their own, next to their type's:

```python
struct Box[T]:
    v: T

    fn pair[U](self, u: U) -> (T, U):
        return (self.v, u)

let b = Box(v=1)
print(b.pair("x"), b.pair[float](2.5))   # (1, "x") (1, 2.5)
```

## Arrays

```python
var xs = [3, 1, 2]
xs.push(5)              # append
let last = xs.pop()     # remove and return the last element (xs.pop(i): at i)
xs.insert(0, 9)         # put 9 at index 0, shifting the rest right
xs.extend([7, 8])       # append every element
xs.sort()               # numbers and strings; stable
xs.reverse()
xs.clear()
print(len(xs), xs == [3, 1, 2], 3 in xs, xs.index(3))  # index: Option[int]
print(xs.contains(3))   # the same as `3 in xs`
print(xs[1:3], xs[:2], xs[2:])   # slices are copies; out of range panics
```
There are no negative indexes: write `xs[len(xs) - 1]`. A statement can't both
change a variable and read it elsewhere (`print(xs.pop(), xs)` is an error), so
evaluation order never matters.

## Strings

Strings are immutable UTF-8. `len(s)`, `s[i]` and slices count **bytes**;
`s[i]` is the byte at `i` as a one-byte string: a character for ASCII text
(the common case), a piece of one otherwise, so scan text byte by byte and
compare with ASCII (`s[i] == " "`). A slice must not cut a character in two:
`"aé"[0:2]` panics. `for c in s` / `s.chars()` walk real characters.

```python
let s = "hello, brine"
print(s[0], s[7:], s + "!", "ri" in s, s.contains("ri"), f"{s} has {len(s)} bytes")
print(s.find("brine"))                    # Some(7); None if absent
print(s.split(", "), "a b  c".split())    # ["hello", "brine"], ["a", "b", "c"]
print("-".join(["a", "b"]), "ab".repeat(3), s.replace("l", "L"))
print(s.starts_with("he"), s.ends_with("!"), s.count("l"))
print("  x ".strip(), "  x".lstrip(), "x  ".rstrip(), s.upper(), s.lower())
print("a\nb".lines(), s.chars(), s.bytes())  # [str], [str], [u8]
print("42".is_digit(), "ab".is_alpha(), " ".is_space(), "a1".is_alnum())
print(ord("a"), chr(97))                   # 97 "a"
```
Text over several lines goes between `"""` quotes (also `f"""..."""`). A line
break right after the opening quotes is dropped, and so is the indentation all
the lines share (the closing quotes' line counts, blank lines don't); `\` at the
end of a line joins it with the next:

```python
fn usage(name: str) -> str:
    return f"""
        usage: {name} [--verbose] FILE
          --verbose   say more
        """
# usage("app") == "usage: app [--verbose] FILE\n  --verbose   say more\n"
```
An f-string field holds any expression, strings included: `f"{d["key"]}"`,
`f"{", ".join(names)}"`.

Case conversion and `is_alpha`/`is_space` follow Unicode, as in Python
(`"straße".upper()` is `"STRASSE"`); `is_digit` is `0`-`9`, the digits `int()`
reads. `str(x)` converts any
printable value; `int("42")` and `float("2.5")` parse (and panic on bad input, so
check with `is_digit` first when the text comes from outside).

## Maps

```python
var ages = {"ann": 31, "bob": 27}   # {str: int}; remembers insertion order
ages["cy"] = 40                     # insert or replace
ages["ann"] += 1                    # missing keys panic: check with `in` first
if "bob" in ages:
    print(ages["bob"])
print(ages.get("zed"))              # Option[int]: None
print(ages.get("zed", 0))           # 0 (default)
ages.remove("bob")                  # true if it was there
for name in ages:                   # keys, in insertion order (a snapshot:
    print(name, ages[name])         # the loop body may change the map)
print(ages.keys(), ages.values(), ages.items(), len(ages))
```

Keys can be integers, `bool`, `str`, enums, or arrays/tuples/structs of those
(not floats). Maps are values like everything else: `let b = a` copies (lazily).

## Built-ins

`print(a, b, ...)` (`sep=" "` goes between the values and `end="\n"` after
them: `print(x, end="")`), `len`, `abs`, `min(a, b)`, `max(a, b)`, `sqrt`, `sin`, `cos`,
`tan`, `exp`, `log`, `pow(x, y)`, `floor`, `ceil`, `round`, `clock()` (seconds),
`args()` (the command-line arguments as `[str]`, without the program's name:
`brine run app.br -- a b` gives `["a", "b"]`), `exit(code)`, `panic(msg)`,
`eprint(...)` (print to stderr), `ord(c)`, `chr(n)`, `enumerate(xs)`, `flush()`.
On floats, `min` and `max` ignore a NaN (the other argument wins; both NaN gives
NaN) and order `-0.0` below `0.0`, the same on every target.

The math functions give the same bits on every platform, natively and in
WebAssembly: brine has its own math library (from musl) rather than the C
library's, whose last bits differ between systems.

`print` output to a terminal or a pipe is written at the end of each line; to a
file it is buffered (and written at exit, or when `eprint`, `run` or `flush()`
needs it out). `flush()` writes out what is buffered, such as a prompt printed
with `end=""`.

Files and processes (all failures are values, never crashes):

```python
match read_file("in.txt"):              # Result[str, str]; text must be UTF-8
    case Ok(text):
        print(len(text.lines()))
    case Err(e):
        eprint(e)                       # "cannot read in.txt: No such file or directory"
write_file("out.txt", "hi\n")          # Result[(), str]
file_exists("out.txt")                  # bool
getenv("HOME")                          # Option[str]
run(["cc", "-O2", "-o", "app", "app.c"])  # Result[int, str]: Ok(exit code), Err if it can't start
```
`write_file` and `run` can't be used inside `par` (their effects would happen in
an unpredictable order).

The `os` module (built in, no import; errors are `Err` values with the OS's
message, e.g. `"The system cannot find the file specified. (os error 2)"`):

```python
os.list_dir(path)       # Result[[str], str]: entry names (not paths), sorted, no "." / ".."
os.is_dir(path), os.is_file(path)            # bool
os.make_dir(path)       # Result[(), str]: creates missing parents; Ok if it exists
os.remove_file(path), os.rename(src, dst)    # Result[(), str]; rename replaces dst
os.cwd(), os.temp_dir() # str (temp_dir has no trailing separator)
os.set_cwd(path)        # Result[(), str]: change the working directory
os.write_bytes(path, b) # Result[(), str]: write a [u8] as a binary file
os.exe_path()           # str: the absolute path of the running program
os.abs_path(path)       # Result[str, str]: canonical; on Windows `\\?\C:\...`
os.name(), os.arch()    # "windows"/"macos"/"linux"..., "x86_64"/"aarch64"...
os.pid(), os.cpu_count()                     # int (cpu_count >= 1)
os.thread_count()       # int: threads `par` uses (BRINE_THREADS / --threads; 1 on wasm)
os.sleep(0.5)           # seconds
os.is_terminal(2)       # is fd 1 (stdout) / 2 (stderr) / 0 a terminal?
os.enable_ansi()        # Windows: let the console show color codes
os.free_ports(n)        # Result[[int], str]: n distinct free localhost TCP ports
let p = os.spawn(["cc", "x.c"], {"LANG": "C"}, true, false)?  # Result[int, str]: a handle
#   env: added to this process's environment; capture: collect stdout/stderr;
#   stdin: false gives the child an empty stdin. cmd[0] is searched on PATH.
os.wait(p)              # int: exit code (101 if killed by a signal)
os.poll(p)              # Option[int]: exit code if it has exited
os.kill(p)              # stop it
let (out, err) = os.output(p)   # captured text once it has exited ("" if not captured)
```
`os.spawn`, `wait`, `poll`, `kill`, `make_dir`, `remove_file`, `rename`, `sleep`
and `enable_ansi` can't be used inside `par`. Results of `os` calls (except
`os.name()`/`os.arch()`) count as rank-dependent for `world` deadlock checks.

`embed("help.txt")` is a `str` with the file's contents, read when the program is
compiled (path: a string literal, relative to the file containing the call;
`\r\n` becomes `\n`). It works in `const`s too: `const HELP = embed("help.txt")`.
Reductions over generators: `sum(x * x for x in xs)`, `product`, `min`, `max`,
`any(x < 0 for x in xs)`, `all(...)`. `min`/`max` of an empty sequence panic.

## Parallelism (data-race free by construction)

```python
let ys = par [f(x) for x in xs]              # parallel map
let total = par sum(x * x for x in xs)        # parallel reduction
var out = [0.0] * n
par for i in 0..n:                            # parallel loop
    out[i] = xs[i] * 2.0                      # only out[i] of outer arrays may be written
let (a, b) = par(work(1), work(2))            # fork-join
```
Rules: inside a `par` body, outer variables are read-only, except that
`par for i` may write `arr[i]` (exactly the loop variable) of an outer array.
Reductions are deterministic: same result bits on any machine and thread count.
They fold leaves of 16 consecutive elements left to right, then combine the leaves
in a balanced pairwise tree (so `par sum` of floats can differ in the last bits from
a sequential `sum`, usually by being more accurate). Integer overflow of any partial
result in that order traps.
`BRINE_THREADS=n` or `brine run --threads n` sets the thread count.

**GPUs.** `par for`, `par [...]` and `par` reductions also run on a GPU (NVIDIA
through CUDA, Apple silicon through Metal) when the runtime expects that to be
faster (big constructs with much work per byte copied); the results are the same
bits, and a failing check panics as on the CPU. On Apple GPUs, f64 code stays on
the CPU (they have no f64).
Nothing to write or install beyond the driver: the compiler makes a kernel of every
body that only uses int/float arithmetic, comparisons, branches, loops, structs,
tuples, reads of outer arrays and scalars, `out[i]` writes, `sqrt`, `abs`, `floor`,
`ceil`, `round`, `min`, `max`, and calls to functions that do the same. Strings,
maps, new arrays, printing, `sin`/`exp`/`log`/`pow` and the like keep a body on the
CPU. `BRINE_GPU=off|on|auto` (default `auto`; `on` uses the GPU whenever it can),
`BRINE_GPU_LOG=1` prints where each construct ran and why.

## Multiple processes and machines: `world`

```python
fn main():
    let rank = world.rank()           # 0 .. world.size()-1
    let local = sum(i for i in 0..1000 if i % world.size() == rank)
    let total = world.sum(local)      # every rank gets the total
    if rank == 0:
        print(total)
    let left = world.shift(rank, 1)       # send to rank + 1, receive from rank - 1
    let up = world.shift(rank, 1, -1)     # no wrapping around: rank 0 gets -1
    let inbox = world.exchange([rank] * world.size())   # xs[j] goes to rank j
    let sums = world.sum([1.0, float(rank)])            # element by element
    let done = world.all(rank >= 0)                     # also world.any
```
API: `rank()`, `size()`, `barrier()`, `broadcast(root, v)`, `sum(v)`, `min(v)`,
`max(v)` (on `int`, `float`, `[int]`, `[float]`), `any(b)`, `all(b)`,
`gather_all(v) -> [T]`, `shift(v, by)`, `shift(v, by, edge)`, `exchange(xs) -> [T]`.
Message types: integers and floats of any width (`[u8]` travels as bytes), `bool`,
`str`, and arrays of those (`[[float]]`, ...).
Run with `brine run --ranks N` (each rank gets cores / N `par` threads unless
`--threads` says otherwise). Without it, `world.size() == 1`. If a rank fails,
`brine run` stops the others and exits with that rank's exit code.

**No deadlocks, by construction.** Every rank takes part in every `world` call,
and the compiler checks that all ranks make the same calls in the same order:
a call under a condition that depends on `world.rank()` (or received values,
`clock()`, ...), or after a rank-dependent `return`, `break` or `exit`, is an
error (E0510). Results of `sum`/`min`/`max`/`any`/`all`/`broadcast`/
`gather_all` are the same on every rank, so loops like
`while world.max(err) > tol:` are fine. At run time, each message also carries
its call site, so a mismatch is a clear panic naming both calls.

## Proofs: rules that can't be broken

```python
fn clamp(x: int, lo: int, hi: int) -> int:
    requires lo <= hi                 # every caller must prove this
    ensures lo <= result <= hi        # proven for every return
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x

fn index_of(xs: [int], target: int) -> int:
    ensures -1 <= result < len(xs)
    var lo = 0
    var hi = len(xs)
    while lo < hi:
        invariant 0 <= lo <= hi <= len(xs)     # holds before every iteration
        let mid = lo + (hi - lo) / 2
        if xs[mid] == target:                  # bounds check proven: removed
            return mid
        if xs[mid] < target:
            lo = mid + 1
        else:
            hi = mid
    return -1
```
* `requires`, `ensures`, `invariant` and `prove e` **must** be proven at compile
  time; if the prover can't, it's an error listing the facts it knew.
* `assert e` checks at runtime (always, in every build) and is a known fact
  afterwards. Use it to hand the prover a fact it can't derive itself.
* Specs may use arithmetic, comparisons, `and`/`or`/`not`, `len`, `abs`, `min`,
  `max`, fields, indexing, constants and `result`.
* In `for i in a..b` loops, invariants may mention `i`: they must hold at the start
  of each iteration (and with `i = b` after the loop).
* The prover handles linear integer arithmetic (`+ - ` and multiplication or
  division by constants). It never accepts a false claim.
* `brine check --report` lists the runtime checks that remain.

## Modules

```python
import geometry            # loads geometry.br next to the main file
import util.strings as s   # loads util/strings.br

let a = geometry.area(2.0, 3.0)
let p = geometry.Point(x=1, y=2)

from geometry import Point, area   # use these names directly
let q = Point(x=3, y=4)
```

`from m import x` only imports items defined in `m` itself. A name can't be both
imported and defined in the same module.

## Tests

```python
test "clamp keeps values in range":
    assert clamp(15, 0, 10) == 10
    assert clamp(-5, 0, 10) == 0
```
`test` is a keyword only at the start of a top-level line (and `case` only at
the start of a line in a `match`), so both are fine as names elsewhere.
`brine test` runs each test in its own process, in parallel. `$BRINE` is set to the
`brine` running the tests, so a test can run the compiler. A test that runs longer
than 300 seconds is stopped and fails (`brine test --timeout <seconds>`; 0: no limit).

## C interop

```python
extern fn rand() -> i32     # declare a C standard library function (ints, floats, bools only)
```
