#!/bin/sh
# Builds the CosmosEx release tarballs, one per architecture, into release/out/.
#
# usage: release/build.sh [arch ...]     arch: amd64 arm64 armhf i386 (default: all of them), windows, macos
#        release/build.sh --native       build only for this machine, with its own toolchains (no docker)
#
# windows: makes the Windows zip from the programs built by builder/build_windows.sh in Cygwin - their dir is given
# in the WINDOWS_BIN env var, e.g. WINDOWS_BIN=/tmp/windows-bin release/build.sh windows
# macos: the same for the macOS zip, from the output of builder/build_macos.sh (run on a Mac) in MACOS_BIN
#
# Needs:
#   - m68k-atari-mint-gcc toolchain, for the Atari programs on the config drive (these are
#     the same for every architecture, so they are built once, on this machine)
#   - docker (not for --native) - ce_cores and webserver are built in the toolchain stage of docker/Dockerfile
#   - g++, make, go, libudev-dev, libevdev-dev (only for --native)
#   - python3 (only for windows / macos - makes the zip)
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
WINDOWS=0
MACOS=0
ARCHS=""
for arg in "$@"; do
    case "$arg" in
        --native)               NATIVE=1 ;;
        amd64|arm64|armhf|i386) ARCHS="$ARCHS $arg" ;;
        windows)                WINDOWS=1 ;;
        macos)                  MACOS=1 ;;
        -h|--help)              sed -n '2,20p' "$0"; exit 0 ;;
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
elif [ -z "$ARCHS" ] && [ "$WINDOWS" -eq 0 ] && [ "$MACOS" -eq 0 ]; then
    ARCHS="amd64 arm64 armhf i386"
fi

if [ -z "${VERSION:-}" ]; then
    VERSION="$(date +%Y%m%d)-$(git -C "$ROOT" rev-parse --short HEAD)"
    if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
        VERSION="$VERSION-dirty"
    fi
fi

echo "Building CosmosEx $VERSION for:$ARCHS$( [ "$WINDOWS" -eq 1 ] && echo " windows" )$( [ "$MACOS" -eq 1 ] && echo " macos" )"

#-------------
# check the tools first, so we don't fail in the middle
missing=""
command -v m68k-atari-mint-gcc >/dev/null || missing="$missing m68k-atari-mint-gcc"
if [ "$NATIVE" -eq 1 ]; then
    for tool in make g++ go; do
        command -v $tool >/dev/null || missing="$missing $tool"
    done
elif [ -n "$ARCHS" ]; then
    command -v docker >/dev/null || missing="$missing docker"
fi
if [ "$WINDOWS" -eq 1 ] || [ "$MACOS" -eq 1 ]; then
    command -v python3 >/dev/null || missing="$missing python3"
fi
if [ -n "$missing" ]; then
    echo "missing tools:$missing" >&2
    exit 1
fi

if [ "$WINDOWS" -eq 1 ]; then
    for f in ce_cores.exe cygwin1.dll webserver.exe ce_run.exe licenses/CYGWIN-NOTICE.txt; do
        if [ ! -f "${WINDOWS_BIN:-}/$f" ]; then
            echo "windows: WINDOWS_BIN must be the output dir of builder/build_windows.sh, '${WINDOWS_BIN:-}/$f' not found" >&2
            exit 1
        fi
    done
fi

if [ "$MACOS" -eq 1 ]; then
    for f in ce_cores webserver ce_run; do
        if [ ! -f "${MACOS_BIN:-}/$f" ]; then
            echo "macos: MACOS_BIN must be the output dir of builder/build_macos.sh, '${MACOS_BIN:-}/$f' not found" >&2
            exit 1
        fi
    done
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
if [ "$NATIVE" -eq 0 ] && [ -n "$ARCHS" ]; then
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
    cp "$ROOT/LICENSE" "$PKG/"
    echo "$VERSION" > "$PKG/VERSION"
    echo "$arch"    > "$PKG/ARCH"

    tar -C "$STAGE/pkg" --owner=0 --group=0 -czf "$OUT/$NAME.tar.gz" "$NAME"
    echo "Created $OUT/$NAME.tar.gz"
done

#-------------
# Windows zip:  ce-<version>-windows-x64/ - one folder with everything, unzip anywhere and run ce_run.exe
if [ "$WINDOWS" -eq 1 ]; then
    NAME="ce-$VERSION-windows-x64"
    PKG="$STAGE/pkg/$NAME"
    mkdir -p "$PKG"

    cp -a "$COMMON/." "$PKG/"
    rm "$PKG/ce_run.sh"                                         # ce_run.exe instead
    cp "$RELEASE_DIR/files/windows.env" "$PKG/.env"
    cp "$RELEASE_DIR/files/windows-README.txt" "$PKG/README.txt"
    cp -r "$WINDOWS_BIN/." "$PKG/"

    rm -f "$OUT/$NAME.zip"
    (cd "$STAGE/pkg" && python3 -m zipfile -c "$OUT/$NAME.zip" "$NAME")
    echo "Created $OUT/$NAME.zip"
fi

#-------------
# macOS zip:  ce-<version>-macos-universal/ - one folder with everything, unzip anywhere and double-click
# CosmosEx.command (opens Terminal with ce_run in it). Universal binaries - Apple Silicon and Intel.
if [ "$MACOS" -eq 1 ]; then
    NAME="ce-$VERSION-macos-universal"
    PKG="$STAGE/pkg/$NAME"
    mkdir -p "$PKG/licenses"

    cp -a "$COMMON/." "$PKG/"
    rm "$PKG/ce_run.sh"                                         # ce_run instead
    cp "$RELEASE_DIR/files/macos.env" "$PKG/.env"
    cp "$RELEASE_DIR/files/macos-README.txt" "$PKG/README.txt"
    cp "$RELEASE_DIR/files/CosmosEx.command" "$PKG/"
    cp "$ROOT/LICENSE" "$PKG/licenses/Cosmo2ex-license.txt"
    for f in ce_cores webserver ce_run; do
        cp "$MACOS_BIN/$f" "$PKG/"
    done
    chmod 755 "$PKG/ce_cores" "$PKG/webserver" "$PKG/ce_run" "$PKG/CosmosEx.command"    # lost in CI artifacts

    # python's zipfile keeps the permissions (Archive Utility restores them), so the programs stay executable
    rm -f "$OUT/$NAME.zip"
    (cd "$STAGE/pkg" && python3 -m zipfile -c "$OUT/$NAME.zip" "$NAME")
    echo "Created $OUT/$NAME.zip"
fi

cp "$RELEASE_DIR/install.sh" "$OUT/install.sh"
chmod 755 "$OUT/install.sh"

(cd "$OUT" && sha256sum $(ls ce-*.tar.gz ce-*.zip 2>/dev/null) install.sh > SHA256SUMS)

echo "Done. Release files are in $OUT"
