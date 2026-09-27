CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
CFLAGS  += -MMD -MP
LDFLAGS ?=

BIN     := z
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

.PHONY: all test test-all clean

all: $(BIN)

$(BIN): $(OBJ) src/runtime_src.h
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# main.c includes the generated embedded-runtime header, so it must be built
# before main.o on a clean build.
src/main.o: src/runtime_src.h

# The Z runtime is embedded into the compiler as a byte array so a built
# `z` is self-contained (no runtime file needed at compile time).
src/runtime_src.h: runtime/z_rt.c
	@python3 -c "d=open('runtime/z_rt.c','rb').read(); print('/* generated */'); print('static const char runtime_src[] = {' + ','.join(str(b) for b in d) + ',0};')" > $@

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
	rm -f $(BIN) $(OBJ) $(DEP) tests/run
	rm -f /tmp/z_*.s /tmp/z_*.out /tmp/z_*_rt.c

-include $(DEP)
