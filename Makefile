BUILD   := build
BIN     := $(BUILD)/realmem
PREFIX  ?= $(HOME)/.local
CMAKE   ?= cmake
GEN     ?= Ninja
TYPE    ?= Release

.PHONY: all configure build test install uninstall run clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD) -G $(GEN) -DCMAKE_BUILD_TYPE=$(TYPE)

build: $(BUILD)/build.ninja
	$(CMAKE) --build $(BUILD)

$(BUILD)/build.ninja: CMakeLists.txt
	$(CMAKE) -S . -B $(BUILD) -G $(GEN) -DCMAKE_BUILD_TYPE=$(TYPE)

test: build
	ctest --test-dir $(BUILD) --output-on-failure

# Symlink rather than copy, so `make` alone refreshes what's on PATH.
install: build
	mkdir -p $(PREFIX)/bin
	ln -sf $(abspath $(BIN)) $(PREFIX)/bin/realmem
	@echo "linked $(PREFIX)/bin/realmem -> $(abspath $(BIN))"

uninstall:
	rm -f $(PREFIX)/bin/realmem

run: build
	@$(BIN)

clean:
	rm -rf $(BUILD)
