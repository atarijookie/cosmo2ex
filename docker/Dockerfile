# syntax=docker/dockerfile:1
#
# CosmosEx Linux apps: ce_cores (C++) + webserver (Go) in one image.
#
# Build from the repository root, for this machine:
#   docker build -f docker/Dockerfile -t cosmo2ex .
#
# For all platforms at once (amd64, arm64, arm/v7, arm/v6 = Pi 1 / Zero, 386), see docker/build_all.sh.
# The programs are always cross compiled on the build machine (no QEMU), the target platform
# image only gets the files copied in, so any platform can be built on any machine.
#
# Preferred run (bind-mounts settings + host filesystem, host network
# so UDP 7200 can receive LAN broadcasts and reply unicast on 7201):
#   docker compose -f docker/docker-compose.yml up --build
#
# Equivalent docker run:
#   docker run --rm --network host \
#     -v /opt/ce/settings:/opt/ce/settings \
#     -v /opt/ce/log:/var/log/ce \
#     -e CE_LOG_LEVEL=3 \
#     -v /:/mnt/hostdrive:rslave \
#     -v /dev:/dev \
#     --device-cgroup-rule='c 13:* rwm' \
#     --device-cgroup-rule='b 8:* rwm' \
#     cosmo2ex
#
# Layout matches the defaults in /.env:
#   BIN_DIR=/opt/ce/bin   SETTINGS_DIR=/opt/ce/settings
#   DATA_DIR=/var/lib/ce  LOG_DIR=/var/log/ce  PID_DIR=/run/ce
# Host root is mounted at /mnt/hostdrive (see docker-compose.yml).

# ---------------------------------------------------------------------------
# Toolchain - runs on the build machine. Also the build environment of the release
# tarballs (release/build.sh builds just this stage).
#
# Debian bullseye on purpose: binaries built against its glibc (2.31) run on
# bullseye and anything newer - Raspberry Pi OS bullseye/bookworm, Debian 11+,
# Ubuntu 20.04+ and most other current distros. Building on a newer distro
# would make ce_cores.elf require a newer glibc than many Pis have.
#
# ce_cores is cross compiled (see release/builder/build_linux.sh), the Go webserver
# is cross compiled with CGO disabled. armhf is ARMv6, so it runs on every Pi,
# including Pi 1 / Zero.
# ---------------------------------------------------------------------------
FROM --platform=$BUILDPLATFORM golang:1.22-bookworm AS go

FROM --platform=$BUILDPLATFORM debian:bullseye AS toolchain

# bullseye is past its end of life, so its packages are only on archive.debian.org
# (frozen, which is fine for a build environment)
RUN printf '%s\n' \
        'deb http://archive.debian.org/debian bullseye main' \
        'deb http://archive.debian.org/debian-security bullseye-security main' \
        > /etc/apt/sources.list \
    && rm -f /etc/apt/sources.list.d/* \
    && printf '%s\n' 'Acquire::Check-Valid-Until "false";' 'Acquire::Retries "5";' > /etc/apt/apt.conf.d/99archive

# native g++ plus cross compilers for the other architectures - the ones Debian has for this
# build machine (all of them on amd64)
RUN set -e; \
    apt-get update; \
    native=$(dpkg --print-architecture); \
    pkgs="make g++ ca-certificates curl"; \
    for p in amd64:g++-x86-64-linux-gnu arm64:g++-aarch64-linux-gnu armhf:g++-arm-linux-gnueabihf i386:g++-i686-linux-gnu; do \
        [ "${p%%:*}" = "$native" ] && continue; \
        if apt-cache show "${p#*:}" >/dev/null 2>&1; then pkgs="$pkgs ${p#*:}"; else echo "no ${p#*:} for $native, skipped"; fi; \
    done; \
    apt-get install -y --no-install-recommends $pkgs; \
    rm -rf /var/lib/apt/lists/*

# apt for the Raspbian archive, separate from the system apt. The packages come through apt, so
# their signatures are checked against the Raspbian archive key, whose hash is pinned here.
ARG RASPBIAN_KEY_SHA256=ca59cd4f2bcbc3a1d41ba6815a02a8dc5c175467a59bd87edeac458f4a5345de
RUN set -e; \
    A=/etc/rpi-apt; mkdir -p $A/lists/partial $A/cache/archives/partial; \
    curl -fsSL -o $A/raspbian.asc http://archive.raspbian.org/raspbian.public.key; \
    echo "$RASPBIAN_KEY_SHA256  $A/raspbian.asc" | sha256sum -c -; \
    echo "deb [arch=armhf signed-by=$A/raspbian.asc] http://archive.raspbian.org/raspbian bullseye main" > $A/sources.list
COPY <<'EOF' /usr/local/bin/rpi-apt-get
#!/bin/sh
# apt-get for the Raspbian (ARMv6 armhf) archive, e.g.: rpi-apt-get update; rpi-apt-get download libc6
A=/etc/rpi-apt
exec apt-get -o Dir::Etc::SourceList=$A/sources.list -o Dir::Etc::SourceParts=/dev/null \
    -o Dir::State::Lists=$A/lists -o Dir::Cache=$A/cache \
    -o APT::Architecture=armhf -o APT::Architectures=armhf "$@"
EOF

# Raspbian sysroot for armhf. Debian's armhf runtime (crt*.o, libgcc, libstdc++, libc_nonshared)
# is ARMv7 / Thumb-2 and gets linked in statically, so it would crash on Pi 1 / Zero. Raspbian ships
# the same gcc 10.2.1 and glibc 2.31 built for ARMv6+VFP, so the Debian cross compiler links
# against these instead (see build_linux.sh).
RUN set -e; \
    chmod +x /usr/local/bin/rpi-apt-get; \
    mkdir -p /tmp/debs; cd /tmp/debs; \
    rpi-apt-get update; \
    rpi-apt-get download libc6 libc6-dev linux-libc-dev libgcc-s1 libgcc-10-dev libstdc++6 libstdc++-10-dev; \
    for deb in /tmp/debs/*.deb; do dpkg-deb -x "$deb" /opt/rpi-sysroot; done; \
    rm -rf /tmp/debs /etc/rpi-apt/lists/* /etc/rpi-apt/cache/*.bin; \
    mkdir -p /etc/rpi-apt/lists/partial; \
    # the dev symlinks are absolute (libpthread.so -> /lib/...), make them point inside the sysroot,
    # otherwise the linker skips the broken .so and links the static .a instead
    find /opt/rpi-sysroot -type l -lname '/*' | while read -r l; do \
        ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "/opt/rpi-sysroot$(readlink "$l")")" "$l"; \
    done

# the Go toolchain is a static binary, so it runs fine on bullseye
COPY --from=go /usr/local/go /usr/local/go
ENV PATH=/usr/local/go/bin:$PATH \
    GOPATH=/go \
    GOCACHE=/go/cache \
    GOFLAGS=-buildvcs=false

# ---------------------------------------------------------------------------
# ce_cores + webserver, cross compiled for the target platform
# ---------------------------------------------------------------------------
FROM toolchain AS build
ARG TARGETARCH

COPY libdospath/ /src/libdospath/
COPY ce_cores/ /src/ce_cores/
COPY webserver/ /src/webserver/
COPY third_party/ /src/third_party/
COPY release/builder/build_linux.sh /src/release/builder/

RUN --mount=type=cache,id=ce-go,target=/go set -e; \
    case "$TARGETARCH" in \
        amd64|arm64)    arch=$TARGETARCH ;; \
        arm)            arch=armhf ;; \
        386)            arch=i386 ;; \
        *)              echo "unsupported platform: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    mkdir -p /out; \
    /src/release/builder/build_linux.sh "$arch" /src /out

# ---------------------------------------------------------------------------
# Packages for the target platform - downloaded and unpacked on the build machine,
# so nothing has to run on the target platform during the build.
#   /overlay  - tini, for every platform
#   /rootfs   - arm/v6 only: minimal Raspbian userland (Debian has no ARMv6 armhf image)
# ---------------------------------------------------------------------------
FROM toolchain AS packages
ARG TARGETARCH
ARG TARGETVARIANT

RUN set -e; \
    mkdir -p /debs /overlay/usr/bin /rootfs; cd /debs; \
    case "$TARGETARCH" in \
        # Raspbian for both arm variants - its ARMv6 tini runs on ARMv7 too
        arm)    rpi-apt-get update; rpi-apt-get download tini ;; \
        amd64|arm64)    debarch=$TARGETARCH ;; \
        386)    debarch=i386 ;; \
    esac; \
    if [ "$TARGETARCH" != arm ]; then \
        dpkg --add-architecture "$debarch"; apt-get update; apt-get download "tini:$debarch"; \
    fi; \
    # the static tini, so it doesn't depend on the libc of the base image
    dpkg-deb --fsys-tarfile tini_*.deb | tar -xO ./usr/bin/tini-static > /overlay/usr/bin/tini; \
    chmod 755 /overlay/usr/bin/tini; \
    if [ "$TARGETARCH$TARGETVARIANT" = armv6 ]; then \
        # what entrypoint.sh needs: sh, coreutils (incl. stdbuf), sed - and their libraries.
        # Plus timezone data, which the Debian base images already have.
        rm -f /debs/*.deb; \
        rpi-apt-get download libc6 libgcc-s1 dash coreutils sed \
            libacl1 libattr1 libselinux1 libpcre2-8-0 libgmp10 tzdata; \
        for deb in /debs/*.deb; do dpkg-deb -x "$deb" /rootfs; done; \
        ln -sf dash /rootfs/bin/sh; \
        mkdir -p /rootfs/etc /rootfs/tmp /rootfs/root /rootfs/proc /rootfs/sys /rootfs/dev /rootfs/run; \
        chmod 1777 /rootfs/tmp; \
        echo 'root:x:0:0:root:/root:/bin/sh' > /rootfs/etc/passwd; \
        echo 'root:x:0:' > /rootfs/etc/group; \
    fi; \
    rm -rf /var/lib/apt/lists/* /etc/rpi-apt/lists/* /debs

# ---------------------------------------------------------------------------
# /opt/ce and the other dirs - the same for every platform
# ---------------------------------------------------------------------------
FROM --platform=$BUILDPLATFORM debian:bullseye AS files

# Defaults from .env - used for the destinations here.
ENV CE_DIR=/opt/ce \
    BIN_DIR=/opt/ce/bin \
    SETTINGS_DIR=/opt/ce/settings \
    DATA_DIR=/var/lib/ce \
    LOG_DIR=/var/log/ce \
    PID_DIR=/run/ce \
    HOSTDRIVE=/mnt/hostdrive

RUN mkdir -p "/files$BIN_DIR" "/files$SETTINGS_DIR" "/files$DATA_DIR" "/files$LOG_DIR" "/files$PID_DIR" "/files$HOSTDRIVE"

WORKDIR /files/opt/ce/bin

# .env defaults (repo-root file; both apps load ./ .env from BIN_DIR)
COPY .env ./.env

# Webserver static files (served as ./static from BIN_DIR)
COPY webserver/static/ ./static/

# ce_cores binary assets (logo, floppy images, config drive)
COPY ce_cores/ce_logo.bin ce_cores/core/ce_conf.st ce_cores/core/fdd_test.st ce_cores/core/ce_conf.zip ./
COPY ce_cores/configdrive/ ./configdrive/
COPY ce_cores/core/configdrive/ ./configdrive/

# Helper scripts used on the host install; handy inside the container too
COPY shellscripts/ce_start.sh shellscripts/ce_stop.sh docker/entrypoint.sh ./

RUN sed -i 's/\r$//' entrypoint.sh ce_start.sh ce_stop.sh .env \
    && chmod +x ce_start.sh ce_stop.sh entrypoint.sh

# Fail the build if the files ce_cores loads on start are not where .env says they are.
# The configdrive drivers (*.bs, *.prg) are gitignored, so they must be present in the build context.
# (ce_cores.elf and webserver are copied in later, from the build stage)
RUN set -e; . ./.env; \
    for d in "$CE_DIR" "$BIN_DIR"; do \
        [ -d "/files$d" ] || { echo "missing dir from .env: $d" >&2; exit 1; }; \
    done; \
    for f in "$CE_DD_BS_L1_FILE" "$CE_DD_BS_L2_FILE" "$CE_DD_PRG_FILE" "$CE_LOGO_FILE" "$BIN_DIR/static/index.html"; do \
        [ -f "/files$f" ] || { echo "missing file from .env: $f" >&2; exit 1; }; \
    done

# ---------------------------------------------------------------------------
# Runtime image - base per platform, everything else is copied in
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS base-debian
FROM base-debian AS base-amd64
FROM base-debian AS base-arm64
FROM base-debian AS base-arm64v8
FROM base-debian AS base-armv7
FROM base-debian AS base-386
FROM scratch AS base-armv6
COPY --from=packages /rootfs/ /

FROM base-${TARGETARCH}${TARGETVARIANT}

ENV CE_DIR=/opt/ce \
    BIN_DIR=/opt/ce/bin \
    SETTINGS_DIR=/opt/ce/settings \
    DATA_DIR=/var/lib/ce \
    LOG_DIR=/var/log/ce \
    PID_DIR=/run/ce \
    CONFIG_DRIVE_PATH=/var/lib/ce/configdrive \
    HOSTDRIVE=/mnt/hostdrive

COPY --from=packages /overlay/ /
COPY --from=files /files/ /
COPY --from=build /out/ce_cores.elf /out/webserver /opt/ce/bin/

WORKDIR /opt/ce/bin

EXPOSE 8080
EXPOSE 7200/udp
EXPOSE 7300 7301 7302

ENTRYPOINT ["/usr/bin/tini", "--", "/opt/ce/bin/entrypoint.sh"]
