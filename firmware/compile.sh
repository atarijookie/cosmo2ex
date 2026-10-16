#!/bin/sh
# Compiles the Pico firmwares with arduino-cli - runs inside the ce-pico-builder image (see build.sh),
# but works also on any machine with arduino-cli and the arduino-pico core installed.
#
# usage: compile.sh <repo root> <output dir> <project ...>     project: ce_pico_hdd_acsi ce_pico_hdd_scsi

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

    # the (slightly patched) Ethernet library shared by both projects, must win over any other one
    arduino-cli compile \
        --fqbn "$FQBN" \
        --library "$SRC/third_party/Ethernet" \
        --build-path "$BUILD/$project" \
        --warnings default \
        "$SRC/$project"

    cp "$BUILD/$project/$project.ino.uf2" "$OUT/$project.uf2"
    cp "$BUILD/$project/$project.ino.elf" "$OUT/$project.elf"
    echo "Created $OUT/$project.uf2"
done
