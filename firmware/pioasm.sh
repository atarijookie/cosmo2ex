#!/bin/sh
# Regenerates the committed *.pio.h of the Pico firmwares from their *.pio, with pioasm from the ce-pico-builder docker
# image (firmware/Dockerfile) - run it after changing a .pio file. The headers are committed for the Arduino IDE, which
# doesn't run pioasm; compile.sh regenerates them for its own build anyway, and warns when the committed ones are stale.
#
# usage: firmware/pioasm.sh [project ...]     project: ce_pico_hdd ce_pico_hdd_acsi ce_pico_hdd_scsi ce_pico_fdd (default: all of them)
#
# PICO_BUILDER_IMAGE env var - use this image instead of building ce-pico-builder locally, see build.sh

set -eu

FW_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$FW_DIR/.." && pwd)

PROJECTS="$*"
if [ -z "$PROJECTS" ]; then
    PROJECTS="ce_pico_hdd ce_pico_hdd_acsi ce_pico_hdd_scsi ce_pico_fdd"
fi

IMAGE=${PICO_BUILDER_IMAGE:-}
if [ -z "$IMAGE" ]; then
    IMAGE=ce-pico-builder
    echo "Building the $IMAGE image"
    docker build -t "$IMAGE" "$FW_DIR"
fi

DIRS=""
for project in $PROJECTS; do
    DIRS="$DIRS /src/$project"
done

# the repo is mounted writable here (unlike in build.sh), the container runs as the calling user so the files keep their owner
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$ROOT":/src \
    "$IMAGE" \
    /src/firmware/pio_gen.sh $DIRS

echo "Regenerated the .pio.h files in: $PROJECTS"
