#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

# If not in Docker and no CROSS_PREFIX, run via Docker
if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
    echo "==> Building in Docker..."
    docker build --platform linux/amd64 -t move-stretch-builder -f scripts/Dockerfile scripts/
    docker run --platform linux/amd64 --rm -v "$REPO_ROOT:/build" -w /build move-stretch-builder ./scripts/build.sh
    echo "==> Build complete."
    ls -lh dist/stretch/dsp.so dist/stretch-module.tar.gz 2>/dev/null
    exit 0
fi

echo "==> Cross-compiling for aarch64..."

# Step 1: Clone and build Bungee as static library (if not cached)
BUNGEE_DIR="src/dsp/bungee"
if [ ! -d "$BUNGEE_DIR" ]; then
    echo "    Cloning Bungee..."
    git clone --recurse-submodules --depth 1 \
        https://github.com/bungee-audio-stretch/bungee.git "$BUNGEE_DIR"
fi

BUNGEE_BUILD="build/bungee"
if [ ! -f "$BUNGEE_BUILD/libbungee.a" ]; then
    echo "    Building Bungee static library..."
    mkdir -p "$BUNGEE_BUILD"
    cmake \
        --preset linux-aarch64 \
        -S "$BUNGEE_DIR" \
        -B "$BUNGEE_BUILD" \
        -DBUNGEE_BUILD_SHARED_LIBRARY=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    cmake --build "$BUNGEE_BUILD"
fi

# Locate the built static libraries (use find for robustness)
BUNGEE_LIB=$(find "$BUNGEE_BUILD" -name "libbungee.a" | head -1)
PFFFT_LIB=$(find "$BUNGEE_BUILD" -name "libpffft.a" | head -1)

if [ -z "$BUNGEE_LIB" ] || [ -z "$PFFFT_LIB" ]; then
    echo "ERROR: Could not find libbungee.a or libpffft.a in $BUNGEE_BUILD"
    exit 1
fi

echo "    Bungee lib: $BUNGEE_LIB"
echo "    PFFFT lib: $PFFFT_LIB"

# Step 2: Compile the DSP plugin
echo "    Compiling stretch_plugin.cpp..."
mkdir -p build

${CXX:-aarch64-linux-gnu-g++} -std=c++20 -O2 -shared -fPIC \
    -I"$BUNGEE_DIR" \
    -I"$BUNGEE_DIR/Eigen" \
    src/dsp/stretch_plugin.cpp \
    "$BUNGEE_LIB" \
    "$PFFFT_LIB" \
    -o build/dsp.so \
    -lm \
    -static-libstdc++ -static-libgcc

${CROSS_PREFIX:-}strip build/dsp.so

echo "    dsp.so size: $(du -h build/dsp.so | cut -f1)"

# Step 3: Package
echo "    Packaging..."
mkdir -p dist/stretch
cp src/module.json dist/stretch/
cp src/ui.js       dist/stretch/
cp src/help.json   dist/stretch/
cp build/dsp.so    dist/stretch/
chmod +x dist/stretch/dsp.so

cd dist
tar -czvf stretch-module.tar.gz stretch/
cd ..

echo "==> Done: dist/stretch-module.tar.gz"
