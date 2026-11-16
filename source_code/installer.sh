#!/bin/sh
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
BOLT="$DIR/compiler/bolt"

if [ ! -f "$BOLT" ]; then
    echo "Error: bolt script not found at $BOLT"
    exit 1
fi

if command -v bolt >/dev/null 2>&1 || [ -x /usr/local/bin/bolt ] || [ -f /usr/local/bin/bolt ]; then
    echo "bolt is already installed; reinstalling..."
    exec bash "$BOLT" --reinstall
else
    echo "bolt is not installed; installing..."
    exec bash "$BOLT" --install
fi
