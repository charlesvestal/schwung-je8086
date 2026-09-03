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

DEST=/data/UserData/schwung/modules/sound_generators/jp8000

# Deploy to Move.
#
# dsp.so goes over as a NEW FILE and is then renamed into place. Writing
# straight onto it corrupts the mapping of a copy that is already loaded --
# the process keeps the inode, and scp truncates and rewrites that same
# inode under it. A rename swaps the directory entry instead, so anything
# still running holds the old inode until it lets go.
#
# help.json ships too. It did not, which is how the module's help went stale
# on the device while being correct in the repo.
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p $DEST/roms $DEST/banks"
scp dist/jp8000/dsp.so ableton@move.local:$DEST/dsp.so.new
scp dist/jp8000/module.json dist/jp8000/help.json ableton@move.local:$DEST/
ssh ableton@move.local "mv -f $DEST/dsp.so.new $DEST/dsp.so"

# Set permissions
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw $DEST"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: $DEST/"
echo ""
echo "IMPORTANT: Place JP-8000 ROM .mid files in:"
echo "  $DEST/roms/"
echo ""
echo "Restart Schwung to load the new module."
