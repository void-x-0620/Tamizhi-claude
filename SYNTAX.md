# Tamizhi 2.2 — complete syntax reference

Tamizhi is compiled to C99/GNU C and then to a native binary. Keywords are
Tanglish (Tamil written in English letters). Every example below compiles.

## 1. Lexical rules

| Item | Syntax |
|------|--------|
| Comment | `# to end of line` |
| Identifier | `[A-Za-z_][A-Za-z0-9_]*` (user names get a `tz_` prefix in C, so they never clash with C keywords) |
| Integer | `42`, `0xFF`, `0b1010`, `0o17`; values above 2^63-1 are `u64` |
| Float | `3.14`, `1e9`, `2.5e-3` |
| Char | `'a'`, `'\n'`, `'\t'`, `'\r'`, `'\0'`, `'\\'`, `'\''`, `'\x41'` (type `char`) |
| String | `"text\n"` escapes `\n \t \r \\ \"`; UTF-8 (Tamil) passes through |
| Bool | `true`, `false` |
| Null | `illai` |

## 2. Keywords

| Keyword | Meaning | Keyword | Meaning |
|---------|---------|---------|---------|
| `seyal` | function / method / function type | `edu` | variable |
| `kudu` | return | `pathi` | printf-style print |
| `@( c ) aga irundha {}` | if | `^( c ) aga irundha {}` | else-if |
| `illana {}` | else | `sutri` | while (and the tail of do-while) |
| `ovvoru` | for | `seithu {} sutri (c);` | do-while |
| `thervu` | switch | `vazhakku` | case |
| `iyalbu` | default | `udai` | break |
| `thodar` | continue | `kattamaipu` | struct |
| `inaippu` | union | `pattiyal` | enum |
| `peyar` | typedef (type alias) | `vagupu` | class |
| `uruvaakku` | constructor | `puthu` | create an object |
| `ithu` | this | `thandhai` | super / parent |
| `pothu` | public | `paathukaappu` | protected |
| `maraivu` | private | `nilai` | const |
| `thangu` | static | `maaridum` | volatile |
| `maatru` | cast | `alavu` | sizeof |
| `illai` | null | `serku` | include a header (`.tzh`) or binding (`.tzb`) file |
| `veli` | external C header / function | `muyarchi` | try |
| `pidi` | catch | `erivi` | throw |
| `ulle` | `in` (for-each) | `moolam` | abstract class / method |

## 3. Types

```
type ::= primitive | Name | 'seyal' '(' [type {',' type}] ')' [':' type]
         followed by any number of   '*'   |   '[' [INT] ']'
```

| Tamizhi | C | Notes |
|---------|---|-------|
| `int` | `long long` | default integer, 64-bit |
| `float` | `double` | default real |
| `bool` | `int` | `true`/`false`; any number converts to bool |
| `string` | `const char *` | NUL-terminated, read-only |
| `void` | `void` | return type only (and `void*`) |
| `char` `uchar` | `char` `unsigned char` | 8-bit |
| `short` `ushort` | `short` `unsigned short` | 16-bit |
| `uint` | `unsigned int` | 32-bit |
| `long` `ulong` | `long` `unsigned long` | 64-bit |
| `i8 i16 i32 i64` | `int8_t … int64_t` | exact width |
| `u8 u16 u32 u64` | `uint8_t … uint64_t` | exact width |
| `usize` `isize` | `size_t` `ptrdiff_t` | |
| `f32` `f64` `ldouble` | `float` `double` `long double` | |
| `T*` | `T *` | pointer; `void*` is the generic pointer |
| `T[N]` | `T a[N]` | fixed array; `int[2][3]` is 2 rows of 3 (like C) |
| `seyal(A, B): R` | `R (*)(A, B)` | function pointer |
| `List<T>` | growable list (heap) | section 8 |
| struct / union / enum / class / alias names | | declared in section 4 |

Conversions: widening is implicit (`i16 → i32`, `int → float`, `f32 → float`).
Anything that may lose information needs `maatru(value, Type)`; a constant that
fits the target is accepted without a cast (`edu b: u8 = 200;`).
Arithmetic uses C's usual conversions (`i8 + u8` is `i32`, `f32 * int` is `f32`).

## 4. Declarations

```
program   ::= decl*
decl      ::= func | struct | union | enum | typedef | class | global | import | extern
```

### Variables and qualifiers
```
[nilai] [thangu] [maaridum] edu name [: type] [= expr] ;
edu a = 5;              # type inferred: int
edu b: u8 = 200;        # explicit type
edu t: int[4] = [1,2,3,4];
edu z: int[8];          # zero-initialised
nilai edu LIMIT = 10;   # const: assignment is a compile error, needs an initialiser
thangu edu calls = 0;   # static: global = private to the file; local = keeps its value
maaridum edu flag = 1;  # volatile
```
Every variable is zero-initialised. A `thangu` local needs a constant initialiser.
Globals may use any expression (non-constant ones run at program start-up).

### Functions
```
[thangu] seyal name ( p [: type] {, p [: type]} ) [: type] { ... }
seyal main(): int { kudu 0; }          # entry point, returns int
```
A `thangu` function is file-private (C `static`). Function names can be used as
values: `edu f: seyal(int,int): int = add;`.

### struct / union / enum / typedef
```
kattamaipu Point { x: int; y: int; }
kattamaipu Node  { val: int; next: Node*; }       # self reference through a pointer
inaippu    Word  { i: i32; f: f32; raw: u8[4]; }  # union: fields share storage
pattiyal   Color { Red, Green = 5, Blue }         # Blue is 6; values are constant expressions
peyar      Byte = u8;                             # alias; works for any type
```
Use: `Point { x: 1, y: 2 }` (struct literal, unset fields are 0), `p.x`,
`Color.Green`, `maatru(5, Color)`. A pointer to a struct uses `.` too: `n.next.val`.

### Classes
```
vagupu Name [: Parent] {
    [access] [thangu] field : type [= expr];       # initialiser only for thangu fields
    [access] uruvaakku ( params ) { ... }          # constructor (at most one)
    [access] [thangu] seyal method ( params ) [: type] { ... }
}
access ::= pothu (public, default) | paathukaappu (protected) | maraivu (private)
```
* Create: `edu d = puthu Dog("Rex");` — objects are heap references; `illai` is the null reference.
* Instance members: `ithu.field`, `obj.method(args)`. Static members: `Name.field`, `Name.method()`.
* All instance methods are **virtual**: a subclass method with the same name replaces it and must have the same signature.
* `thandhai(args);` must be the first statement of a constructor and runs the parent constructor
  (required when the parent constructor takes arguments). `thandhai.method(args)` calls the parent's version.
* A class value can be assigned to a variable of any ancestor class (upcast, implicit); `maatru(x, Sub)` downcasts (unchecked).
* Members of a `maraivu` field are visible only inside the declaring class; `paathukaappu` also in subclasses.

### Imports, Tamizhi headers and C bindings
```
serku "shapes.tzh";                       # Tamizhi header: functions, structs, enums, classes, globals
serku "math.tzb";                         # C binding file (see below)
veli "math.h";                            # C header to #include
veli seyal root = sqrt(x: float): float;  # root is the Tamizhi name, sqrt the C name
```
`serku` includes a file once (a second `serku` of the same file is ignored, cycles are safe) and the
path is looked up next to the including file first, then in the current directory.
* `*.tzh` (any extension except `.tzb`) is a **Tamizhi header**: it may contain every declaration a
  program can (`seyal`, `kattamaipu`, `inaippu`, `pattiyal`, `peyar`, `vagupu`, `edu`, `serku`, `veli`)
  but not `main`. Everything is compiled into your program; errors point at the header's own file and line.
* `*.tzb` is a **C binding file**: only `veli` and `serku` lines are allowed.

Binding types: all numeric types, `bool`, `string`, `void`, pointers, function pointers.

## 5. Statements

```
edu ...;                                   # declaration
expr ;                                     # expression statement
{ ... }                                    # block (own scope)
@( cond ) aga irundha { } ^( cond ) aga irundha { } illana { }
sutri ( cond ) { }                         # while
seithu { } sutri ( cond ) ;                # do-while: body runs at least once
ovvoru ( [edu x = e | expr] ; [cond] ; [update] ) { }    # for
thervu ( e ) { vazhakku 1, 2: { } iyalbu: { } }            # switch
udai ;   thodar ;   kudu [expr] ;
pathi ( "format", args... ) ;
```
`thervu` takes an integer, char or enum; labels are constants and unique; arms
**do not fall through** (each ends the switch); `udai` leaves the switch early.
`pathi` formats: `%d %i %u %x %X %o` (any integer type), `%f %e %g` (float types),
`%s` (string / `char*`), `%c` (char), `%%`; width/flags/precision work as in C;
the count and kinds are checked at compile time.

## 6. Expressions

Precedence, loosest first (all binary levels are left-associative except `=`):

| Level | Operators |
|-------|-----------|
| 1 | `=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=` (right) |
| 2 | `c ? a : b` |
| 3 | `||` |
| 4 | `&&` |
| 5 | `|` |
| 6 | `^` |
| 7 | `&` |
| 8 | `==  !=` |
| 9 | `<  <=  >  >=` |
| 10 | `<<  >>` |
| 11 | `+  -` |
| 12 | `*  /  %` |
| 13 | unary `-  !  ~  &  *  ++  --  maatru(e, T)  alavu(T or e)  puthu C(args)` |
| 14 | postfix `a[i]  a.f  f(args)  o.m(args)  x++  x--` |
| 15 | literals, names, `( e )`, `[a, b, c]` (array literal, declarations only), `T { f: e }`, `ithu`, `thandhai`, `illai` |

Notes: strings support `+` (concatenation, heap), `==` `<` … (strcmp), `s[i]` and `*s`
(read-only chars); `s += "x"` appends. Pointers support `+ - ++ --`, comparison, `p[i]`, `*p`, `&x`.
`^` as an operator is bitwise xor; after `}` of an `@` arm it starts an else-if.
Array indexing with a constant out-of-range index is a compile error.

## 7. Safety checks

Compile time: types, `nilai`, narrowing, access control, override signatures,
constructor chaining, constant index bounds, division by constant zero, shift
counts, duplicate `vazhakku` labels, `pathi` format/argument mismatch.

Run time (removed by `--no-checks`): null object / pointer use, index of a
fixed-size array, integer `/` and `%` by zero. A failure prints
`file:line: runtime error: …` and exits with status 1.

## 8. Version 2.1 additions

### List<T> and for-each
```
edu xs = puthu List<int>(3, 1, 2);     # or  puthu List<int>()
xs.push(9);  xs.insert(0, 7);          # append / insert at position
xs[1] = xs[1] + 100;   xs.set(2, 55);  # index (checked) / set
edu n = xs.length;                     # also works on strings: "abc".length
edu last = xs.pop();   edu r = xs.remove(0);   xs.get(1);   xs.clear();
ovvoru (edu x ulle xs) { pathi("%d\n", x); }   # for-each: List, fixed array or string
```
Lists are heap references (`illai` when never created); `List<List<int>>` and lists of structs/classes
work. Index/remove/pop errors are runtime errors (catchable, removed by `--no-checks`).

### Exceptions
```
muyarchi { erivi "too big"; } pidi (e) { pathi("%s\n", e); }
```
`erivi` throws a string. Runtime errors (bounds, divide by zero, null, list range) are catchable too.
An uncaught one prints `file:line: uncaught error: msg` and exits with status 1. `udai`, `thodar` and
`kudu` leave a `muyarchi` block correctly. Caveat: locals changed inside `muyarchi` and read in `pidi` should be
treated as volatile (C `setjmp` rule); there is no `finally`.

### Abstract classes
```
moolam vagupu Shape { moolam seyal area(): float; seyal show() { pathi("%f\n", ithu.area()); } }
```
An abstract class cannot be created with `puthu`; every concrete subclass must implement all `moolam` methods.

### Lambdas (non-capturing)
```
edu f = seyal(x: int, y: int): int { kudu x + y; };
```
A lambda is an anonymous function usable as a value (`seyal(int,int): int`). It sees globals but **cannot capture local variables**.

### Standard library
`serku "nool/all.tzh";` imports all of: `io` (read_line, read_int, read_float, read_file, write_file, append_file, file_exists),
`string` (str_len, str_sub, str_find, str_upper, str_lower, str_trim, str_starts, str_ends, str_replace, str_repeat, chr,
int_to_str, float_to_str, parse_int, parse_float), `math` (sqrt pow sin cos tan log exp floor ceil round fabs abs min max pi),
`os` (arg_count, arg_get, exit_program, env_get), `time` (clock_ms, time_now, sleep_ms, rand_seed, rand_int, rand_float).
Import one with e.g. `serku "nool/math.tzb";`. Library lookup: `$TAMIZHI_LIB`, `<exe>/lib`, `<exe>/../lib/tamizhi`, `/usr/local/lib/tamizhi`.
Library strings are malloc'd and never freed.

## 9. Version 2.2: diagnostics and range loops

Range loop: `ovvoru (edu i ulle 0 .. n) { }` runs `i = 0 … n-1` (`n` is evaluated once).

Every error and warning shows the source line with a caret under the token, and may carry a
`help:` line. Misspelt variables, functions, types, fields and methods get `did you mean 'x'?`.
```
prog.tz:5:19: error: undeclared identifier 'countr'
    5 |     pathi("%d\n", countr);
      |                   ^~~~~~
      = help: did you mean 'counter'?
```
New checks: a non-void function (or method / lambda) that can reach its end without `kudu` is an
error; **warnings** for a local variable never used (start the name with `_` to silence it),
unreachable code after `kudu`/`udai`/`thodar`/`erivi`, and an assignment used as a condition.
Options: `--no-warn`, `-Werror`, `--color=auto|always|never` (`NO_COLOR` is honoured).
At most 20 errors are shown, then the compiler stops.

## 10. Not supported (on purpose or yet)

`goto`, bit-fields, the comma operator, inline assembly, interfaces, `Map`/user generics, closures (lambdas cannot capture locals), `finally`, constructor/method overloading, multiple inheritance, generics,
pointer-to-const (`nilai` applies to the variable itself), `nilai` struct/class
fields, nested/local type declarations, variadic user functions, and unchecked
downcast safety. C evaluates function arguments in an unspecified order, so do
not rely on side-effect order inside one call.
