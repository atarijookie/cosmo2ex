# Build environment for the release tarballs, used by release/build.sh.
#
# Debian bullseye on purpose: binaries built against its glibc (2.31) run on
# bullseye and anything newer - Raspberry Pi OS bullseye/bookworm, Debian 11+,
# Ubuntu 20.04+ and most other current distros. Building on a newer distro
# would make ce_cores.elf require a newer glibc than many Pis have.
#
# ce_cores is cross compiled for all architectures (no QEMU needed), the Go
# webserver is cross compiled with CGO disabled.

FROM golang:1.22-bookworm AS go

FROM debian:bullseye

# bullseye is past its end of life, so its packages are only on archive.debian.org
# (frozen, which is fine for a build environment)
RUN printf '%s\n' \
        'deb http://archive.debian.org/debian bullseye main' \
        'deb http://archive.debian.org/debian-security bullseye-security main' \
        > /etc/apt/sources.list \
    && rm -f /etc/apt/sources.list.d/* \
    && echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99archive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        make \
        g++ \
        g++-aarch64-linux-gnu \
        g++-arm-linux-gnueabihf \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# the Go toolchain is a static binary, so it runs fine on bullseye
COPY --from=go /usr/local/go /usr/local/go
ENV PATH=/usr/local/go/bin:$PATH \
    GOPATH=/go \
    GOCACHE=/go/cache \
    GOFLAGS=-buildvcs=false
