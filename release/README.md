# Release tarballs

Builds CosmosEx for a direct install on a Linux machine (Raspberry Pi or PC), without docker
on the target machine and without building anything there.

## Building

From the repo root:

```
release/build.sh                # all architectures: amd64 arm64 armhf
release/build.sh arm64          # just some of them
release/build.sh --native       # only for this machine, using its own toolchains (no docker)
```

Output goes to `release/out/`:

```
ce-<version>-amd64.tar.gz
ce-<version>-arm64.tar.gz
ce-<version>-armhf.tar.gz
install.sh
SHA256SUMS
```

The version defaults to `<date>-<git hash>` (with `-dirty` for uncommitted changes), set `VERSION=...` to override it.

Needed on the build machine:
- `m68k-atari-mint-gcc` - the Atari programs on the config drive (`ce_dd.prg`, boot sectors, `ce_tests.tos`)
  are built once, on the build machine, and go into every tarball
- `docker` - `ce_cores` and `webserver` are built in the `release/builder` image, which cross compiles for all
  the architectures (no QEMU needed)
- for `--native` instead of docker: `make`, `g++`, `go`

The builder image is Debian bullseye, so the binaries need glibc 2.28 or newer: Raspberry Pi OS / Debian 10+,
Ubuntu 20.04+ and most other current distros. `libstdc++` is linked in statically, the webserver is a static Go binary.
`armhf` is ARMv7 - Raspberry Pi 2 and newer running 32-bit OS, Pi 1 / Zero (ARMv6) are not supported.

## Installing

Copy the tarball for the machine (or all of them) and `install.sh` to the target machine, then:

```
sudo ./install.sh                               # picks the newest tarball for this machine next to the script
sudo ./install.sh ce-<version>-arm64.tar.gz     # or a specific one
```

This installs to `/opt/ce/bin`, sets up and starts the `ce` systemd service:

```
systemctl status ce
journalctl -u ce -f
```

Upgrade = run `install.sh` with the new tarball. Settings in `/opt/ce/settings` are kept. If you changed
`/opt/ce/bin/.env`, your version is kept and the one from the new release is saved as `.env.new`.

```
sudo ./install.sh --uninstall           # keeps settings, data and logs
sudo ./install.sh --uninstall --purge   # removes everything
```

Without systemd the files are installed, but you have to start it yourself: `cd /opt/ce/bin && sudo ./ce_run.sh`.

## Files

- `build.sh` - builds everything and creates the tarballs
- `install.sh` - installs / upgrades / uninstalls from a tarball
- `builder/Dockerfile` - build environment (bullseye + cross compilers + Go)
- `builder/build_linux.sh` - builds `ce_cores` + `webserver` for one architecture
- `files/ce_run.sh` - runs `ce_cores` + `webserver` in the foreground, used by the service
- `files/ce.service` - the systemd unit
