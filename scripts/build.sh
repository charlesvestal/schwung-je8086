#!/usr/bin/env bash
# Build JP-8000 module for Schwung (ARM64)
#
# Uses CMake to build the gearmulator JE-8086 library and plugin wrapper.
# Automatically uses Docker for cross-compilation if needed.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-jp8000-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== JP-8000 Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # The Remote UI's parameter table is generated from the header the plugin
    # is built from. Done HERE, on the host, because the build image has no
    # python3; the container step below only copies the result. CI then fails
    # if the committed copy differs from what this tree generates.
    if command -v python3 >/dev/null 2>&1; then
        python3 "$REPO_ROOT/src/tools/gen_remote_params.py"
    else
        echo "python3 not found: shipping the committed src/remote/assets/params.js"
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
cd "$REPO_ROOT"

echo "=== Building JP-8000 Module ==="

# Create build directory
mkdir -p build

# Run CMake configure with cross-compilation toolchain
echo "Configuring CMake..."
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G Ninja \
    2>&1

# Build plugin
echo "Building plugin..."
cmake --build build --target jp8000-move-plugin -j$(nproc) 2>&1

# Package
echo "Packaging..."
mkdir -p dist/jp8000

# Copy files to dist
cat src/module.json > dist/jp8000/module.json
cat src/help.json > dist/jp8000/help.json
cat build/dsp.so > dist/jp8000/dsp.so
chmod +x dist/jp8000/dsp.so

# The Remote UI: web_ui.html beside module.json is what Schwung Manager looks
# for, and assets/ is served under it (params.js was regenerated on the host
# above; this side of the script has no python3).
rm -rf dist/jp8000/assets
cp src/remote/web_ui.html dist/jp8000/web_ui.html
cp -R src/remote/assets dist/jp8000/assets

# Asset directory placeholders (ROMs required, extra banks optional)
mkdir -p dist/jp8000/roms dist/jp8000/banks

# Create tarball for release
cd dist
tar -czvf jp8000-module.tar.gz jp8000/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/jp8000/"
echo "Tarball: dist/jp8000-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
echo ""
echo "IMPORTANT: Place JP-8000 ROM .mid files in the roms/ directory on device:"
echo "  /data/UserData/schwung/modules/sound_generators/jp8000/roms/"
