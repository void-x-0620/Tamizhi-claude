# Tamizhi

A compiler for a Tanglish language (Tamil keywords in English transliteration), written in C.
It translates Tamizhi source to C99 and hands the result to the host C compiler, so the output
is a native binary with no interpretation overhead.

## Build and run

```sh
make                      # build ./tamizhi
make test                 # run the full suite
make asan                 # build ./tamizhi-asan (AddressSanitizer + UBSan)
./tamizhi prog.tz -o prog && ./prog
```

Options: `--no-warn`, `-Werror`, `--color=WHEN`, `-o <file>`, `-S` (emit C to stdout or `-o`), `-c` (object file), `-O<0-3>`,
`-v` (per-stage timings), `--dump-tokens`, `--cc=<cmd>`, `--version`.

## The language (version 2.3)

The full grammar, every keyword and the operator table are in **[SYNTAX.md](SYNTAX.md)**.
A quick tour:

```
peyar Byte = u8;                                   # typedef
pattiyal Color { Red, Green = 5, Blue }            # enum
kattamaipu Point { x: int; y: int; }               # struct   (inaippu = union)
nilai edu LIMIT = 10;                              # const    (thangu = static, maaridum = volatile)

vagupu Animal {                                    # class
    pothu name: string;                            # pothu / paathukaappu / maraivu
    uruvaakku(n: string) { ithu.name = n; }        # constructor
    seyal speak(): string { kudu "..."; }          # virtual method
}
vagupu Dog : Animal {                              # inheritance
    uruvaakku(n: string) { thandhai(n); }          # parent constructor
    seyal speak(): string { kudu "Woof"; }
}

seyal main(): int {
    edu a: Animal = puthu Dog("Rex");              # upcast, heap object
    pathi("%s says %s\n", a.name, a.speak());      # Rex says Woof
    ovvoru (edu i = 0; i < 3; i++) { pathi("%d ", i); }
    thervu (Color.Blue) { vazhakku Color.Blue: { pathi("blue\n"); } iyalbu: { } }
    kudu 0;
}
```

Types: `int float bool string char`, the exact C family (`i8…i64 u8…u64 short ushort uint long
ulong usize isize f32 f64 ldouble uchar`), pointers `T*`, arrays `T[N]`, function pointers
`seyal(int): int`, structs, unions, enums, classes and `peyar` aliases. Comments start with `#`.

## Vim support

`vim/` has filetype detection (`*.tz` and `*.tzb`), syntax highlighting, C-style indentation, comment settings
and a dark colour scheme.

```sh
./vim/install.sh                         # installs into ~/.vim
VIMDIR=~/.config/nvim ./vim/install.sh   # Neovim
```

Then add `colorscheme tamizhi` to your vimrc. Syntax highlighting works with any colour scheme;
the bundled one is optional.

## Calling C (bindings)

Declare the C functions you want in a separate **binding file** (by convention `*.tzb`) and
include it from your program with `serku`:

```
# mymath.tzb
veli "math.h";                                  # #include this header (put it first)
veli seyal root  = sqrt(x : float) : float;     # Tamizhi name = C name(...)
veli seyal power = pow(x : float, y : float) : float;
veli seyal floor(x : float) : float;            # no "=": same name on both sides
```

```
# main.tz
serku "mymath.tzb";
seyal main() : int {
    pathi("%.1f\n", root(144.0));
    kudu 0;
}
```

- **Renaming:** in `veli seyal A = b(...)`, `A` is the name you use in Tamizhi and `b` is the
  C function. Bind the same C function under several names if you like.
- **Binding files** may contain only `veli` and `serku` lines. Paths are relative to the
  including file (then the current directory). Each file is included once, so circular
  includes are harmless.
- **Headers:** `veli "x.h";` uses `x.h` from next to the binding file if it exists, otherwise
  the normal include path (add directories with `-I`). Calls then use the header's prototypes.
  Without a header line, Tamizhi writes the prototype itself, so the declared types must
  match the C signature exactly (`int` is a 64-bit `long long`, `float` is `double`).
- **Types** across the boundary: `int`, `float`, `string` (`const char *`), `bool` and no
  return value. Arrays and structs are not supported.
- **Linking:** `-l<lib>` and `-L<dir>` are passed to the C compiler, and extra `.c`, `.o`, `.a`
  files on the command line are compiled and linked in:

```sh
./tamizhi main.tz mylib.c -o app          # your own C code
./tamizhi main.tz -lsqlite3 -o app        # a system library
```

`tests/ext/` is a complete example (a C file, its header and two binding files).

## Strings and garbage collection

Strings can be joined with `+`, which allocates on the heap. Heap memory is managed by the
**Boehm garbage collector** by default, and you can switch it off:

| Flag | Behaviour |
|---|---|
| `--gc` (default) | Heap data is collected. Links `-lgc` only if the program allocates. |
| `--no-gc` | Plain `malloc`, never freed; memory returns to the OS at exit. No libgc needed. |

A program that never allocates needs no collector and no extra library either way. If you
use GC without libgc installed you get a link error plus a hint; install `libgc-dev`
(Debian/Ubuntu), point `-L` at it, or use `--no-gc`. On a loop that builds 5 million
short strings, peak memory was 12.7 MB with `--gc` and 535.7 MB with `--no-gc`.

## Architecture

Single pass per stage, all stages share one arena, so the whole AST is freed in one call.

| File | Stage |
|---|---|
| `util.c`, `tamizhi.h` | Arena allocator, string buffer, diagnostics (`file:line:col: error:`) |
| `lexer.c` | Hand-written scanner, one-token lookahead, keyword table |
| `parser.c`, `ast.c` | Recursive descent with precedence climbing; panic-mode recovery so several errors are reported per run |
| `sema.c` | Scoped symbol table, type checking, call arity and type checks |
| `codegen.c` | AST to C99, plus the optimisations below |
| `main.c` | Driver: runs the stages, then invokes the host C compiler |

Optimisations done in `codegen.c`: constant folding (int and float), peephole simplification
(`x+0`, `x*1`, `x*0`), and dead-code elimination for constant-false `if` branches and `while(false)`.
The host compiler then applies `-O2` by default.

## Tests

`make test` runs the sample programs in `tests/` (output compared with `.expected`), the
compile-error cases in `tests/errors/` (diagnostics compared with `.expected`), the C-binding
and heap cases (`tests/ext/`, `tests/heap/`), classes and runtime checks (`tests/oop/`, `tests/rt/`), and optimiser checks that inspect the generated C.
The `--gc` test is skipped if libgc cannot be linked; pass `GCFLAGS=-L<dir>` to point at it.
`make test BIN=./tamizhi-asan` (after `make asan`) runs everything under AddressSanitizer.

## Your own header files

`serku "shapes.tzh";` pulls in another Tamizhi file once: functions, structs, enums, classes and
globals, anything except `main`. Files ending in `.tzb` are C binding files instead (only `veli` /
`serku`). Errors inside a header are reported with the header's file name and line.

## Runtime checks

Null object/pointer use, fixed-array indexing and integer division by zero are checked at run
time and stop the program with `file:line: runtime error: ...`. `--no-checks` removes them.

## Known limits

- Not supported: `goto`, bit-fields, the comma operator, inline asm, abstract classes /
  interfaces, overloading, multiple inheritance, generics, pointer-to-const (`nilai` applies to
  the variable itself), `nilai` fields. See SYNTAX.md section 8.
- A subclass must declare a constructor when its parent's constructor takes arguments
  (constructors are not inherited). Downcasts with `maatru` are unchecked.
- A program that declares a class always uses the heap, so it needs libgc unless built with
  `--no-gc` (`--no-gc` never frees objects).
- Output goes through the system `cc`, so a C compiler must be installed.
- Argument evaluation order is C's, i.e. unspecified: `f(tick(), tick())` may run the calls in
  either order. Put calls with side effects in separate statements.
- Global initialisers that are not constants (calls, other variables, string `+`) run before
  `main`, in declaration order.
