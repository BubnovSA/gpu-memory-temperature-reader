#!/bin/sh
# Build libgddr6 + gddr6 and install both into /usr/local.
set -eu

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SELF_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"

echo "Configuring (cmake) in $BUILD_DIR"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR"

echo "Building"
cmake --build "$BUILD_DIR" --config Release

echo "Installing libgddr6.a + gddr6 to /usr/local"
sudo cmake --install "$BUILD_DIR" --prefix /usr/local

cat <<EOF

Installed:
  /usr/local/bin/gddr6
  /usr/local/lib/libgddr6.a

To run:
  sudo gddr6

To uninstall:
  sudo rm /usr/local/bin/gddr6 /usr/local/lib/libgddr6.a
EOF
