# Tamizhi compiler — build and test
#
#   make          build ./tamizhi
#   make test     run program tests, error tests and optimiser checks
#   make asan     rebuild with AddressSanitizer + UBSan (./tamizhi-asan)
#   make clean

CC       ?= gcc
CFLAGS   ?= -O2 -g
WARN     := -std=gnu11 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
TARGET   := tamizhi
SRCS     := main.c util.c lexer.c ast.c parser.c sema.c codegen.c
OBJS     := $(SRCS:.c=.o)
HDRS     := tamizhi.h lexer.h ast.h parser.h sema.h codegen.h

.PHONY: all test asan clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm -lpthread

%.o: %.c $(HDRS)
	$(CC) $(WARN) $(CFLAGS) -c -o $@ $<

# Robustness checks: mutation fuzzing (needs `make asan`) and folding-vs-runtime.
fuzz: asan $(TARGET)
	python3 tests/fuzz/folding.py 1
	python3 tests/fuzz/folding.py 2
	python3 tests/fuzz/fuzz.py 1 3000

asan: $(SRCS) $(HDRS)
	$(CC) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(TARGET)-asan $(SRCS) -lm -lpthread

# ── Tests ────────────────────────────────────────────────────────────────
# BIN lets `make test BIN=./tamizhi-asan` run the suite under the sanitizers.
BIN ?= ./$(TARGET)
# GCFLAGS lets the --gc test find libgc, e.g. `make test GCFLAGS=-L/opt/gc/lib`.
GCFLAGS ?=

test: $(TARGET)
	@tmp=$$(mktemp -d); pass=0; fail=0; \
	echo "── program tests (compile, run, compare output) ──"; \
	for tz in tests/*.tz; do \
	  name=$$(basename $$tz .tz); printf "  %-24s" $$name; \
	  if $(BIN) $$tz -o $$tmp/$$name 2>$$tmp/err; then \
	    if [ "$$($$tmp/$$name 2>&1 | sed 's/[ \t]*$$//')" = "$$(sed 's/[ \t]*$$//' tests/$$name.expected)" ]; \
	    then echo "PASS"; pass=$$((pass+1)); \
	    else echo "FAIL (wrong output)"; fail=$$((fail+1)); fi; \
	  else echo "FAIL (did not compile)"; cat $$tmp/err; fail=$$((fail+1)); fi; \
	done; \
	echo "── error tests (must be rejected with the expected message) ──"; \
	for tz in tests/errors/*.tz; do \
	  name=$$(basename $$tz .tz); printf "  %-24s" $$name; \
	  if $(BIN) $$tz -o $$tmp/x 2>$$tmp/err; then \
	    echo "FAIL (was accepted)"; fail=$$((fail+1)); \
	  elif grep -qF -- "$$(cat tests/errors/$$name.expected)" $$tmp/err; \
	  then echo "PASS"; pass=$$((pass+1)); \
	  else echo "FAIL (wrong message)"; cat $$tmp/err; fail=$$((fail+1)); fi; \
	done; \
	echo "── C bindings and heap (need extra command-line files or flags) ──"; \
	run() { printf "  %-24s" "$$1"; shift; exp="$$1"; shift; \
	  if "$$@" 2>$$tmp/err && [ "$$($$tmp/out 2>&1)" = "$$(cat $$exp)" ]; \
	  then echo "PASS"; pass=$$((pass+1)); \
	  else echo "FAIL"; cat $$tmp/err; fail=$$((fail+1)); fi; }; \
	run "own C library (.c file)" tests/ext/main.expected $(BIN) tests/ext/main.tz tests/ext/mylib.c -o $$tmp/out; \
	run "heap with --no-gc" tests/heap/strcat.expected $(BIN) tests/heap/strcat.tz --no-gc -o $$tmp/out; \
	run "classes (virtual calls)" tests/oop/classes.expected $(BIN) tests/oop/classes.tz --no-gc -o $$tmp/out; \
	run "Tamizhi header (.tzh)" tests/hdr/main.expected $(BIN) tests/hdr/main.tz --no-gc -o $$tmp/out; \
	run "List<T> and foreach" tests/v21/list.expected $(BIN) tests/v21/list.tz --no-gc -o $$tmp/out; \
	run "try / catch / throw" tests/v21/try.expected $(BIN) tests/v21/try.tz --no-gc -o $$tmp/out; \
	run "abstract classes" tests/v21/abstract.expected $(BIN) tests/v21/abstract.tz --no-gc -o $$tmp/out; \
	run "lambdas" tests/v21/lambda.expected $(BIN) tests/v21/lambda.tz --no-gc -o $$tmp/out; \
	run "standard library" tests/v21/stdlib.expected $(BIN) tests/v21/stdlib.tz --no-gc -o $$tmp/out; \
	run "closures" tests/v21/closure.expected $(BIN) tests/v21/closure.tz --no-gc -o $$tmp/out; \
	run "Map and Set" tests/v21/map.expected $(BIN) tests/v21/map.tz --no-gc -o $$tmp/out; \
	printf "  %-24s" "deep nesting rejected"; \
	awk 'BEGIN{printf "seyal main(): int { kudu "; for(i=0;i<5000;i++)printf "("; printf "1"; for(i=0;i<5000;i++)printf ")"; print "; }"}' > $$tmp/deep.tz; \
	if $(BIN) $$tmp/deep.tz -S 2>&1 | grep -q "nested too deeply"; then echo "PASS"; pass=$$((pass+1)); else echo "FAIL"; fail=$$((fail+1)); fi; \
	printf "  %-24s" "long sum compiles"; \
	awk 'BEGIN{printf "seyal main(): int { edu x = 1"; for(i=0;i<100000;i++)printf "+1"; print "; pathi(\"%d\\n\", x); kudu 0; }"}' > $$tmp/sum.tz; \
	if $(BIN) $$tmp/sum.tz --no-gc -o $$tmp/sum 2>/dev/null && [ "`$$tmp/sum`" = "100001" ]; then echo "PASS"; pass=$$((pass+1)); else echo "FAIL"; fail=$$((fail+1)); fi; \
	run "range loop" tests/v21/range.expected $(BIN) tests/v21/range.tz --no-gc -o $$tmp/out; \
	run "runtime: array bounds" tests/rt/bounds.expected $(BIN) tests/rt/bounds.tz --no-gc -o $$tmp/out; \
	run "runtime: divide by zero" tests/rt/divzero.expected $(BIN) tests/rt/divzero.tz --no-gc -o $$tmp/out; \
	run "runtime: null object" tests/rt/null.expected $(BIN) tests/rt/null.tz --no-gc -o $$tmp/out; \
	printf "  %-24s" "warnings / -Werror"; \
	if $(BIN) tests/v21/warn.tz -o $$tmp/w 2>$$tmp/werr && grep -q "never used" $$tmp/werr && \
	   ! $(BIN) tests/v21/warn.tz -Werror -o $$tmp/w 2>/dev/null; \
	then echo "PASS"; pass=$$((pass+1)); else echo "FAIL"; fail=$$((fail+1)); fi; \
	printf "  %-24s" "--no-checks removes them"; \
	if $(BIN) tests/rt/bounds.tz --no-checks --no-gc -S | grep -q "tz_bc"; then echo "FAIL"; fail=$$((fail+1)); \
	else echo "PASS"; pass=$$((pass+1)); fi; \
	printf "  %-24s" "heap with --gc"; \
	if echo 'int main(void){return 0;}' | $(CC) -x c - $(GCFLAGS) -lgc -o $$tmp/probe 2>/dev/null; then \
	  if $(BIN) tests/heap/strcat.tz --gc $(GCFLAGS) -o $$tmp/out 2>$$tmp/err && \
	     [ "$$($$tmp/out 2>&1)" = "$$(cat tests/heap/strcat.expected)" ]; \
	  then echo "PASS"; pass=$$((pass+1)); else echo "FAIL"; cat $$tmp/err; fail=$$((fail+1)); fi; \
	else echo "SKIP (libgc not found; try GCFLAGS=-L<dir>)"; fi; \
	printf "  %-24s" "no runtime unless used"; \
	if $(BIN) tests/01_hello.tz -S | grep -q "tz_alloc\|GC_"; then echo "FAIL"; fail=$$((fail+1)); \
	else echo "PASS"; pass=$$((pass+1)); fi; \
	echo "── optimiser checks (inspect generated C) ──"; \
	$(BIN) tests/opt/fold.tz -S > $$tmp/fold.c; \
	check() { printf "  %-24s" "$$1"; shift; \
	  if "$$@"; then echo "PASS"; pass=$$((pass+1)); \
	  else echo "FAIL"; fail=$$((fail+1)); fi; }; \
	check "constant folding"   grep -qF "long long tz_a = 42;" $$tmp/fold.c; \
	check "peephole x+0 -> x"  grep -qF "long long tz_c = tz_a;" $$tmp/fold.c; \
	check "x*0 -> 0"           grep -qF "long long tz_e = 0;" $$tmp/fold.c; \
	check "dead if removed"    sh -c '! grep -q NEVER $$0' $$tmp/fold.c; \
	check "while(false) gone"  sh -c '! grep -q "while" $$0' $$tmp/fold.c; \
	check "no dangling else"   sh -c '! grep -Eq "^ *else" $$0' $$tmp/fold.c; \
	$(BIN) tests/opt/fold.tz -o $$tmp/fold && \
	check "folded program runs" sh -c '[ "$$($$0)" = "$$(cat tests/opt/fold.expected)" ]' $$tmp/fold; \
	rm -rf $$tmp; \
	echo "──────────────────────────────────────────────"; \
	echo "  passed: $$pass   failed: $$fail"; \
	[ $$fail -eq 0 ]

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET)-asan
