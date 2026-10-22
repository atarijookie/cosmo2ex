#!/bin/sh
# Compiles the Pico firmwares with arduino-cli - runs inside the ce-pico-builder image (see build.sh),
# but works also on any machine with arduino-cli and the arduino-pico core installed.
#
# Each project is compiled from a copy, with the *.pio.h regenerated from the *.pio (pio_gen.sh) - so the build always
# matches the .pio files. When the committed .pio.h differ, there's a warning: run firmware/pioasm.sh and commit them,
# the Arduino IDE builds use those.
#
# usage: compile.sh <repo root> <output dir> <project ...>     project: ce_pico_hdd ce_pico_hdd_acsi ce_pico_hdd_scsi

set -eu

SRC=$1
OUT=$2
shift 2

# Raspberry Pi Pico, with the Arduino IDE settings from ce_pico_hdd_*/readme.md
FQBN="rp2040:rp2040:rpipico:flash=2097152_0,freq=133,opt=Small,os=none,profile=Disabled,rtti=Disabled,stackprotect=Disabled,exceptions=Disabled,dbgport=Disabled,dbglvl=None,usbstack=nousb,ipbtstack=ipv4only,uploadmethod=picoprobe_cmsis_dap"

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

mkdir -p "$OUT"

for project in "$@"; do
    echo "Compiling $project"

    # copy of the sketch with freshly generated PIO headers (the repo might be read-only, as in build.sh)
    SKETCH="$BUILD/src/$project"
    mkdir -p "$SKETCH"
    cp -R "$SRC/$project/." "$SKETCH"
    "$(dirname "$0")/pio_gen.sh" "$SKETCH"

    for header in "$SKETCH"/*.pio.h; do
        [ -e "$header" ] || continue
        name=$(basename "$header")
        if ! cmp -s "$header" "$SRC/$project/$name"; then
            echo "WARNING: $project/$name is not up to date with its .pio - run firmware/pioasm.sh $project and commit it"
        fi
    done

    # the (slightly patched) Ethernet library shared by all the projects, must win over any other one
    arduino-cli compile \
        --fqbn "$FQBN" \
        --library "$SRC/third_party/Ethernet" \
        --build-path "$BUILD/$project" \
        --warnings default \
        "$SKETCH"

    cp "$BUILD/$project/$project.ino.uf2" "$OUT/$project.uf2"
    cp "$BUILD/$project/$project.ino.elf" "$OUT/$project.elf"
    echo "Created $OUT/$project.uf2"
done
