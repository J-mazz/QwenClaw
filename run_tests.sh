#!/bin/bash
# Build and run the test suite. Logs go to ninja_out.txt / test_out.txt in
# the repo root. Override the build directory with BUILD_DIR if needed.
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-cmake43}"

if [ ! -d "$BUILD_DIR" ]; then
  echo "Build directory not found: $BUILD_DIR" >&2
  echo "Configure first, e.g.: cmake --preset gcc16-ninja" >&2
  exit 1
fi

cd "$BUILD_DIR" || exit 1
ninja -j"$(nproc)" quantclaw_tests > "$ROOT/ninja_out.txt" 2>&1
ninja_exit=$?
echo "ninja exit: $ninja_exit" >> "$ROOT/ninja_out.txt"
if [ "$ninja_exit" -ne 0 ]; then
  echo "Build failed (exit $ninja_exit); see ninja_out.txt" >&2
  exit "$ninja_exit"
fi

./quantclaw_tests > "$ROOT/test_out.txt" 2>&1
test_exit=$?
echo "test exit: $test_exit" >> "$ROOT/test_out.txt"
exit "$test_exit"
