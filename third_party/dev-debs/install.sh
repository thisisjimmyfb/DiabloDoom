#!/bin/bash
# Install SDL2 and other dev headers from committed .debs.
# Run after a VM recycle to restore the build environment.
# Usage: ./third_party/dev-debs/install.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Installing dev headers from $SCRIPT_DIR..."

# Install all .debs; dpkg will handle ordering via dependencies.
# Use --force-depends if there are circular deps (rare).
sudo dpkg -i "$SCRIPT_DIR"/*.deb 2>&1 | tail -5

# Fix any broken dependencies
sudo apt-get install -f -y 2>&1 | tail -3

echo "Done. Re-run 'cmake .' in the build dir before building."
