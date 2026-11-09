# W-CODE -- the WuBuOS agentic IDE
#
# Plain C11, no dependencies beyond libc.  Both frontends (human TUI and agent
# JSON) link the same core, so `make test` exercises what both use.

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra
LDFLAGS ?=

SRC     := src/wcode_buf.c src/wcode_syntax.c src/wcode_fs.c \
           src/wcode_build.c src/wcode_agent.c
CORE    := $(SRC) src/wcode_tui.c

BIN     := wcode
TESTBIN := wcode_test

.PHONY: all test clean agent-demo install

all: $(BIN)

$(BIN): $(CORE) src/main.c
	$(CC) $(CFLAGS) -Isrc $(CORE) src/main.c -o $@ $(LDFLAGS)

# The test binary links the core but its own main.
$(TESTBIN): $(SRC) tests/wcode_test.c
	$(CC) $(CFLAGS) -Isrc $(SRC) tests/wcode_test.c -o $@ $(LDFLAGS)

test: $(TESTBIN)
	./$(TESTBIN)

agent-demo: $(BIN)
	@printf '%s\n' \
	  '{"op":"status"}' \
	  '{"op":"help"}' \
	  '{"op":"open","path":"src/wcode.h"}' \
	  '{"op":"status"}' \
	  '{"op":"edit","line":1,"col":1,"insert":"/* W-CODE */\\n"}' \
	  '{"op":"status"}' \
	  '{"op":"save"}' \
	  '{"op":"build","cwd":".","argv":["make","-n"]}' \
	  '{"op":"diags"}' | ./$(BIN) --agent

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)/usr/local/bin/$(BIN)

clean:
	rm -f $(BIN) $(TESTBIN)
