CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
CFLAGS  += -MMD -MP
LDFLAGS ?=

BIN     := z
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

.PHONY: all test clean

all: $(BIN)

$(BIN): $(OBJ) src/runtime_src.h
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# main.c includes the generated embedded-runtime header, so it must be built
# before main.o on a clean build.
src/main.o: src/runtime_src.h

# The Vela runtime is embedded into the compiler as a byte array so a built
# `vela` is self-contained (no runtime file needed at compile time).
src/runtime_src.h: runtime/vela_rt.c
	@python3 -c "d=open('runtime/vela_rt.c','rb').read(); print('/* generated */'); print('static const char runtime_src[] = {' + ','.join(str(b) for b in d) + ',0};')" > $@

# Golden + diagnostic tests; run after the compiler links.
test: $(BIN) tests/run
	@./tests/run ./$(BIN)

tests/run: tests/run.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BIN) $(OBJ) $(DEP) tests/run
	rm -f /tmp/z_*.s /tmp/z_*.out /tmp/z_*_rt.c

-include $(DEP)
