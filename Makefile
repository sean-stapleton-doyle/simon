CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
BUILD   := build
BIN     := $(BUILD)/realmem
PREFIX  ?= $(HOME)/.local

.PHONY: all clean install uninstall run

all: $(BIN)

$(BIN): realmem.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# Symlink rather than copy, so `make` alone refreshes what's on PATH.
install: $(BIN)
	mkdir -p $(PREFIX)/bin
	ln -sf $(abspath $(BIN)) $(PREFIX)/bin/realmem
	@echo "linked $(PREFIX)/bin/realmem -> $(abspath $(BIN))"

uninstall:
	rm -f $(PREFIX)/bin/realmem

run: $(BIN)
	@$(BIN)

clean:
	rm -rf $(BUILD)
