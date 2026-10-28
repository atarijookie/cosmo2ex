#!/bin/sh
# Builds the Windows programs on Linux from the current sources (uncommitted changes included), for testing on
# Windows during development - the output dir is a ready to run folder: copy it to Windows and start ce_run.exe.
#
# usage: release/builder/build_windows_cross.sh [--clean] [output dir]     (default: release/out/windows-dev)
#   --clean     throw away the build cache first (full rebuild)
#
# ce_cores.exe is built with the Cygwin cross compiler, webserver.exe + ce_run.exe with Go, all in the
# ce-windows-cross docker image (windows_cross.Dockerfile). The sources are copied (rsync) into a docker volume and
# built there, so the next build compiles only what changed, and the Linux build outputs in this checkout (obj/, lib/,
# *.o, *.d, *.a) are neither used nor touched.
#
# The output has ce_cores.exe with debug info (not stripped, for gdb) + the Cygwin DLLs of the cross compiler, and the
# other files of the Windows zip, except the licenses. The Atari programs on the config drive (ce_dd.bs, ce_dd.prg, ...)
# need the m68k-atari-mint toolchain - they are taken from this checkout if they were built here, otherwise
# the output is missing them (you'll get a warning).
#
# .env in the output dir is created from release/files/windows.env only when it's not there yet, so your changes
# to it (e.g. LOG_LEVEL) are kept between builds.
#
# Releases are still built by build_windows.sh in a real Cygwin (CI), this is only for development.

set -eu

BUILDER_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$BUILDER_DIR/../.." && pwd)
IMAGE=ce-windows-cross
BUILD_VOLUME=ce-windows-cross-build
GO_VOLUME=ce-windows-cross-go

CLEAN=0
OUT=""
for arg in "$@"; do
    case "$arg" in
        --clean)    CLEAN=1 ;;
        -h|--help)  sed -n '2,22p' "$0"; exit 0 ;;
        -*)         echo "unknown option: $arg" >&2; exit 1 ;;
        *)          OUT="$arg" ;;
    esac
done
OUT=${OUT:-$ROOT/release/out/windows-dev}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

VERSION="dev-$(git -C "$ROOT" rev-parse --short HEAD)"
if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then
    VERSION="$VERSION-dirty"
fi

echo "Building the $IMAGE image"
docker build -q -t "$IMAGE" -f "$BUILDER_DIR/windows_cross.Dockerfile" "$BUILDER_DIR" >/dev/null

if [ "$CLEAN" -eq 1 ]; then
    echo "Removing the build cache"
    docker volume rm -f "$BUILD_VOLUME" >/dev/null
fi

# the repo is mounted read-only; the container runs as root (the volumes are root's), so the results are handed over
# to the calling user at the end
docker run --rm \
    -v "$ROOT":/src:ro \
    -v "$BUILD_VOLUME":/build \
    -v "$GO_VOLUME":/root/go \
    -v "$OUT":/out \
    -e VERSION="$VERSION" \
    -e OUT_UID="$(id -u)" -e OUT_GID="$(id -g)" \
    "$IMAGE" \
    /src/release/builder/compile_windows_cross.sh /src /build /out

echo "Done ($VERSION). Copy $OUT to Windows and start ce_run.exe"
