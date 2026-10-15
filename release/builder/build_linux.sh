#!/bin/sh
# Builds the Linux parts (ce_cores.elf + webserver) for one architecture.
# Called by release/build.sh - either inside the builder container, or directly on the host for --native builds.
#
# usage: build_linux.sh <arch> <repo dir> <output dir>
#   arch: amd64 | arm64 | armhf | native
#
# The sources are copied to a temp dir first, so the build never touches (or reuses)
# object files of the repo checkout, which might be for a different architecture.

set -eu

ARCH="$1"
SRC="$2"
OUT="$3"

case "$ARCH" in
    amd64)  CROSS=x86_64-linux-gnu-;     GOARCH=amd64; GOARM= ;;
    arm64)  CROSS=aarch64-linux-gnu-;    GOARCH=arm64; GOARM= ;;
    armhf)  CROSS=arm-linux-gnueabihf-;  GOARCH=arm;   GOARM=7 ;;
    native) CROSS=;                      GOARCH=$(go env GOARCH); GOARM=$(go env GOARM) ;;
    *)      echo "unknown architecture: $ARCH" >&2; exit 1 ;;
esac

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

echo "[$ARCH] copying sources"
for d in libdospath ce_cores webserver third_party; do
    cp -a "$SRC/$d" "$BUILD/"
done

# drop build outputs of the checkout
find "$BUILD/libdospath" "$BUILD/ce_cores" \( -name '*.o' -o -name '*.a' -o -name '*.d' -o -name '*.elf' \) -delete
rm -rf "$BUILD/ce_cores/obj" "$BUILD/ce_cores/lib"
mkdir -p "$BUILD/ce_cores/obj" "$BUILD/ce_cores/lib"

#-------------
# ce_cores (builds libdospath on its own)
# libstdc++ and libgcc are linked statically, so the binary only depends on glibc
echo "[$ARCH] building ce_cores"
make -C "$BUILD/ce_cores" -j"$(nproc)" \
    CXX="${CROSS}g++" CC="${CROSS}gcc" AR="${CROSS}ar" \
    LDFLAGS="-Llib -static-libstdc++ -static-libgcc"

"${CROSS}strip" -o "$OUT/ce_cores.elf" "$BUILD/ce_cores/ce_cores.elf"

#-------------
# webserver - pure Go, so it cross compiles without any C toolchain
echo "[$ARCH] building webserver"
cd "$BUILD/webserver"
CGO_ENABLED=0 GOOS=linux GOARCH="$GOARCH" GOARM="$GOARM" \
    go build -trimpath -ldflags "-s -w" -o "$OUT/webserver" .

echo "[$ARCH] done"
