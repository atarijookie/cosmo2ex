#!/bin/sh
# Builds the macOS programs as universal binaries (Apple Silicon arm64 + Intel x86_64): ce_cores, webserver and
# ce_run (the launcher, starts and stops ce_cores + webserver). Run on a Mac with the Xcode command line tools and Go.
#
# usage: build_macos.sh <repo dir> <output dir>
#
# Each program is built once per architecture, then lipo puts both into one file. ce_cores is built in a separate
# copy of the sources for each architecture (the object files differ), with -arch in CXX / CC, so it also reaches
# the libdospath sub-make and the link step - same trick as for armhf in build_linux.sh.
#
# Needs macOS 11 or newer - the first one for Apple Silicon, and the oldest one Go supports.

set -eu

SRC="$1"
OUT="$2"

case "$(uname -s)" in
    Darwin) ;;
    *)      echo "run this on macOS (needs Apple clang, the macOS SDK and lipo)" >&2; exit 1 ;;
esac

export MACOSX_DEPLOYMENT_TARGET=11.0

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

# clang -arch names - Go calls x86_64 amd64
ARCHS="arm64 x86_64"
NCPU=$(sysctl -n hw.ncpu)

#-------------
for arch in $ARCHS; do
    echo "[macos $arch] copying sources"
    mkdir -p "$BUILD/$arch"
    for d in libdospath ce_cores third_party; do
        cp -a "$SRC/$d" "$BUILD/$arch/"
    done

    # drop build outputs of the checkout
    find "$BUILD/$arch/libdospath" "$BUILD/$arch/ce_cores" \( -name '*.o' -o -name '*.a' -o -name '*.d' -o -name '*.elf' \) -delete
    rm -rf "$BUILD/$arch/ce_cores/obj" "$BUILD/$arch/ce_cores/lib" "$BUILD/$arch/ce_cores/ce_cores"
    mkdir -p "$BUILD/$arch/ce_cores/obj" "$BUILD/$arch/ce_cores/lib"

    echo "[macos $arch] building ce_cores"
    make -C "$BUILD/$arch/ce_cores" -j"$NCPU" UNAME_S=Darwin \
        CXX="clang++ -arch $arch" CC="clang -arch $arch"
done

echo "[macos] ce_cores -> universal binary"
lipo -create -output "$OUT/ce_cores" "$BUILD/arm64/ce_cores/ce_cores" "$BUILD/x86_64/ce_cores/ce_cores"
strip "$OUT/ce_cores"

#-------------
# pure Go, no cgo - both architectures cross compile on either Mac
cp -a "$SRC/webserver" "$BUILD/webserver"

for prog in webserver ce_run; do
    pkg=.
    if [ "$prog" = ce_run ]; then
        pkg=./cmd/ce_run
    fi

    for goarch in arm64 amd64; do
        echo "[macos $goarch] building $prog"
        (cd "$BUILD/webserver" && CGO_ENABLED=0 GOOS=darwin GOARCH=$goarch \
            go build -trimpath -ldflags "-s -w" -o "$BUILD/$prog-$goarch" "$pkg")
    done

    echo "[macos] $prog -> universal binary"
    lipo -create -output "$OUT/$prog" "$BUILD/$prog-arm64" "$BUILD/$prog-amd64"
done

#-------------
# Apple Silicon runs only signed code - the linkers sign ad-hoc, but strip changed ce_cores after that, so sign all
# of them again (ad-hoc: no certificate, so Gatekeeper still warns about downloaded files - that needs a Developer ID)
for prog in ce_cores webserver ce_run; do
    codesign --force --sign - "$OUT/$prog"
    codesign --verify "$OUT/$prog"
done

# every binary must have both architectures
for prog in ce_cores webserver ce_run; do
    for arch in $ARCHS; do
        if ! lipo "$OUT/$prog" -verify_arch "$arch"; then
            echo "$prog has no $arch code" >&2
            exit 1
        fi
    done
    lipo -info "$OUT/$prog"
done

echo "[macos] done"
