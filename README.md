# Cosmo2ex
This is a continuation / evolution of CosmosEx device. It is an ACSI/SCSI hard drive emulator, floppy emulator, IKBD emulator for Atari 16/32 Bit range of computers

# What's what

- ce_cores - this is the network server to which the device connects to
- ce_dd_bootsectors	- ACSI/SCSI boot sectors
- ce_dd_prg	- driver for translated/network drives, date setting and screenshots
- docker - build and run scripts for ce_cores and webserver running in docker container
- webserver - a configuration interface via browser

- libacsiscsi - library for access to ACSI/SCSI interface via few functions
- libdospath - long-to-short filename conversion library, emulating DOS paths (8.3)

- test_acsi	- Atari ACSI/SCSI transfer tester
- test_filesystem	- Atari filesystem tester
- test_floppy	- Atari floppy emulation tester
- test_floppy_read	- Atari another floppy emulation tester
- test_manual	- Atari another ACSI transfer tester
