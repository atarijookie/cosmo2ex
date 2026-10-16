#!/bin/sh
# Builds the Pico firmwares (.uf2 to drag & drop onto the Pico in BOOTSEL mode, + .elf for debugging)
# into firmware/out/, using arduino-cli in the ce-pico-builder docker image (firmware/Dockerfile).
#
# usage: firmware/build.sh [project ...]     project: ce_pico_hdd_acsi ce_pico_hdd_scsi (default: all of them)
#
# PICO_BUILDER_IMAGE env var - use this (already pulled / pullable) image instead of building
# ce-pico-builder locally, e.g. ghcr.io/<owner>/ce-pico-builder:<tag> (this is what CI does)

set -eu

FW_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$FW_DIR/.." && pwd)
OUT="$FW_DIR/out"

PROJECTS="$*"
if [ -z "$PROJECTS" ]; then
    PROJECTS="ce_pico_hdd_acsi ce_pico_hdd_scsi"
fi

IMAGE=${PICO_BUILDER_IMAGE:-}
if [ -z "$IMAGE" ]; then
    IMAGE=ce-pico-builder
    echo "Building the $IMAGE image"
    docker build -t "$IMAGE" "$FW_DIR"
fi

mkdir -p "$OUT"

# the repo is mounted read-only, the container runs as the calling user so the results belong to them
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$ROOT":/src:ro \
    -v "$OUT":/out \
    "$IMAGE" \
    /src/firmware/compile.sh /src /out $PROJECTS

echo "Done. Firmwares are in $OUT"
