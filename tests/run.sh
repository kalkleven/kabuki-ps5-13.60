#!/usr/bin/env bash
# Host checks for tiles, settings, the librespot adapter, the broker, the C SDK and the panel's model
# (any C++20 compiler).
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
cxx=${CXX:-g++}
mkdir -p "$repo/build/tests"
flags=(-std=c++20 -Wall -Wextra -Werror -Wno-deprecated-literal-operator
       -I"$repo/src/common" -I"$repo/src/loader" -I"$repo/src/ui" -I"$repo/src/daemon" -I"$repo/sdk/c" -I"$repo/.deps/nlohmann/include")
"$cxx" "${flags[@]}" "$repo/tests/prologue.cpp" -o "$repo/build/tests/prologue"
"$repo/build/tests/prologue"
"$cxx" "${flags[@]}" "$repo/tests/tiles.cpp" -o "$repo/build/tests/tiles"
"$repo/build/tests/tiles"
"$cxx" "${flags[@]}" -pthread "$repo/tests/sdk.cpp" -o "$repo/build/tests/sdk"
"$repo/build/tests/sdk"
"$cxx" "${flags[@]}" "$repo/tests/layout.cpp" -o "$repo/build/tests/layout"
"$repo/build/tests/layout"

"$cxx" "${flags[@]}" -pthread "$repo/tests/plugins.cpp" -o "$repo/build/tests/plugins"
"$repo/build/tests/plugins"
