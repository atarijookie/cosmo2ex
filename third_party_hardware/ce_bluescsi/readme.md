# CosmosEx HDD on the BlueSCSI v2 board

The [BlueSCSI v2](https://github.com/BlueSCSI/BlueSCSI-v2) board as a Cosmo2ex SCSI hard drive: the disk images live on
the CE host (like with `ce_pico_hdd`), the BlueSCSI's SD card is not used. The host sees it as an HDD device (SCSI).

- **status: proof of concept** - works on a TT over WiFi, but not 100% reliable: a lost packet from the pico costs
  1 - 3 s (lwIP retransmit timer, precompiled in arduino-pico), and the TT driver gives a command 3 s - so some commands
  (mostly big writes) time out and get retried. The W6100 (Ethernet) version is the one meant for real use.
- board: BlueSCSI v2, 2023.09a or newer layout (the `hardware/Desktop_50_Pin` of BlueSCSI-v2 was used as reference)
- pico: **Raspberry Pi Pico 2 W** (WiFi). Pico W should work too (needs its own build: board "Raspberry Pi Pico W",
  2 MB flash). W6100-EVB-Pico / -Pico2 with 4 rewired pins: planned, see board.h - not implemented yet
- SD card: not used, leave the slot empty. IKBD, neopixel, STinG / MiNTnet raw network: not on this board

The firmware is written for this project, not taken from the BlueSCSI firmware (that one is GPLv3) - only the pinout
and the electrical behaviour of the board come from the BlueSCSI schematic. Most of the code is `ce_pico_hdd`'s.

## WiFi

For now the WiFi network is set at compile time: copy `secrets.example.h` to `secrets.h` and fill it in. `secrets.h` is
in `.gitignore` - never commit it. `firmware/compile.sh` (docker / release builds) deletes it from its copy of the
sources, so those firmwares come without WiFi settings. The captive portal (on the SW1 button) will come later.

## Status

LED of the pico module:
- fast blinking - starting, joining WiFi, looking for / connecting to the host
- on - connected to the host
- N blinks then a pause - error: 1 = network chip not working (halted), 2 = can't join the WiFi network (retrying),
  4 = PIO setup failed (halted), 5 = no WiFi network set - `secrets.h` missing (halted)

While not connected to the host the drive answers on its lowest enabled SCSI ID with the boot status sector (as
`ce_pico_hdd`), the Atari shows the WiFi / host state then.

The activity LED of the BlueSCSI board is lit while BSY is asserted.

## Debug output

115200 8N1 on the SDA pin of the Qwiic connector J6 (pin 3, GND is pin 1) - connect a 3.3 V USB-UART RX there.

## How it works

- `bridge_scsi.cpp` - the SCSI target on the BlueSCSI board (pins in `board.h`), PIO programs `scsi_read.pio` /
  `scsi_write.pio`. The control outputs of the board are enabled only while we hold BSY; C/D and MSG share their GPIOs
  with the SEL and BSY inputs.
- `net.cpp` - the network (WiFi now, W6100 later) behind the Arduino `Client` / `UDP` classes
- `connection.cpp`, `command_handling.cpp`, `core1_main_loop.cpp`, `boot_status.cpp`, `fw_update.cpp` - from
  `ce_pico_hdd`, SCSI only
- core 1 (`setup1()` / `loop1()`) handles the bus, core 0 the network and host protocol

## Compiling

- Arduino IDE: settings at the top of `ce_bluescsi.ino` (Raspberry Pi Pico 2W, 4MB with 2MB FS, 150 MHz, USB stack
  Pico SDK), no extra libraries
- docker: `firmware/build.sh third_party_hardware/ce_bluescsi` - the result is `firmware/out/ce_pico_hdd_BLUESCSI-PICO-2W.uf2`
  (the web UI names the HDD firmwares by device type). Not in the default `firmware/build.sh` list yet (untested).
- after changing a `.pio` file: `firmware/pioasm.sh third_party_hardware/ce_bluescsi`

## TODO

- tested on TT (2026-11-17): boots, reads / writes, test_acsi 'D' mostly OK, WiFi timeouts as above. Not tested: bus
  reset, OTA update
- W6100-EVB-Pico / -Pico2 (next) (W6100 backend in `net.cpp`; RP2350: the Pico 2 W build always starts the WiFi chip, so the
  EVB-Pico2 needs its own build)
- WiFi: captive portal on SW1 - dropped for now (WiFi not reliable enough), `secrets.h` stays. Maybe STinG over WiFi
