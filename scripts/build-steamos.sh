#!/usr/bin/env bash
# Build RainbowNext on SteamOS / Arch / any Linux with SDL3 available.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

missing=()
command -v cmake >/dev/null 2>&1 || missing+=("cmake")
command -v c++   >/dev/null 2>&1 || missing+=("gcc")
pkg-config --exists sdl3 2>/dev/null || missing+=("sdl3")

if [ ${#missing[@]} -gt 0 ]; then
  echo "Missing build dependencies: ${missing[*]}" >&2
  echo "On SteamOS the read-only root needs: sudo steamos-readonly disable" >&2
  echo "then: sudo pacman -S --needed base-devel cmake sdl3" >&2
  exit 1
fi

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$(nproc)"

ctest --test-dir "$BUILD_DIR" --output-on-failure

echo
echo "Built $BUILD_DIR/rainbow — run: $BUILD_DIR/rainbow roms/48.rom"
