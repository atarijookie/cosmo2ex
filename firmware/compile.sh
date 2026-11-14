#!/bin/sh
# Compiles the Pico firmwares with arduino-cli - runs inside the ce-pico-builder image (see build.sh),
# but works also on any machine with arduino-cli and the arduino-pico core installed.
#
# Each project is compiled from a copy, with the *.pio.h regenerated from the *.pio (pio_gen.sh) - so the build always
# matches the .pio files. When the committed .pio.h differ, there's a warning: run firmware/pioasm.sh and commit them,
# the Arduino IDE builds use those.
#
# usage: compile.sh <repo root> <output dir> <project ...>     project: ce_pico_hdd ce_pico_fdd ce_pico_display
#
# The results are named <project>_<DEVICE TYPE>.uf2 (e.g. ce_pico_hdd_W6100-EVB-PICO.uf2) - the device type is taken from
# the identity string in the firmware (fw_update.h: FW_DEVICE_TYPE), the web UI tells the user which file their device needs.
#
# CE_BUILD_ID env var - the build id stored in the firmware identity (default: git describe, 'unknown' without git)

set -eu

SRC=$1
OUT=$2
shift 2

# The file system (FS) of HDD and FDD holds the new firmware during the update from the web UI (fw_update.cpp). Changing
# the flash / FS size changes the flash layout - the update from the web UI refuses such firmware, it must go via USB.

# Raspberry Pi Pico, with the Arduino IDE settings from ce_pico_hdd/readme.md and the top of ce_pico_hdd.ino
FQBN_HDD="rp2040:rp2040:rpipico:flash=2097152_524288,freq=133,opt=Small,os=none,profile=Disabled,rtti=Disabled,stackprotect=Disabled,exceptions=Disabled,dbgport=Disabled,dbglvl=None,usbstack=nousb,ipbtstack=ipv4only,uploadmethod=picoprobe_cmsis_dap"

# Raspberry Pi Pico 2 W, with the Arduino IDE settings from ce_pico_fdd/readme.md and the top of ce_pico_fdd.ino
FQBN_FDD="rp2040:rp2040:rpipico2w:arch=arm,flash=4194304_2097152,freq=150,opt=Small,os=none,profile=Disabled,rtti=Disabled,stackprotect=Disabled,exceptions=Disabled,dbgport=Disabled,dbglvl=None,usbstack=picosdk,ipbtstack=ipv4only,wificountry=worldwide,uploadmethod=picoprobe_cmsis_dap"

# RP2040-Zero (Waveshare and its clones), with the Arduino IDE settings from ce_pico_display/readme.md - Generic RP2040
# with the slow generic flash access (boot2), as the clones come with various flash chips
FQBN_DISPLAY="rp2040:rp2040:generic:flash=2097152_0,freq=133,boot2=boot2_generic_03h_4_padded_checksum,opt=Small,os=none,profile=Disabled,rtti=Disabled,stackprotect=Disabled,exceptions=Disabled,dbgport=Disabled,dbglvl=None,usbstack=picosdk,ipbtstack=ipv4only,uploadmethod=default"

BUILD_ID=${CE_BUILD_ID:-$(git -C "$SRC" describe --tags --always --dirty 2>/dev/null || echo unknown)}
echo "Build id: $BUILD_ID"

BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

mkdir -p "$OUT"

for project in "$@"; do
    echo "Compiling $project"

    case "$project" in
        ce_pico_fdd)     FQBN=$FQBN_FDD ;;
        ce_pico_display) FQBN=$FQBN_DISPLAY ;;
        *)               FQBN=$FQBN_HDD ;;
    esac

    # copy of the sketch with freshly generated PIO headers (the repo might be read-only, as in build.sh)
    SKETCH="$BUILD/src/$project"
    mkdir -p "$SKETCH"
    cp -R "$SRC/$project/." "$SKETCH"
    "$(dirname "$0")/pio_gen.sh" "$SKETCH"
    printf '#define CE_BUILD_ID "%s"\n' "$BUILD_ID" > "$SKETCH/build_id.h"     # used by fw_update.cpp, Arduino IDE builds don't have it

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

    # device type from the firmware identity ("CEFWID:dev=HDD;type=W6100-EVB-PICO;..."), the display has none
    TYPE=$(LC_ALL=C grep -aoE 'CEFWID:dev=[A-Z]+;type=[A-Z0-9-]+' "$BUILD/$project/$project.ino.bin" | head -n 1 | sed 's/.*type=//')
    if [ -z "$TYPE" ]; then
        case "$project" in
            ce_pico_display) TYPE=RP2040-ZERO ;;
            *) echo "ERROR: no firmware identity found in $project"; exit 1 ;;
        esac
    fi

    NAME="${project}_${TYPE}"
    cp "$BUILD/$project/$project.ino.uf2" "$OUT/$NAME.uf2"
    cp "$BUILD/$project/$project.ino.elf" "$OUT/$NAME.elf"
    echo "Created $OUT/$NAME.uf2"
done
