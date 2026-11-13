# Tamizhi 2.5 — complete syntax reference

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
| `Map<K,V>` `Set<T>` | insertion-ordered hash map / set (heap) | section 10 |
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
A lambda that uses no local variables is a plain function value (`seyal(int,int): int`). One that does becomes a **closure** (section 11).

### Standard library
The library has two halves, laid out like a C library: **declarations** in `<prefix>/include/tamizhi/nool/`
(`.tzb` bindings, `.tzh` modules written in Tamizhi, and `tz_std.h`) and the **compiled code** in
`<prefix>/lib/tamizhi/libtamizhi.a`, which the compiler links automatically.

| prefix | include | lib |
|---|---|---|
| `~/.local/share` (current default, `make install`) | `~/.local/share/include/tamizhi` | `~/.local/share/lib/tamizhi` |
| `/usr` (system: `sudo make install PREFIX=/usr`) | `/usr/include/tamizhi` | `/usr/lib/tamizhi` |

Search order: `$TAMIZHI_INCLUDE` / `$TAMIZHI_LIB`, `stdlib/` next to the compiler (source tree), `~/.local/share`,
`/usr/local`, `/usr`. `make uninstall` removes it.

`serku "nool/all.tzh";` imports everything; import one module with e.g. `serku "nool/math.tzb";`.
Strings returned by the library are malloc'd and never freed.

| module | functions |
|---|---|
| `io` | read_line read_int read_float read_all read_file write_file append_file file_exists remove_file print_str eprint |
| `string` | str_len str_sub str_find str_upper str_lower str_trim str_starts str_ends str_replace str_repeat str_reverse str_count str_pad_left str_pad_right str_cmp str_capitalize str_is_digits str_is_alpha char_at ord chr int_to_str float_to_str parse_int parse_float parse_int_base to_hex to_bin |
| `math` | sqrt pow sin cos tan asin acos atan atan2 sinh cosh tanh log log10 log2 exp floor ceil round trunc fabs fmod cbrt hypot abs min max minf maxf clamp clampf sign gcd lcm ipow factorial is_prime is_nan deg_to_rad rad_to_deg pi euler inf |
| `os` | arg_count arg_get exit_program env_get |
| `time` | clock_ms time_now sleep_ms rand_seed rand_seed_now rand_int rand_float format_time now_iso year month day hour minute second weekday |
| `fs` | file_size is_file is_dir rename_file copy_file make_dir remove_dir list_dir current_dir change_dir home_dir temp_dir path_join path_base path_dir path_ext |
| `process` | run_command capture_command get_pid hostname |
| `encoding` | hash_djb2 hash_fnv1a crc32 sha256 base64_encode/decode hex_encode/decode url_encode/decode |
| `regex` | regex_match regex_find regex_replace (POSIX extended) |
| `net` | tcp_connect tcp_listen tcp_accept net_port net_send net_recv net_recv_line net_wait net_close net_peer; http_get http_post http_request http_status http_headers (plain `http://` only, redirects and chunked replies handled, no TLS) |
| `json` (text based) | json_valid json_get json_type json_has json_len json_keys json_escape json_quote json_pretty json_minify; paths like `users[0].name` |
| `collections` (generic, written in Tamizhi) | Stack<T> Queue<T> Pair<A, B> min_of max_of swap_values |
| `listutil` (written in Tamizhi) | sum_int sum_float max_int min_int index_of_int index_of_str contains_int reverse_int sort_int sort_float sort_str range_list |

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

## 10. Version 2.3: Map, Set, debug info, hardening

```
edu ages = puthu Map<string, int>();        # created empty
ages.put("asha", 31);   ages["ravi"] = 27;  # insert / overwrite
ages["asha"] += 1;      ages["n"] += 5;     # a missing key starts at 0 (or false / illai) when written
edu a = ages["asha"];   ages.get_or("zed", -1);   # reading a missing key is a runtime error (catchable)
ages.has("ravi");  ages.remove("ravi");  ages.clear();  ages.length;
edu ks = ages.keys();   edu vs = ages.values();         # new List<K> / List<V>
ovvoru (edu k ulle ages) { pathi("%s\n", k); }           # keys, in insertion order

edu seen = puthu Set<int>(3, 1, 3);         # initial items are optional
seen.add(9);  seen.has(1);  seen.remove(3);  seen.items();  seen.length
```
Keys are integers, `char`, `bool`, enums or strings. Map values can be any type except arrays, function
types and `void`. `add` and `remove` return `bool` (was it new / was it present). Iteration order is insertion
order, so output is deterministic.

**Debug info:** `-g` (or `--lines`) emits `#line` directives, so gdb, valgrind and C compiler messages
refer to your `.tz` file and line numbers.

**Hardening:** nesting deeper than 1000 levels is reported as an error instead of crashing the compiler,
the compiler runs on a 1 GiB stack so very long expressions work, and `make fuzz` runs mutation fuzzing
(ASan build) plus a constant-folding-versus-runtime differential test.

## 11. Version 2.4: closures

```
seyal make_adder(n: int): Fn<(int): int> {            # Fn<(params): result> is the closure type
    kudu seyal(x: int): int { kudu x + n; };          # uses n, so it captures it
}
edu add5 = make_adder(5);
pathi("%d\n", add5(1));                               # 6
edu count = 0;
edu next = seyal(): int { count += 1; kudu count; };  # the closure has its own copy of count
next(); next();                                       # 1, 2   (the outer 'count' stays 0)
```
* Captures are **by value**: the closure copies each local it uses when it is created and keeps (and may
  modify) its own copy. Two closures from the same function do not share state.
* An `Fn<…>` value can hold a closure, a lambda with no captures, or the name of an ordinary function
  (conversion is implicit). An `Fn` cannot be passed where a plain `seyal(…)` function type is expected
  (C libraries need plain pointers), but a non-capturing lambda can.
* Closures work as variables, parameters, return values, struct/class fields (`b.on_click(1)`) and List items.
  Lambdas nest; the inner one may use variables of the outermost function.
* Not allowed: capturing an array (capture a `List`), using `ithu` directly (copy it to a local first).
* Closure environments live on the heap (freed by the GC with `--gc`, leaked with `--no-gc`).
* Calling a never-assigned `Fn` is a runtime error.
* Function calls now report `'f' expects N arguments but M were given` for too many arguments as well as too few.

## 12. Version 2.5: strings

```
edu msg = $"Hello {name}, {age + 1} next year, pi = {pi:.2f}";   # interpolation: $"..." with {expr}
edu a = $"{{literal braces}}";                                    # {{ and }} are literal braces
str(42)  str(2.5)  str(true)  str(x, "05d")  str(f, ".3f")       # explicit conversion (+ printf-style format)
```
`{expr:format}` uses the same format as `str`: flags, width, `.precision` and one of `d x X o u c f e g s`
(`{255:x}`, `{7:03d}`, `{pi:8.3f}`). Interpolated strings build a new heap string (like `+`).

String methods (all return new strings; the original never changes):

| Method | Result |
|--------|--------|
| `s.upper()` `s.lower()` `s.trim()` | string |
| `s.contains(t)` `s.starts(t)` `s.ends(t)` | bool |
| `s.find(t)` | index of the first match, or -1 |
| `s.sub(start, len)` | substring (clamped to the string, like a slice) |
| `s.replace(a, b)` `s.repeat(n)` | string |
| `s.split(sep)` | `List<string>` (`"a,,b".split(",")` has 3 items; empty `sep` gives one item) |
| `s.to_int()` `s.to_float()` | number (0 if the text is not a number) |
| `list.join(sep)` | string, for a `List<string>` |

## 13. Version 2.6: typed exceptions and `finally`

`erivi` throws a string or an object of any class. `pidi` can name the type it
catches; clauses are tried in order and the first match wins. With no match the
exception continues outwards.

```
muyarchi { ... }
pidi (e: ParseError) { pathi("%s %d\n", e.msg, e.code); }  # class (or subclass)
pidi (e: IOError)    { ... }
pidi (e: string)     { ... }   # string throws and runtime errors
pidi (e)             { ... }   # catch-all; e is a string (the class name for objects)
irudhi { ... }                 # finally: always runs
```

- `irudhi` runs on normal exit, after a caught or uncaught exception, and on
  `kudu`, `udai` and `thodar` leaving the block. It may be used without `pidi`.
- An uncaught object prints `file:line: uncaught exception: ClassName`.
- A `muyarchi` needs at least one `pidi` or an `irudhi`.

Any expression holding an `Fn` can be called directly: `add(3)(4)`,
`fs.get(1)(5)`, `m.get("k")(1)`. Calling something that is not a function is an
error ("cannot be called").

## 14. Version 2.7: interfaces

```
idaimugam Shape { seyal area(): float; seyal name(): string; }

vagupu Sq : Base, Shape, Scalable { ... }   # optional parent class first, then interfaces
vagupu Circ : Shape { ... }                  # or interfaces only
```

- An interface lists method signatures only (no fields, bodies, `thangu`, or `moolam`).
- A class must provide every method of each interface it names, with the same
  signature (inherited methods count; a `moolam` class may re-declare one as `moolam seyal`).
- An interface type can be used for variables, parameters, fields and `List<Shape>`;
  a class converts to it implicitly, and `maatru(value, Class)` converts back.
- Calls through an interface are dispatched by the object's real class. A null
  interface value is caught as a runtime error. Interfaces cannot extend each other.

## 15. Version 2.9: tuples, Dict, generics

**Dict / Set.** `Dict<K, V>` is another name for `Map<K, V>`; `Set<T>` is as before.

**Tuples** hold a fixed number of values of different types.

```
seyal divmod(a: int, b: int): (int, int) { kudu (a / b, a % b); }
edu t = divmod(17, 5);
pathi("%d %d", t.0, t.1);          # elements are .0 .1 ... ; t.0 = 10 assigns
edu (q, r) = divmod(100, 7);       # destructuring; use _ to skip an element
edu xs = puthu List<(string, int)>();   # tuples work inside List, Map, fields, ...
```
A tuple has 2 to 16 elements. Two tuples with the same element types are the same type.

**Generics** make functions, classes and structs that work for any type. A generic
declaration is checked each time it is used with concrete types.

```
seyal first<T>(xs: List<T>): T { kudu xs.get(0); }
first(xs)                  # T is inferred from the arguments
first<int>(xs)             # or written out

vagupu Box<T> {
    v: T;
    uruvaakku(x: T) { ithu.v = x; }
    seyal get(): T { kudu ithu.v; }
}
edu b = puthu Box<int>(5);
edu nested = puthu Box<Box<string>>(puthu Box<string>("hi"));

kattamaipu Pair<A, B> { first: A; second: B; }
edu p = Pair<int, string> { first: 1, second: "x" };
```
- A generic must be declared before it is used with `<...>` (a generic function may be called
  by inference from anywhere in the program).
- Inference looks at the arguments only; a type parameter that appears nowhere in the
  parameters (for example just the return type) must be written out: `make<int>()`.
- A generic class cannot extend a generic class yet, and type parameters cannot have
  constraints: using `<` on a type that lacks it is reported as an error for that instance.
- `nool/collections.tzh` has `Stack<T>`, `Queue<T>`, `Pair<A, B>`, `min_of`, `max_of`, `swap_values`.

## 16. Not supported (on purpose or yet)

`goto`, bit-fields, the comma operator, inline assembly, interfaces, user generics, `finally`, constructor/method overloading, multiple inheritance, generics,
pointer-to-const (`nilai` applies to the variable itself), `nilai` struct/class
fields, nested/local type declarations, variadic user functions, and unchecked
downcast safety. C evaluates function arguments in an unspecified order, so do
not rely on side-effect order inside one call.
