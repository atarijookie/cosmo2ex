#!/bin/sh
# Builds the config drive drivers (boot sectors, ce_dd.prg) with m68k-atari-mint-gcc and copies them
# to ce_cores/configdrive/drivers/, where docker/Dockerfile expects them (they are gitignored build outputs).
# Used by docker/build_all.sh and by the CI workflows.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)

echo "Building Atari drivers"
make -C "$ROOT/libacsiscsi"
make -C "$ROOT/ce_dd_bootsectors" all       # builds also ce_dd_prg
mkdir -p "$ROOT/ce_cores/configdrive/drivers"
cp "$ROOT/ce_dd_bootsectors/ce_dd.bs" "$ROOT/ce_dd_bootsectors/ce_dd_l2.bs" "$ROOT/ce_dd_prg/ce_dd.prg" \
    "$ROOT/ce_cores/configdrive/drivers/"
