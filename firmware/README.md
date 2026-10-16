Pico firmwares - ce_pico_hdd_acsi and ce_pico_hdd_scsi (Arduino sketches for the
arduino-pico core), built on Linux with arduino-cli in a docker image.

To build them (from the repo root):
make firmware
or
firmware/build.sh                       # all of them
firmware/build.sh ce_pico_hdd_acsi      # just one

The results are in firmware/out/:
ce_pico_hdd_acsi.uf2, ce_pico_hdd_scsi.uf2  - hold BOOTSEL while plugging in the Pico,
                                              then copy the .uf2 onto the RPI-RP2 drive
ce_pico_hdd_acsi.elf, ce_pico_hdd_scsi.elf  - for flashing / debugging with a debug probe

Files:
Dockerfile  - the ce-pico-builder image: arduino-cli + arduino-pico core (pinned versions)
build.sh    - builds the image (docker layer cache makes it instant after the first time)
              and runs compile.sh in it, with the repo mounted read-only
compile.sh  - the arduino-cli compile itself; the board settings (FQBN) match the Arduino
              IDE settings in ce_pico_hdd_*/readme.md - change them in both places

To use a prebuilt image instead of building it locally:
PICO_BUILDER_IMAGE=ghcr.io/<owner>/ce-pico-builder:latest firmware/build.sh

The image on ghcr.io is tagged with the hash of the Dockerfile. On release (and CI),
if the image for the current Dockerfile isn't on ghcr.io yet, it gets built and pushed,
otherwise the existing one is pulled - so to update arduino-cli or the arduino-pico
core, just change the versions in the Dockerfile.
