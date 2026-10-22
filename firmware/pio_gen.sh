#!/bin/sh
# Generates <name>.pio.h from each <name>.pio in the given directories with pioasm (-o c-sdk). Runs inside the
# ce-pico-builder image (used by compile.sh and pioasm.sh), but works also on any machine with the arduino-pico core.
#
# usage: pio_gen.sh <dir ...>
#
# PIOASM env var - path to pioasm, found in the arduino-pico core tools when not set

set -eu

PIOASM=${PIOASM:-$(find /opt/arduino "$HOME/.arduino15" -path '*pqt-pioasm*' -name pioasm -type f 2>/dev/null | head -n 1)}

if [ -z "$PIOASM" ]; then
    echo "pioasm not found - set PIOASM" >&2
    exit 1
fi

for dir in "$@"; do
    for pio in "$dir"/*.pio; do
        [ -e "$pio" ] || continue       # no .pio files in this dir
        "$PIOASM" -o c-sdk "$pio" "$pio.h"
    done
done
