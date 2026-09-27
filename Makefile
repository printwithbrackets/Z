CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
CFLAGS  += -MMD -MP
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
src/runtime_src.h: runtime/z_rt.c
	@python3 -c "d=open('runtime/z_rt.c','rb').read(); print('/* generated */'); print('static const char runtime_src[] = {' + ','.join(str(b) for b in d) + ',0};')" > $@

# The standard library, the same way. Each file is announced with a line comment
# naming it, so a diagnostic that points into the prelude names the file a reader
# can go and read.
src/std_src.h: $(STDLIB)
	@python3 -c "import sys; out=['/* generated */','static const char std_src[] = {']; [out.append('/* --- %s --- */' % p) or out.append(','.join(str(b) for b in open(p,'rb').read())) for p in sys.argv[1:]]; out.append(',0};'); print('\n'.join(out))" $(STDLIB) > $@

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

-include $(DEP)
