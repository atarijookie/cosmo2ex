# Release tarballs

Builds CosmosEx for a direct install on a Linux machine (Raspberry Pi or PC), without docker
on the target machine and without building anything there.

## Building

From the repo root:

```
release/build.sh                # all architectures: amd64 arm64 armhf i386
release/build.sh arm64          # just some of them
release/build.sh --native       # only for this machine, using its own toolchains (no docker)
```

Output goes to `release/out/`:

```
ce-<version>-amd64.tar.gz
ce-<version>-arm64.tar.gz
ce-<version>-armhf.tar.gz
ce-<version>-i386.tar.gz
install.sh
SHA256SUMS
```

The version defaults to `<date>-<git hash>` (with `-dirty` for uncommitted changes), set `VERSION=...` to override it.

Needed on the build machine:
- `m68k-atari-mint-gcc` - the Atari programs on the config drive (`ce_dd.prg`, boot sectors, `ce_tests.tos`)
  are built once, on the build machine, and go into every tarball
- `docker` - `ce_cores` and `webserver` are built in the toolchain stage of `docker/Dockerfile`, which cross compiles for all
  the architectures (no QEMU needed)
- for `--native` instead of docker: `make`, `g++`, `go`, `libudev-dev`, `libevdev-dev`

The builder image is Debian bullseye, so the binaries need glibc 2.28 or newer: Raspberry Pi OS / Debian 10+,
Ubuntu 20.04+ and most other current distros. `libstdc++` is linked in statically, the webserver is a static Go binary.
`armhf` is ARMv6+VFP, same as Raspbian - it runs on every Pi with a 32-bit OS, including Pi 1 / Zero.
The Debian armhf runtime libraries are ARMv7, so the builder image links `armhf` against a Raspbian
bullseye sysroot instead (same gcc and glibc versions, see `docker/Dockerfile`).
`i386` is 32-bit x86 (i686, no SSE2 needed), for old PCs and for 32-bit distros on 64-bit machines.

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

## macOS

The macOS release is a zip, `ce-<version>-macos-universal.zip`: one folder, unzip it anywhere and double-click
`CosmosEx.command` - that opens Terminal and runs `ce_run` in it (see `files/macos-README.txt`, which goes into the zip
as `README.txt`). Universal programs, Apple Silicon (arm64) + Intel (x86_64), macOS 11+.

- `ce_run` - the same launcher as on Windows (`webserver/cmd/ce_run`); Ctrl+C or closing the Terminal window stops
  both programs
- `.env` - `files/macos.env`: settings, logs and data in `~/Library/Application Support/cosmo2ex`; `BIN_DIR` is the
  folder of `ce_run`, set by `ce_run`
- `ce_cores`, `webserver` - universal binaries
- the rest is the same as in the Linux tarballs (web UI, config drive with the Atari programs, floppy images)

The programs are signed only ad-hoc (no Apple Developer ID), so macOS blocks them when downloaded - the README tells
the user to remove the quarantine flag once (`xattr -dr com.apple.quarantine <folder>`). A Developer ID signature +
notarization would remove that step, but needs a paid Apple developer account.

Building takes two steps, like for Windows:

1. on a Mac with the Xcode command line tools and Go: `release/builder/build_macos.sh <repo dir> <output dir>` -
   builds `ce_cores`, `webserver` and `ce_run` per architecture (Go with `GOARCH=arm64` / `amd64`, `ce_cores` with
   `clang++ -arch arm64` / `-arch x86_64`), combines them with `lipo`, signs them ad-hoc and checks both architectures
   are there. CI runs it too and keeps the result as the `macos-ci` artifact,
   packed in `ce-macos-ci.tar.gz` (GitHub zips artifacts without file permissions, the tar keeps the programs executable;
   in the release workflow `release/build.sh` sets them again instead).
2. on Linux: `MACOS_BIN=<that output dir> release/build.sh macos` - makes the zip (needs `python3`)

The release workflow does both (`macos` job, then `tarballs`).

## Files

- `build.sh` - builds everything and creates the tarballs (and the Windows / macOS zips)
- `install.sh` - installs / upgrades / uninstalls from a tarball
- `builder/build_linux.sh` - builds `ce_cores` + `webserver` for one architecture
- `files/ce_run.sh` - runs `ce_cores` + `webserver` in the foreground, used by the service
- `files/ce.service` - the systemd unit
- `builder/build_windows.sh` - builds the Windows programs, in Cygwin
- `builder/build_macos.sh` - builds the macOS programs as universal binaries, on a Mac
- `files/windows.env`, `files/windows-README.txt` - `.env` and `README.txt` of the Windows zip
- `files/macos.env`, `files/macos-README.txt`, `files/CosmosEx.command` - the same for the macOS zip

## Windows

The Windows release is a zip, `ce-<version>-windows-x64.zip`: one folder, unzip it anywhere and double-click
`ce_run.exe` (see `files/windows-README.txt`, which goes into the zip as `README.txt`).

- `ce_run.exe` - the launcher, does what `ce_run.sh` + the systemd service do on Linux: creates the dirs from `.env`,
  copies the config drive, runs `ce_cores.exe` + `webserver.exe` with their output in its console window, and stops
  both on Ctrl+C / closing the window, or when one of them exits - except when `ce_cores` fails (exits with error or
  crashes): it's restarted after 3 s, like `Restart=on-failure` of the systemd service, unless it fails more than
  5 times within a minute. Only one instance can run. Source:
  `webserver/cmd/ce_run`
- `.env` - `files/windows.env`: settings, logs and data in `%LOCALAPPDATA%\cosmo2ex`, Windows paths
  (`ce_cores` converts them to Cygwin paths); `BIN_DIR` is the folder of `ce_run.exe`, set by `ce_run.exe`
- `ce_cores.exe` + `cygwin1.dll`, `cygstdc++-6.dll`, `cyggcc_s-seh-1.dll` - `ce_cores` is a Cygwin program; the GCC
  runtime (`libstdc++` / `libgcc`) is a DLL too, because Cygwin's static `libstdc++.a` doesn't link (see
  `ce_cores/makefile`). The DLLs are taken from what `cygcheck` finds.
- `webserver.exe` - native Windows program
- `licenses/` - `cygwin1.dll` is LGPLv3: distributing it with this BSD licensed project is fine, as long as the package
  has the LGPLv3 + GPLv3 texts, a notice that the Cygwin library is used, and the source of exactly the bundled
  `cygwin1.dll` (its Cygwin source package, checked against the mirror's checksum). The GCC runtime DLLs are GPLv3
  with the GCC Runtime Library Exception, which allows distributing them with programs compiled by GCC
- the rest is the same as in the Linux tarballs (web UI, config drive with the Atari programs, floppy images)

The programs have no code signing certificate, so SmartScreen warns on the first start of a downloaded copy - the
README tells the user to click "More info" > "Run anyway" (or to unblock the zip before unzipping). A code signing
certificate would remove that, but costs money and needs a build step to sign with it.

Building takes two steps, as the Atari programs need the m68k-atari-mint toolchain, which is not available in Cygwin:

1. in a Cygwin shell (packages `gcc-g++`, `make`, `curl`, with Go for Windows in `PATH`):
   `release/builder/build_windows.sh <repo dir> <output dir>` - builds the Windows programs and puts the license files
   next to them. CI runs it too and keeps the result as the `windows-ci` artifact.
2. on Linux: `WINDOWS_BIN=<that output dir> release/build.sh windows` - makes the zip (needs `python3`)

The release workflow does both (`windows` job, then `tarballs`).
