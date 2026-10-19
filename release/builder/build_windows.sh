#!/bin/sh
# Builds the Windows programs (ce_cores.exe, webserver.exe, ce_run.exe) - run in a Cygwin shell (packages: gcc-g++,
# make, curl) with Go for Windows in PATH. release/build.sh makes the release zip from the output, together with the
# Atari programs and the other files (those need the m68k-atari-mint toolchain, which is not available in Cygwin).
#
# usage: build_windows.sh <repo dir> <output dir>
#
# ce_cores.exe is a Cygwin program, so it needs cygwin1.dll next to it, and the GCC runtime DLLs cygstdc++-6.dll and
# cyggcc_s-seh-1.dll (they can't be linked in statically on Cygwin, see ce_cores/makefile). The DLLs are taken from
# what cygcheck finds.
#
# The GCC runtime DLLs are under GPLv3 with the GCC Runtime Library Exception, which allows distributing them with
# a program compiled by GCC, under any license - the notice says that.
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

# mirrors tried in this order (a mirror can be down or not have the newest package yet), CYGWIN_MIRROR=<url> uses only that one
CYGWIN_MIRRORS="${CYGWIN_MIRROR:-https://mirrors.kernel.org/sourceware/cygwin https://cygwin.mirror.constant.com https://mirrors.dotsrc.org/cygwin https://ftp.fau.de/cygwin}"

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

echo "[windows] copying sources"
for d in libdospath ce_cores webserver third_party; do
    cp -R "$SRC/$d" "$BUILD/"       # not cp -a: setting the checkout's Windows ACLs / owner on the copy fails in Cygwin
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

# ce_cores.exe doesn't start without them
for dll in cygwin1.dll cygstdc++-6.dll cyggcc_s-seh-1.dll; do
    if [ ! -f "$OUT/$dll" ]; then
        echo "$dll not found by cygcheck - is this a Cygwin build?" >&2
        exit 1
    fi
done

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
CYGWIN_SRC_OK=
for mirror in $CYGWIN_MIRRORS; do
    echo "[windows] $CYGWIN_SRC from $mirror"
    SETUP_INI=$(curl -fsSL --retry 3 --connect-timeout 20 "$mirror/x86_64/setup.ini") || { echo "  cannot download setup.ini" >&2; continue; }
    CYGWIN_SRC_SHA512=$(echo "$SETUP_INI" | awk -v f="src/release/cygwin/$CYGWIN_SRC" '$1 == "source:" && $2 == f { print $4; exit }')
    [ -n "$CYGWIN_SRC_SHA512" ] || { echo "  not listed in setup.ini" >&2; continue; }
    curl -fsSL --retry 3 --connect-timeout 20 -o "$OUT/licenses/$CYGWIN_SRC" "$mirror/src/release/cygwin/$CYGWIN_SRC" || { echo "  cannot download it" >&2; continue; }
    echo "$CYGWIN_SRC_SHA512  $OUT/licenses/$CYGWIN_SRC" | sha512sum -c - >/dev/null || { echo "  doesn't match the checksum in setup.ini" >&2; continue; }
    CYGWIN_SRC_OK=1
    break
done
if [ -z "$CYGWIN_SRC_OK" ]; then
    rm -f "$OUT/licenses/$CYGWIN_SRC"
    echo "cannot get $CYGWIN_SRC with a matching checksum from any mirror: $CYGWIN_MIRRORS" >&2
    exit 1
fi

cat > "$OUT/licenses/CYGWIN-NOTICE.txt" <<EOF
ce_cores.exe uses the Cygwin library (cygwin1.dll), version $CYGWIN_VERSION, https://cygwin.com

The Cygwin library is free software, licensed under the GNU Lesser General Public License version 3
(LGPL-3.0.txt), which is an addition to the GNU General Public License version 3 (GPL-3.0.txt).
Its complete source code is in $CYGWIN_SRC next to this file.

cygwin1.dll is a separate DLL - you may replace it with your own, modified version of the same Cygwin version.

cygstdc++-6.dll and cyggcc_s-seh-1.dll are the GCC runtime libraries (libstdc++, libgcc), licensed under the
GNU General Public License version 3 with the GCC Runtime Library Exception, which allows distributing them with
programs compiled by GCC under any license: https://www.gnu.org/licenses/gcc-exception-3.1.html
EOF

echo "[windows] done"
