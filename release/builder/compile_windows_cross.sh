#!/bin/sh
# Runs inside the ce-windows-cross image, called by build_windows_cross.sh - see there.
#
# usage: compile_windows_cross.sh <repo dir> <build dir> <output dir>

set -eu

SRC="$1"
BUILD="$2"
OUT="$3"

T=x86_64-pc-cygwin

#-------------
# current sources to the build dir - build outputs of the checkout (Linux ones) are excluded, and the excluded files
# in the build dir (Cygwin ones from the previous build) are kept, so make rebuilds only what changed
echo "[windows-cross] syncing sources"
for d in libdospath ce_cores webserver third_party; do
    rsync -a --delete \
        --exclude '*.o' --exclude '*.d' --exclude '*.a' --exclude '*.elf' --exclude '*.exe' \
        --exclude '/obj/' --exclude '/lib/' \
        "$SRC/$d/" "$BUILD/$d/"
done
mkdir -p "$BUILD/ce_cores/obj" "$BUILD/ce_cores/lib"

#-------------
echo "[windows-cross] building ce_cores"
# UNAME_S selects the Cygwin part of the makefiles (also in libdospath, the sub-make gets it from the command line)
make -C "$BUILD/ce_cores" -j"$(nproc)" UNAME_S=CYGWIN_NT-10.0 CXX=$T-g++ CC=$T-gcc AR=$T-ar

#-------------
echo "[windows-cross] building webserver + ce_run"
(cd "$BUILD/webserver" &&
    CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -trimpath -o "$OUT/webserver.exe" . &&
    CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -trimpath -o "$OUT/ce_run.exe" ./cmd/ce_run)

#-------------
# ready to run folder - the same layout as the Windows zip made by release/build.sh
echo "[windows-cross] assembling $OUT"
cp "$BUILD/ce_cores/ce_cores.exe" "$OUT/"

# the DLLs of the cross compiler - ce_cores.exe is built against its libstdc++, an older cygstdc++-6.dll might miss symbols
for dll in cygwin1.dll cygstdc++-6.dll cyggcc_s-seh-1.dll; do
    cp "$CYGWIN_SYSROOT/usr/bin/$dll" "$OUT/"
done

if [ ! -f "$OUT/.env" ]; then
    cp "$SRC/release/files/windows.env" "$OUT/.env"
fi
cp "$SRC/release/files/windows-README.txt" "$OUT/README.txt"
echo "$VERSION" > "$OUT/VERSION"

rsync -a --delete "$SRC/webserver/static/" "$OUT/static/"
cp "$SRC/ce_cores/core/ce_logo.bin" "$SRC/ce_cores/core/ce_conf.st" "$SRC/ce_cores/core/fdd_test.st" "$SRC/ce_cores/core/ce_conf.zip" "$OUT/"

# config drive - same layering as release/build.sh, with the Atari programs built in the checkout (if they are there)
rm -rf "$OUT/configdrive"
cp -r "$SRC/ce_cores/configdrive" "$OUT/configdrive"
cp -r "$SRC/ce_cores/core/configdrive/." "$OUT/configdrive/"
mkdir -p "$OUT/configdrive/drivers" "$OUT/configdrive/tests"
for f in ce_dd_bootsectors/ce_dd.bs ce_dd_bootsectors/ce_dd_l2.bs ce_dd_prg/ce_dd.prg; do
    if [ -f "$SRC/$f" ]; then cp "$SRC/$f" "$OUT/configdrive/drivers/"; fi
done
if [ -f "$SRC/test_ce/ce_tests.tos" ]; then cp "$SRC/test_ce/ce_tests.tos" "$OUT/configdrive/tests/"; fi

for f in drivers/ce_dd.bs drivers/ce_dd_l2.bs drivers/ce_dd.prg; do
    if [ ! -f "$OUT/configdrive/$f" ]; then
        echo "[windows-cross] WARNING: configdrive/$f is missing - build the Atari programs (needs m68k-atari-mint-gcc)" >&2
    fi
done

chown -R "$OUT_UID:$OUT_GID" "$OUT"
echo "[windows-cross] done"
