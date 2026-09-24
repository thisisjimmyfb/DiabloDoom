#!/usr/bin/env bash
#
# Build DiabloDoom (Chocolate Doom fork) with CMake.
#
# Usage: scripts/build.sh [--deps] [--clean] [--debug]
#   --deps   install build dependencies via apt (needs sudo)
#   --clean  remove the build directory before configuring
#   --debug  build with debug symbols instead of Release
#

set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=build
BUILD_TYPE=Release
INSTALL_DEPS=0
CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --deps)  INSTALL_DEPS=1 ;;
        --clean) CLEAN=1 ;;
        --debug) BUILD_TYPE=Debug ;;
        -h|--help)
            sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

DEPS=(cmake make gcc pkg-config libsdl2-dev libsdl2-mixer-dev
      libsdl2-net-dev libpng-dev libsamplerate0-dev)

if [ "$INSTALL_DEPS" -eq 1 ]; then
    sudo apt-get update
    sudo apt-get install -y "${DEPS[@]}"
fi

# Check that the required tools and SDL2 headers are present.
missing=()
for tool in cmake make pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if command -v pkg-config >/dev/null 2>&1; then
    for lib in sdl2 SDL2_mixer SDL2_net; do
        pkg-config --exists "$lib" || missing+=("$lib (dev headers)")
    done
fi
if [ "${#missing[@]}" -gt 0 ]; then
    echo "Missing dependencies: ${missing[*]}" >&2
    echo "Run: scripts/build.sh --deps   (or: sudo apt-get install ${DEPS[*]})" >&2
    exit 1
fi

if [ "$CLEAN" -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo
echo "Build complete: $BUILD_DIR/src/chocolate-doom"
echo "Run with:       $BUILD_DIR/src/chocolate-doom -iwad /path/to/doom.wad"
