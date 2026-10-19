# Cosmo2ex
This is a continuation / evolution of CosmosEx device. It is an ACSI/SCSI hard drive emulator, floppy emulator, IKBD emulator for Atari 16/32 Bit range of computers

# What's what

- ce_cores - this is the network server to which the device connects to
- ce_dd_bootsectors	- ACSI/SCSI boot sectors
- ce_dd_prg	- driver for translated/network drives, date setting and screenshots
- docker - build and run scripts for ce_cores and webserver running in docker container
- release - builds tarballs + install script for installing directly on a Linux machine (systemd service)
- webserver - a configuration interface via browser

- libacsiscsi - library for access to ACSI/SCSI interface via few functions
- libdospath - long-to-short filename conversion library, emulating DOS paths (8.3)

- test_ce	- Atari test tools, all built into a single app (ce_tests.tos) with a menu to pick the test:
  - test_acsi	- Atari ACSI/SCSI transfer tester
  - test_filesystem	- Atari filesystem tester
  - test_floppy	- Atari floppy emulation tester
  - test_floppy_read	- Atari another floppy emulation tester
  - test_ikbd	- Atari IKBD injection chain tester
  - test_manual, test_manual2	- Atari another ACSI/SCSI transfer testers
  - test_rw, test_rw_user	- Atari ACSI write/read verification testers

# Unit tests

`make test` runs all of them (see the top level `Makefile`, CI runs the same in `.github/workflows/ci.yml`).

- Linux: needs `g++`, `make`, `go` and the `libudev-dev` + `libevdev-dev` packages
- Windows: install [Cygwin](https://www.cygwin.com/) with the `gcc-g++` and `make` packages, then in the Cygwin shell:

      make -C libdospath test
      make -C ce_cores test

  Check out the repository with LF line endings (`git config --global core.autocrlf input` before cloning).
- macOS: needs the Xcode command line tools (`xcode-select --install`), then the same two `make` commands as for Windows
