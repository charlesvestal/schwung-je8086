#!/bin/bash
# Install JP-8000 module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/jp8000" ]; then
    echo "Error: dist/jp8000 not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing JP-8000 Module ==="

# Deploy to Move
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/jp8000/roms"
scp -r dist/jp8000/module.json dist/jp8000/dsp.so \
    ableton@move.local:/data/UserData/schwung/modules/sound_generators/jp8000/

# Set permissions
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/sound_generators/jp8000"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/sound_generators/jp8000/"
echo ""
echo "IMPORTANT: Place JP-8000 ROM .mid files in:"
echo "  /data/UserData/schwung/modules/sound_generators/jp8000/roms/"
echo ""
echo "Restart Schwung to load the new module."
