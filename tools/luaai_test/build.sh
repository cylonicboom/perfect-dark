#!/bin/sh
# Builds and runs the standalone action block -> Lua transpiler test.
# Requires only a C compiler; does NOT require the Perfect Dark ROM.
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
LUA_DIR=../../port/lua
$CC -O2 -I"$LUA_DIR" \
	test.c \
	../../src/game/luaai_transpile.c \
	"$LUA_DIR"/*.c \
	-lm \
	-o luaai_test
echo "--- running ---"
./luaai_test
