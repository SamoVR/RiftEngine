#!/usr/bin/env bash
# Builds and runs every unit test with AddressSanitizer + UBSan (not the e2e test).
# Run from the project root:  bash tests/run_tests.sh
set -u
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/rift_tests
mkdir -p "$OUT/lua"

INC="-I. -Iimgui -Ilibraries/glm -Ilibraries/glew/include -Ilibraries/glfw/include -Ilibraries/json-develop/single_include -Ilibraries/stb -Ilibraries/lua -Ilibraries/sol2/include"
FLAGS="-std=c++20 -w -pthread -g -O0 -fsanitize=address,undefined -DGLEW_NO_GLU -DGLFW_INCLUDE_NONE -DSOL_USING_CXX_LUA=1"
LINK="-Wl,--unresolved-symbols=ignore-all"
CORE="Object.cpp Scene.cpp Geometry.cpp Texture.cpp Shader.cpp"
RUNTIME="GameRuntime.cpp LuaRuntime.cpp ScriptStore.cpp InputManager.cpp Camera.cpp $CORE"

echo "== building Lua (as C++)"
for f in libraries/lua/*.c; do g++ -x c++ -std=c++20 -c -O1 -w -Ilibraries/lua -DLUA_USE_POSIX "$f" -o "$OUT/lua/$(basename "$f" .c).o" & done; wait
ar rcs "$OUT/liblua.a" "$OUT"/lua/*.o

fail=0
run() { # name, sources...
  local name=$1; shift
  echo "== $name"
  if g++ $FLAGS $INC "tests/$name.cpp" "$@" -o "$OUT/$name" $LINK 2>"$OUT/$name.log"; then
    ASAN_OPTIONS=detect_leaks=0 "$OUT/$name" 2>&1 | grep -E "FAIL|PASSED|FAILED|ERROR: AddressSanitizer|runtime error" | head -20
    [ "${PIPESTATUS[0]}" -eq 0 ] || fail=1
  else
    echo "build failed, see $OUT/$name.log"; grep error "$OUT/$name.log" | head -5; fail=1
  fi
}

run test_transform
run test_core $CORE
run test_commands $CORE AnimationSequencer.cpp
run test_scriptstore ScriptStore.cpp
run test_runtime $RUNTIME "$OUT/liblua.a"
run test_lua $RUNTIME "$OUT/liblua.a"
run test_scenes $RUNTIME "$OUT/liblua.a"
[ -f tests/test_sequencer.cpp ] && run test_sequencer AnimationSequencer.cpp $CORE imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp

[ $fail -eq 0 ] && echo && echo "ALL TEST SUITES PASSED" || { echo; echo "SOME TEST SUITES FAILED"; exit 1; }
