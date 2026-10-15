#!/bin/sh
# Builds the Linux parts (ce_cores.elf + webserver) for one architecture.
# Called by release/build.sh - either inside the builder container, or directly on the host for --native builds.
#
# usage: build_linux.sh <arch> <repo dir> <output dir>
#   arch: amd64 | arm64 | armhf | i386 | native
#
# The sources are copied to a temp dir first, so the build never touches (or reuses)
# object files of the repo checkout, which might be for a different architecture.

set -eu

ARCH="$1"
SRC="$2"
OUT="$3"

# GO386=softfloat: Debian i386 is i686 without SSE2, the Go default (sse2) would not run on all of it
ARCH_FLAGS=
GO386=
case "$ARCH" in
    amd64)  CROSS=x86_64-linux-gnu-;     GOARCH=amd64; GOARM= ;;
    arm64)  CROSS=aarch64-linux-gnu-;    GOARCH=arm64; GOARM= ;;
    armhf)  CROSS=arm-linux-gnueabihf-;  GOARCH=arm;   GOARM=6 ;;
    i386)   CROSS=i686-linux-gnu-;       GOARCH=386;   GOARM=;  GO386=softfloat ;;
    native) CROSS=;                      GOARCH=$(go env GOARCH); GOARM=$(go env GOARM); GO386=$(go env GO386) ;;
    *)      echo "unknown architecture: $ARCH" >&2; exit 1 ;;
esac

# armhf is ARMv6+VFP (same as Raspbian), so it runs on Pi 1 / Zero too. The Debian cross compiler
# generates ARMv6 code with these flags, but its own runtime libs are ARMv7, so it links against the
# Raspbian sysroot of the toolchain image (docker/Dockerfile) instead. --sysroot alone is not enough, the Debian cross
# compiler still searches its own lib dirs first, hence -B (crt*.o, libgcc, libstdc++) and -L (glibc).
# The flags are part of CC / CXX, so they also reach the libdospath sub-make and the link step.
if [ "$ARCH" = armhf ]; then
    SYSROOT=/opt/rpi-sysroot
    MULTIARCH=arm-linux-gnueabihf
    ARCH_FLAGS="-march=armv6 -mfpu=vfp -mfloat-abi=hard -marm --sysroot=$SYSROOT \
        -B$SYSROOT/usr/lib/gcc/$MULTIARCH/10 -B$SYSROOT/usr/lib/$MULTIARCH \
        -L$SYSROOT/lib/$MULTIARCH -L$SYSROOT/usr/lib/$MULTIARCH"
fi

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
    CXX="${CROSS}g++ $ARCH_FLAGS" CC="${CROSS}gcc $ARCH_FLAGS" AR="${CROSS}ar" \
    LDFLAGS="-Llib -static-libstdc++ -static-libgcc"

"${CROSS}strip" -o "$OUT/ce_cores.elf" "$BUILD/ce_cores/ce_cores.elf"

#-------------
# webserver - pure Go, so it cross compiles without any C toolchain
echo "[$ARCH] building webserver"
cd "$BUILD/webserver"
CGO_ENABLED=0 GOOS=linux GOARCH="$GOARCH" GOARM="$GOARM" GO386="$GO386" \
    go build -trimpath -ldflags "-s -w" -o "$OUT/webserver" .

echo "[$ARCH] done"
