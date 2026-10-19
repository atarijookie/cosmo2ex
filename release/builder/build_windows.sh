#!/bin/sh
# Builds the Windows programs (ce_cores.exe, webserver.exe, ce_run.exe) - run in a Cygwin shell (packages: gcc-g++,
# make, curl) with Go for Windows in PATH. release/build.sh makes the release zip from the output, together with the
# Atari programs and the other files (those need the m68k-atari-mint toolchain, which is not available in Cygwin).
#
# usage: build_windows.sh <repo dir> <output dir>
#
# ce_cores.exe is a Cygwin program, so it needs cygwin1.dll next to it. libstdc++ and libgcc are linked in statically
# (see ce_cores/makefile), so cygstdc++-6.dll and cyggcc_s-seh-1.dll are not needed - if that ever changes, they get
# bundled too, the DLLs are taken from what cygcheck finds.
#
# cygwin1.dll is under LGPLv3, which allows distributing it with a program under any license, if the package has:
#   - a notice that the program uses the Cygwin library under LGPLv3, with the LGPLv3 and GPLv3 texts
#   - the source code of exactly the bundled cygwin1.dll (here: its source package, downloaded from a Cygwin mirror)
#   - cygwin1.dll as a separate, replaceable DLL (not linked in statically)
# All of that goes to <output dir>/licenses.
#
# The webserver and ce_run (the launcher, starts and stops ce_cores + webserver) are native Windows programs
# (pure Go, no Cygwin).

set -eu

SRC="$1"
OUT="$2"

case "$(uname -s)" in
    CYGWIN*) ;;
    *)  echo "run this in a Cygwin shell" >&2; exit 1 ;;
esac

CYGWIN_MIRROR="${CYGWIN_MIRROR:-https://mirrors.kernel.org/sourceware/cygwin}"

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

echo "[windows] copying sources"
for d in libdospath ce_cores webserver third_party; do
    cp -a "$SRC/$d" "$BUILD/"
done

# drop build outputs of the checkout
find "$BUILD/libdospath" "$BUILD/ce_cores" \( -name '*.o' -o -name '*.a' -o -name '*.d' -o -name '*.elf' -o -name '*.exe' \) -delete
rm -rf "$BUILD/ce_cores/obj" "$BUILD/ce_cores/lib"
mkdir -p "$BUILD/ce_cores/obj" "$BUILD/ce_cores/lib"

#-------------
echo "[windows] building ce_cores"
make -C "$BUILD/ce_cores" -j"$(nproc)"
strip -o "$OUT/ce_cores.exe" "$BUILD/ce_cores/ce_cores.exe"

# Cygwin DLLs it needs - cygcheck lists the whole chain as Windows paths; Windows' own DLLs are not ours to bundle
cygcheck "$OUT/ce_cores.exe" | tr -d '\r' | sed 's/^ *//' | grep -i '\\bin\\cyg[^\\]*\.dll$' | sort -u |
while read -r dll; do
    cp "$(cygpath -u "$dll")" "$OUT/"
    echo "[windows] bundled $(basename "$(cygpath -u "$dll")")"
done

if [ ! -f "$OUT/cygwin1.dll" ]; then
    echo "cygwin1.dll not found by cygcheck - is this a Cygwin build?" >&2
    exit 1
fi

#-------------
echo "[windows] building webserver + ce_run"
(cd "$BUILD/webserver" && CGO_ENABLED=0 GOOS=windows GOARCH=amd64 \
    go build -trimpath -ldflags "-s -w" -o "$(cygpath -w "$OUT/webserver.exe")" . &&
    CGO_ENABLED=0 GOOS=windows GOARCH=amd64 \
    go build -trimpath -ldflags "-s -w" -o "$(cygpath -w "$OUT/ce_run.exe")" ./cmd/ce_run)

#-------------
echo "[windows] license files"
mkdir -p "$OUT/licenses"
cp "$SRC/release/files/licenses/LGPL-3.0.txt" "$SRC/release/files/licenses/GPL-3.0.txt" "$OUT/licenses/"
cp "$SRC/ce_cores/license.txt" "$OUT/licenses/CosmosEx-license.txt"

CYGWIN_VERSION=$(cygcheck -c -d cygwin | awk '$1 == "cygwin" { print $2 }')     # e.g. 3.5.4-1
if [ -z "$CYGWIN_VERSION" ]; then
    echo "cannot find out the version of the cygwin package" >&2
    exit 1
fi

# source package of exactly this cygwin1.dll, checked against the SHA-512 in the mirror's package list (setup.ini)
CYGWIN_SRC="cygwin-$CYGWIN_VERSION-src.tar.xz"
curl -fsSL -o "$OUT/licenses/$CYGWIN_SRC" "$CYGWIN_MIRROR/src/release/cygwin/$CYGWIN_SRC"

CYGWIN_SRC_SHA512=$(curl -fsSL "$CYGWIN_MIRROR/x86_64/setup.ini" | awk -v f="src/release/cygwin/$CYGWIN_SRC" '$1 == "source:" && $2 == f { print $4 }')
if [ -z "$CYGWIN_SRC_SHA512" ] || ! echo "$CYGWIN_SRC_SHA512  $OUT/licenses/$CYGWIN_SRC" | sha512sum -c - >/dev/null; then
    echo "$CYGWIN_SRC doesn't match the checksum in setup.ini (or it's not listed there)" >&2
    exit 1
fi

cat > "$OUT/licenses/CYGWIN-NOTICE.txt" <<EOF
ce_cores.exe uses the Cygwin library (cygwin1.dll), version $CYGWIN_VERSION, https://cygwin.com

The Cygwin library is free software, licensed under the GNU Lesser General Public License version 3
(LGPL-3.0.txt), which is an addition to the GNU General Public License version 3 (GPL-3.0.txt).
Its complete source code is in $CYGWIN_SRC next to this file.

cygwin1.dll is a separate DLL - you may replace it with your own, modified version of the same Cygwin version.

libstdc++ and libgcc are linked into ce_cores.exe under the GCC Runtime Library Exception.
EOF

echo "[windows] done"
