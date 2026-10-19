#!/bin/sh
# Builds the CosmosEx release tarballs, one per architecture, into release/out/.
#
# usage: release/build.sh [arch ...]     arch: amd64 arm64 armhf i386 (default: all of them)
#        release/build.sh --native       build only for this machine, with its own toolchains (no docker)
#
# Needs:
#   - m68k-atari-mint-gcc toolchain, for the Atari programs on the config drive (these are
#     the same for every architecture, so they are built once, on this machine)
#   - docker (not for --native) - ce_cores and webserver are built in the toolchain stage of docker/Dockerfile
#   - g++, make, go, libudev-dev, libevdev-dev (only for --native)
#
# VERSION env var overrides the version used in the tarball names (default: date + git hash).

set -eu

RELEASE_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$RELEASE_DIR/.." && pwd)
OUT="$RELEASE_DIR/out"
BUILDER_IMAGE=ce-release-builder

#-------------
# arguments
NATIVE=0
ARCHS=""
for arg in "$@"; do
    case "$arg" in
        --native)               NATIVE=1 ;;
        amd64|arm64|armhf|i386) ARCHS="$ARCHS $arg" ;;
        -h|--help)              sed -n '2,15p' "$0"; exit 0 ;;
        *)                      echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

if [ "$NATIVE" -eq 1 ]; then
    # name the native build by the architecture of this machine
    case "$(uname -m)" in
        x86_64)                 if [ "$(getconf LONG_BIT)" = 32 ]; then ARCHS=i386; else ARCHS=amd64; fi ;;
        i?86)                   ARCHS=i386 ;;
        aarch64|arm64)          if [ "$(getconf LONG_BIT)" = 32 ]; then ARCHS=armhf; else ARCHS=arm64; fi ;;
        armv6l|armv7l|armv8l)   ARCHS=armhf ;;
        *)                      ARCHS=$(uname -m) ;;
    esac
elif [ -z "$ARCHS" ]; then
    ARCHS="amd64 arm64 armhf i386"
fi

if [ -z "${VERSION:-}" ]; then
    VERSION="$(date +%Y%m%d)-$(git -C "$ROOT" rev-parse --short HEAD)"
    if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
        VERSION="$VERSION-dirty"
    fi
fi

echo "Building CosmosEx $VERSION for:$ARCHS"

#-------------
# check the tools first, so we don't fail in the middle
missing=""
command -v m68k-atari-mint-gcc >/dev/null || missing="$missing m68k-atari-mint-gcc"
if [ "$NATIVE" -eq 1 ]; then
    for tool in make g++ go; do
        command -v $tool >/dev/null || missing="$missing $tool"
    done
else
    command -v docker >/dev/null || missing="$missing docker"
fi
if [ -n "$missing" ]; then
    echo "missing tools:$missing" >&2
    exit 1
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

#-------------
# Atari programs for the config drive
echo "Building Atari programs"
make -C "$ROOT/libacsiscsi"
make -C "$ROOT/ce_dd_bootsectors" all       # builds also ce_dd_prg
make -C "$ROOT/test_ce"

#-------------
# files common to all architectures - this is what ends up in BIN_DIR (/opt/ce/bin)
COMMON="$STAGE/common"
mkdir -p "$COMMON"

cp "$ROOT/.env"                         "$COMMON/.env"
cp -r "$ROOT/webserver/static"          "$COMMON/static"
cp "$ROOT/ce_cores/ce_logo.bin"         "$COMMON/"
cp "$ROOT/ce_cores/core/ce_conf.st"     "$COMMON/"
cp "$ROOT/ce_cores/core/fdd_test.st"    "$COMMON/"
cp "$ROOT/ce_cores/core/ce_conf.zip"    "$COMMON/"
cp "$RELEASE_DIR/files/ce_run.sh"       "$COMMON/"
echo "$VERSION" >                       "$COMMON/VERSION"

# config drive - same layering as the docker image, then the freshly built Atari programs
cp -r "$ROOT/ce_cores/configdrive"      "$COMMON/configdrive"
cp -r "$ROOT/ce_cores/core/configdrive/." "$COMMON/configdrive/"
mkdir -p "$COMMON/configdrive/drivers" "$COMMON/configdrive/tests"
cp "$ROOT/ce_dd_bootsectors/ce_dd.bs"    "$COMMON/configdrive/drivers/"
cp "$ROOT/ce_dd_bootsectors/ce_dd_l2.bs" "$COMMON/configdrive/drivers/"
cp "$ROOT/ce_dd_prg/ce_dd.prg"           "$COMMON/configdrive/drivers/"
cp "$ROOT/test_ce/ce_tests.tos"          "$COMMON/configdrive/tests/"

chmod 755 "$COMMON/ce_run.sh"

#-------------
# Linux programs - per architecture
if [ "$NATIVE" -eq 0 ]; then
    echo "Building the builder image"
    docker build -t "$BUILDER_IMAGE" --target toolchain -f "$ROOT/docker/Dockerfile" "$ROOT"
fi

mkdir -p "$OUT"

for arch in $ARCHS; do
    BIN="$STAGE/$arch"
    mkdir -p "$BIN"

    if [ "$NATIVE" -eq 1 ]; then
        "$RELEASE_DIR/builder/build_linux.sh" native "$ROOT" "$BIN"
    else
        # the repo is mounted read-only, results are written to /out and then handed over to the calling user
        docker run --rm \
            -v "$ROOT":/src:ro \
            -v "$BIN":/out \
            -v ce-release-go:/go \
            "$BUILDER_IMAGE" \
            sh -c "/src/release/builder/build_linux.sh $arch /src /out && chown -R $(id -u):$(id -g) /out"
    fi

    #-------------
    # assemble the tarball:  ce-<version>-<arch>/{bin/, ce.service, VERSION, ARCH}
    NAME="ce-$VERSION-$arch"
    PKG="$STAGE/pkg/$NAME"
    mkdir -p "$PKG/bin"

    cp -a "$COMMON/." "$PKG/bin/"
    cp "$BIN/ce_cores.elf" "$BIN/webserver" "$PKG/bin/"
    cp "$RELEASE_DIR/files/ce.service" "$PKG/"
    echo "$VERSION" > "$PKG/VERSION"
    echo "$arch"    > "$PKG/ARCH"

    tar -C "$STAGE/pkg" --owner=0 --group=0 -czf "$OUT/$NAME.tar.gz" "$NAME"
    echo "Created $OUT/$NAME.tar.gz"
done

cp "$RELEASE_DIR/install.sh" "$OUT/install.sh"
chmod 755 "$OUT/install.sh"

(cd "$OUT" && sha256sum ce-*.tar.gz install.sh > SHA256SUMS)

echo "Done. Release files are in $OUT"
