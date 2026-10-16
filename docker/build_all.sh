#!/bin/sh
# Builds the cosmo2ex image for several platforms on this machine, and saves one image tarball
# per platform into docker/out/ - copy it to the target and load it there with:
#   docker load -i cosmo2ex-<platform>.tar.gz
#
# usage: docker/build_all.sh [platform ...]
#   platform: linux/amd64 linux/arm64 linux/arm/v7 linux/arm/v6 linux/386 (default: all of them)
#     linux/arm64   - Pi 3 / 4 / 5 / Zero 2 W with 64-bit OS
#     linux/arm/v7  - Pi 2 / 3 / 4 / 5 with 32-bit OS
#     linux/arm/v6  - Pi 1 / Zero / Zero W (minimal Raspbian userland)
#
# Nothing runs emulated: the programs are cross compiled on this machine, the platform images only
# get files copied in (see docker/Dockerfile). Needs docker with the containerd image store (the
# default since Docker 29), which keeps all the platforms under the one cosmo2ex tag.
#
# Needs m68k-atari-mint-gcc for the config drive drivers, unless they are already in
# ce_cores/configdrive/drivers/ (see docker/README.md).

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/docker/out"
IMAGE=cosmo2ex

PLATFORMS="$*"
if [ -z "$PLATFORMS" ]; then
    PLATFORMS="linux/amd64 linux/arm64 linux/arm/v7 linux/arm/v6 linux/386"
fi

# config drive drivers - build them fresh if the Atari toolchain is here
if command -v m68k-atari-mint-gcc >/dev/null; then
    "$ROOT/docker/build_drivers.sh"
else
    echo "m68k-atari-mint-gcc not found, using the drivers already in ce_cores/configdrive/drivers/"
fi

echo "Building $IMAGE for: $PLATFORMS"
docker build --platform "$(echo $PLATFORMS | tr ' ' ',')" -f "$ROOT/docker/Dockerfile" -t "$IMAGE" "$ROOT"

mkdir -p "$OUT"
for p in $PLATFORMS; do
    f="$OUT/$IMAGE-$(echo "${p#linux/}" | tr '/' '-').tar.gz"
    docker save --platform "$p" "$IMAGE" | gzip > "$f"
    echo "Saved $f"
done

echo "Done. Image tarballs are in $OUT"
