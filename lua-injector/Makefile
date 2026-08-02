# lua-injector build
#
# Default toolchain: `zig cc` — cross-compiles Windows binaries from any
# host (Linux/macOS/Windows), because zig bundles a MinGW-w64 toolchain.
# Any MinGW-w64 gcc works too, e.g.:
#     make CC64=x86_64-w64-mingw32-gcc CC32=i686-w64-mingw32-gcc
#
# Targets:
#     make            build Windows binaries + Linux core test
#     make windows    build only the Windows binaries
#     make test       build and run the Linux core test
#     make clean

ZIG    ?= zig
CC64   ?= $(ZIG) cc -target x86_64-windows-gnu
CC32   ?= $(ZIG) cc -target x86-windows-gnu
HOSTCC ?= gcc

LUA     = third_party/lua
LUA_SRC = $(filter-out $(LUA)/lua.c $(LUA)/luac.c,$(wildcard $(LUA)/*.c))

X64_OBJ = $(patsubst $(LUA)/%.c,build/x64/%.o,$(LUA_SRC))
X86_OBJ = $(patsubst $(LUA)/%.c,build/x86/%.o,$(LUA_SRC))
LNX_OBJ = $(patsubst $(LUA)/%.c,build/lnx/%.o,$(LUA_SRC))

BIN = bin

.PHONY: all windows test run-test clean

all: windows test

windows: $(BIN)/lua-injector-x64.exe $(BIN)/lua-injector-x86.exe \
         $(BIN)/lua-injector-gui-x64.exe $(BIN)/lua-injector-gui-x86.exe \
         $(BIN)/lua-host-x64.dll  $(BIN)/lua-host-x86.dll \
         $(BIN)/lua-test-game-x64.exe $(BIN)/lua-test-game-x86.exe

test: build/lua-core-test build/game-sim-test
run-test: build/lua-core-test build/game-sim-test
	./build/lua-core-test < host/test_scripts.lua
	@echo "== game simulation =="
	./build/game-sim-test

# ---------------------------------------------------------------- dirs
build/x64 build/x86 build/lnx $(BIN):
	@mkdir -p $@

# ------------------------------------------------------- Lua objects
build/x64/%.o: $(LUA)/%.c | build/x64
	$(CC64) -O2 -c $< -o $@

build/x86/%.o: $(LUA)/%.c | build/x86
	$(CC32) -O2 -c $< -o $@

build/lnx/%.o: $(LUA)/%.c | build/lnx
	$(HOSTCC) -O2 -fPIC -c $< -o $@

# ------------------------------------------------------ host DLLs
$(BIN)/lua-host-x64.dll: host/win_host.c host/luacore.c host/luacore.h \
                         $(X64_OBJ) | $(BIN)
	$(CC64) -O2 -shared -I$(LUA) -Ihost host/win_host.c host/luacore.c \
	        $(X64_OBJ) -o $@

$(BIN)/lua-host-x86.dll: host/win_host.c host/luacore.c host/luacore.h \
                         $(X86_OBJ) | $(BIN)
	$(CC32) -O2 -shared -I$(LUA) -Ihost host/win_host.c host/luacore.c \
	        $(X86_OBJ) -o $@

# ------------------------------------------------------ clients
$(BIN)/lua-injector-x64.exe: injector/injector.c client/common.c \
                             client/common.h | $(BIN)
	$(CC64) -O2 -Iclient injector/injector.c client/common.c -o $@

$(BIN)/lua-injector-x86.exe: injector/injector.c client/common.c \
                             client/common.h | $(BIN)
	$(CC32) -O2 -Iclient injector/injector.c client/common.c -o $@

$(BIN)/lua-injector-gui-x64.exe: gui/gui.c client/common.c client/common.h | $(BIN)
	$(CC64) -O2 -Iclient -Wl,/subsystem:windows gui/gui.c client/common.c \
	        -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -o $@

$(BIN)/lua-injector-gui-x86.exe: gui/gui.c client/common.c client/common.h | $(BIN)
	$(CC32) -O2 -Iclient -Wl,/subsystem:windows gui/gui.c client/common.c \
	        -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -o $@

# ------------------------------------------------------ test game
$(BIN)/lua-test-game-x64.exe: test_game/win_game.c test_game/game_logic.c \
                              test_game/game_logic.h | $(BIN)
	$(CC64) -O2 -Itest_game -Wl,/subsystem:windows test_game/win_game.c \
	        test_game/game_logic.c -lgdi32 -luser32 -o $@

$(BIN)/lua-test-game-x86.exe: test_game/win_game.c test_game/game_logic.c \
                              test_game/game_logic.h | $(BIN)
	$(CC32) -O2 -Itest_game -Wl,/subsystem:windows test_game/win_game.c \
	        test_game/game_logic.c -lgdi32 -luser32 -o $@

# ------------------------------------------------------ Linux tests
build/lua-core-test: host/test_host.c host/luacore.c host/luacore.h \
                     $(LNX_OBJ)
	$(HOSTCC) -O1 -g -I$(LUA) -Ihost host/test_host.c host/luacore.c \
	          $(LNX_OBJ) -lm -ldl -o $@

build/game-sim-test: test_game/game_sim.c test_game/game_logic.c \
                     test_game/game_logic.h
	$(HOSTCC) -O1 -g -Itest_game test_game/game_sim.c \
	          test_game/game_logic.c -o $@

clean:
	rm -rf build $(BIN)
