#!/usr/bin/env bash
# Run the native (host) tests in Docker: the firmware's control logic, the
# on-device unit suite and the scenario tests, compiled with g++ under ASan +
# UBSan (test/native/Makefile). No board needed; CI runs this before it
# publishes a build to the update channel.
#
# Usage: scripts/native-test.sh            build + run
#        scripts/native-test.sh clean      remove test/native/build
#
# Uses the same .devcontainer image as scripts/docker-build.sh. Set
# HVAC_BUILD_IMAGE to run in an existing image instead of building that one.
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=${HVAC_BUILD_IMAGE:-hvac-controller-build}
if [ -z "${HVAC_BUILD_IMAGE:-}" ]; then
  docker build -q -t "$IMAGE" .devcontainer >/dev/null
fi

TARGET=${1:-run}
exec docker run --rm \
  -v "$PWD":/workspace \
  -w /workspace/test/native \
  "$IMAGE" \
  make -j"$(nproc)" "$TARGET"
