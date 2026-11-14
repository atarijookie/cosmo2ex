Pico firmwares - ce_pico_hdd (Pico), ce_pico_fdd (Pico 2 W) and ce_pico_display
(RP2040-Zero, the FDD front panel) - Arduino sketches for the arduino-pico core, built on Linux with arduino-cli in a
docker image.

To build them (from the repo root):
make firmware
or
firmware/build.sh                       # all of them
firmware/build.sh ce_pico_hdd           # just one

The results are in firmware/out/:
ce_pico_hdd_W6100-EVB-PICO.uf2
                                            - hold BOOTSEL while plugging in the Pico,
                                              then copy the .uf2 onto the RPI-RP2 drive
ce_pico_fdd_PICO-2W.uf2                     - the same on the Pico 2 W, its drive is RP2350
ce_pico_display_RP2040-ZERO.uf2             - the same on the RP2040-Zero (hold BOOT), its drive is RPI-RP2
*.elf (same names)                          - for flashing / debugging with a debug probe

The part after the project name is the device type (board), in capitals - the HDD / FDD device reports it and the
web UI (device page, Update) shows which file the device needs. It comes from FW_DEVICE_TYPE in fw_update.h of each
project. HDD and FDD can be updated from the web UI too (once they run a firmware with the file system partition).
The firmware identity contains a build id - git describe (e.g. v1.0.0), or CE_BUILD_ID env var.

ce_pico_hdd is for the v40_pico_hdd board (ACSI or SCSI variant, detected on boot).

Files:
Dockerfile  - the ce-pico-builder image: arduino-cli + arduino-pico core + U8g2 library (pinned versions)
build.sh    - builds the image (docker layer cache makes it instant after the first time)
              and runs compile.sh in it, with the repo mounted read-only
compile.sh  - the arduino-cli compile itself; the board settings (FQBN) match the Arduino
              IDE settings in ce_pico_hdd/readme.md, ce_pico_fdd/readme.md and
              ce_pico_display/readme.md - change them in both places.
              Compiles a copy of each project with the *.pio.h regenerated from the *.pio,
              and warns when the committed ones are not up to date
pio_gen.sh  - runs pioasm (from the arduino-pico core) on all *.pio in the given dirs
pioasm.sh   - regenerates the committed *.pio.h in the image - run it after changing
              a .pio file and commit the result (the Arduino IDE doesn't run pioasm):
              firmware/pioasm.sh                    # all projects
              firmware/pioasm.sh ce_pico_hdd        # just one

To use a prebuilt image instead of building it locally:
PICO_BUILDER_IMAGE=ghcr.io/<owner>/ce-pico-builder:latest firmware/build.sh

The image on ghcr.io is tagged with the hash of the Dockerfile. On release (and CI),
if the image for the current Dockerfile isn't on ghcr.io yet, it gets built and pushed,
otherwise the existing one is pulled - so to update arduino-cli, the arduino-pico
core or U8g2, just change the versions in the Dockerfile.

License: open design, non-commercial license - see LICENSE / the License section of the main README.md.
