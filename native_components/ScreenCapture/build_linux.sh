#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
IMAGE=screen-capture-builder
docker build --platform linux/amd64 -f "$SCRIPT_DIR/Dockerfile.linux" -t "$IMAGE" "$SCRIPT_DIR"
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -v "$SCRIPT_DIR:/src" "$IMAGE" bash -c 'tr -d "\r" < /src/build_linux_inner.sh | bash'
echo "Done: $SCRIPT_DIR/build_linux/ScreenCapture.so"
