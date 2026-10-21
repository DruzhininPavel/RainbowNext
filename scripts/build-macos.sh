#!/usr/bin/env bash
# Build myNext on macOS (Apple Silicon or Intel).
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

if ! brew list sdl3 >/dev/null 2>&1; then
  echo "SDL3 is not installed. Run: brew install sdl3 cmake" >&2
  exit 1
fi

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"

ctest --test-dir "$BUILD_DIR" --output-on-failure

echo
echo "Built $BUILD_DIR/mynext — run: $BUILD_DIR/mynext roms/48.rom"
