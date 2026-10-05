CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
# The generated src/runtime_src.h and src/std_src.h hold the whole Z runtime and
# the whole standard library as C string literals, so one string is tens of
# kilobytes. C99 only *requires* an implementation to accept 4095 characters in a
# literal; every compiler this targets handles tens of megabytes, and relying on
# that is far better than the alternative, which is a hand-maintained array of
# decimal byte values that no one can read or diff.
CFLAGS  += -Wno-overlength-strings -MMD -MP
LDFLAGS ?=

BIN     := z
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

# The standard library, in Z. Embedded into the compiler the same way the
# runtime is, so a built `z` is self-contained, and spliced ahead of every
# compilation unit -- which is what makes it a *library* rather than a set of
# compiler builtins: `StringBuilder` here is a class with methods, checked and
# compiled by exactly the front end a user's own code goes through, so a bug in
# it is a bug any program using it would have found.
#
# Sorted by filename, so the concatenation is deterministic and a build does not
# depend on directory order. Order matters only in that a name must be declared
# before it is used; nothing here depends on that, because the pre-scan reads the
# whole unit.
STDLIB  := lib/string.z lib/collections.z lib/io.z
STDLIB_SRC := $(firstword $(STDLIB))

.PHONY: all test test-all clean

all: $(BIN)

$(BIN): $(OBJ) src/runtime_src.h src/std_src.h
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# main.c includes the generated embedded-runtime and embedded-stdlib headers, so
# they must be built before main.o on a clean build.
src/main.o: src/runtime_src.h src/std_src.h

# The Z runtime is embedded into the compiler as a byte array so a built
# `z` is self-contained (no runtime file needed at compile time).
src/runtime_src.h: runtime/z_rt.c tools/embed.py
	@python3 tools/embed.py runtime_src runtime/z_rt.c > $@

# The standard library, the same way.
#
# One C string per file rather than one concatenated blob, plus a table naming
# them. That is what lets the driver lex each file on its own and stamp the right
# file name on the tokens, so a diagnostic inside the library says which file it
# is in -- the alternative is one NUL-terminated blob with every span pointing at
# a synthetic name and a line number counted across three files. The escaping is
# octal for every byte outside the printable ASCII range, so the generated file is
# valid C regardless of what the sources contain, and the result is readable
# enough to diff.
src/std_src.h: $(STDLIB) tools/embed.py
	@python3 tools/embed.py std_src $(STDLIB) > $@

# Golden + diagnostic tests; run after the compiler links.
test: $(BIN) tests/run
	@./tests/run ./$(BIN)

# The same suite at every optimization level.
#
# `make test` alone only exercises the default, and that is not enough: a pass
# that only runs at -O2 or -O3 can miscompile with the default level showing
# nothing wrong. Loop-invariant code motion shipped exactly that way -- hoisting
# an induction-variable expression out of the loop that varied it -- and the
# golden output was correct at -O1 and wrong at -O2. Every level has to agree.
test-all: $(BIN) tests/run
	@for o in -O0 -O1 -O2 -O3; do \
		printf '%s ' "$$o"; \
		Z_TEST_OPT=$$o ./tests/run ./$(BIN) | tail -1 || exit 1; \
	done

tests/run: tests/run.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN) $(OBJ) $(DEP) tests/run src/runtime_src.h src/std_src.h
	rm -f /tmp/z_*.s /tmp/z_*.out /tmp/z_*_rt.c /tmp/z_test_bin /tmp/z_test_dbg
	rm -f /tmp/z_leak_* /tmp/z_asan_probe_*

-include $(DEP)

# Benchmark the generated code.
#
# Not part of `test`: nothing here asserts correctness, it measures speed, and a
# speed number on a machine with a load average above 1 is not comparable to one
# taken when it was idle. `tools/bench.py` documents what it takes to make the
# numbers mean something.
bench: $(BIN)
	@python3 tools/bench.py
