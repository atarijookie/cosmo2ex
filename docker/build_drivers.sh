#!/bin/sh
# Builds the config drive drivers (boot sectors, ce_dd.prg, ce_eth.stx, ce_sting.prg) and ce_brows.prg, ce_video.ttp, ce_audio.ttp (root of the config drive) with m68k-atari-mint-gcc and copies them
# to ce_cores/configdrive/drivers/, where docker/Dockerfile expects them (they are gitignored build outputs).
# Used by docker/build_all.sh and by the CI workflows.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)

echo "Building Atari drivers"
make -C "$ROOT/libacsiscsi"
make -C "$ROOT/ce_dd_prg"                   # on its own - ce_dd_bootsectors 'all' builds it only when the boot sectors are built
make -C "$ROOT/ce_dd_bootsectors" all
make -C "$ROOT/ce_sting_stx_driver"          # ce_eth.stx - port driver for real STinG
make -C "$ROOT/ce_fakesting"                 # ce_sting.prg - fake STinG, networking done by ce_cores
make -C "$ROOT/ce_browser_prg"               # ce_brows.prg - web browser, pages rendered by ce_browser on the host
make -C "$ROOT/ce_media_prg"                 # ce_video.ttp, ce_audio.ttp - players, files decoded by ce_media on the host
mkdir -p "$ROOT/ce_cores/configdrive/drivers"
cp "$ROOT/ce_dd_bootsectors/ce_dd.bs" "$ROOT/ce_dd_bootsectors/ce_dd_l2.bs" "$ROOT/ce_dd_prg/ce_dd.prg" \
    "$ROOT/ce_sting_stx_driver/ce_eth.stx" "$ROOT/ce_fakesting/ce_sting.prg" "$ROOT/ce_cores/configdrive/drivers/"
cp "$ROOT/ce_browser_prg/ce_brows.prg" "$ROOT/ce_cores/configdrive/"     # web browser - in the root of the config drive
cp "$ROOT/ce_media_prg/ce_video.ttp" "$ROOT/ce_media_prg/ce_audio.ttp" "$ROOT/ce_cores/configdrive/"    # players - root too
