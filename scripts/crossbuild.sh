#!/usr/bin/env bash
# Cross-compiles the whole tree for Windows with mingw-w64.
#
# This is the portability/compile check, not the shipping build -- the
# supported build is MSVC (see README). It exists because it is the only way
# to verify the WASAPI code on a machine that is not Windows.
set -euo pipefail
cd "$(dirname "$0")/.."
GUI="${1:-ON}"
cmake -S . -B build-cross-cmake \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DAUDIOMON_BUILD_GUI="$GUI"
cmake --build build-cross-cmake -j"$(nproc)"
echo
echo "Built:"
find build-cross-cmake -name '*.exe' -printf '  %p (%s bytes)\n'
