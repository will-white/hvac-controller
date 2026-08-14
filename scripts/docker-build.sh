#!/usr/bin/env bash
# Build the firmware inside Docker — no host toolchain needed.
#
# Usage: scripts/docker-build.sh [pio run args...]
#   scripts/docker-build.sh                    build default env (hvac_controller)
#   scripts/docker-build.sh -e test_relays     build the relay hardware test
#   scripts/docker-build.sh -t clean           clean build artifacts
#
# Shares the "platformio-cache" named volume with the devcontainer, so
# platforms/toolchains download once and are reused by both.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=hvac-controller-build

docker build -q -t "$IMAGE" .devcontainer >/dev/null

exec docker run --rm \
  -v "$PWD":/workspace \
  -w /workspace \
  -v platformio-cache:/home/vscode/.platformio \
  "$IMAGE" \
  pio run "$@"
