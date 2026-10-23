# Builder image for the Pico firmwares (ce_pico_hdd_acsi, ce_pico_hdd_scsi, ce_pico_fdd, ce_pico_display):
# arduino-cli + the arduino-pico core (earlephilhower) + U8g2 (Arduino library manager), no sources inside - those
# are mounted at run time, see firmware/build.sh.
#
# Release pushes this image to ghcr.io/<owner>/ce-pico-builder:<hash of this file>, only when this file
# changes (see .github/workflows/release.yml), so bump the versions here to get a new image.

FROM debian:bookworm-slim

ARG ARDUINO_CLI_VERSION=1.5.1
# the version the firmwares were developed with, see ce_pico_hdd_acsi/readme.md
ARG ARDUINO_PICO_VERSION=5.4.2
# display library of ce_pico_display
ARG U8G2_VERSION=2.36.19
ARG ARDUINO_PICO_INDEX=https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl python3 \
 && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" \
    | tar -xz -C /usr/local/bin arduino-cli

# the core and the libraries live in /opt/arduino, readable by any user - build.sh runs the container as the calling
# user, which then gets a writable HOME and user dir in /tmp; the libraries are installed as builtin ones, as the user
# dir is in /tmp
ENV ARDUINO_DIRECTORIES_DATA=/opt/arduino \
    ARDUINO_DIRECTORIES_BUILTIN_LIBRARIES=/opt/arduino/libraries \
    ARDUINO_DIRECTORIES_DOWNLOADS=/tmp/arduino-staging \
    ARDUINO_DIRECTORIES_USER=/tmp/arduino-user \
    ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=${ARDUINO_PICO_INDEX} \
    ARDUINO_UPDATER_ENABLE_NOTIFICATION=false \
    HOME=/tmp

RUN arduino-cli core update-index \
 && arduino-cli core install "rp2040:rp2040@${ARDUINO_PICO_VERSION}" \
 && arduino-cli lib update-index \
 && arduino-cli lib install --install-in-builtin-dir "U8g2@${U8G2_VERSION}" \
 && rm -rf /tmp/arduino-staging \
 && chmod -R a+rX /opt/arduino

LABEL org.opencontainers.image.description="arduino-cli + arduino-pico ${ARDUINO_PICO_VERSION} + U8g2 ${U8G2_VERSION} for building the CosmosEx Pico firmwares"
