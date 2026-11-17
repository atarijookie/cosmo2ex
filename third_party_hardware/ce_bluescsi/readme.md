# CosmosEx HDD on the BlueSCSI v2 board

The [BlueSCSI v2](https://github.com/BlueSCSI/BlueSCSI-v2) board as a Cosmo2ex SCSI hard drive: the disk images live on
the CE host (like with `ce_pico_hdd`), the BlueSCSI's SD card is not used. The host sees it as an HDD device (SCSI).

- **status: proof of concept** - works on a TT over WiFi, but not 100% reliable: a lost packet from the pico costs
  1 - 3 s (lwIP retransmit timer, precompiled in arduino-pico), and the TT driver gives a command 3 s - so some commands
  (mostly big writes) time out and get retried. The W6100 (Ethernet) version is the one meant for real use.
- board: BlueSCSI v2, 2023.09a or newer layout (the `hardware/Desktop_50_Pin` of BlueSCSI-v2 was used as reference)
- pico: **Raspberry Pi Pico 2 W** / **Pico W** (WiFi), or **W6100-EVB-Pico** / **-Pico2** (Ethernet) with the adapter
  (`../evb_to_w_adapter_for_bluescsi`) or 4 wires: the EVB's GP16..GP21 header pins must stay unconnected (the W6100 is
  there), BlueSCSI GP19 -> EVB GP10, GP18 -> GP11, GP20 -> GP12, GP21 -> GP13 - see `board.h`. W6100: written, not
  tested on HW yet
- SD card: not used, leave the slot empty (on the EVB its pins carry SCSI signals). IKBD, neopixel: not on this board
- STinG / MiNTnet raw network (`sting_net.cpp`, a copy of `ce_pico_hdd`'s): W6100 boards only

The firmware is written for this project, not taken from the BlueSCSI firmware (that one is GPLv3) - only the pinout
and the electrical behaviour of the board come from the BlueSCSI schematic. Most of the code is `ce_pico_hdd`'s.

## Builds

One firmware per pico module (the board picked in the Arduino IDE / `firmware/compile.sh` decides, details at the top of
`ce_bluescsi.ino`):

| pico module | Arduino board | release file |
|---|---|---|
| Pico W, W6100-EVB-Pico | Raspberry Pi Pico W | `ce_pico_hdd_BLUESCSI-PICO-W.uf2` - finds out at runtime whether the WiFi chip is there |
| Pico 2 W | Raspberry Pi Pico 2W | `ce_pico_hdd_BLUESCSI-PICO-2W.uf2` |
| W6100-EVB-Pico2 | Raspberry Pi Pico 2 | `ce_pico_hdd_BLUESCSI-W6100-EVB-PICO2.uf2` - the Pico 2W build would hang on the missing WiFi chip |

## WiFi

For now the WiFi network is set at compile time: copy `secrets.example.h` to `secrets.h` and fill it in. `secrets.h` is
in `.gitignore` - never commit it. `firmware/compile.sh` (docker / release builds) deletes it from its copy of the
sources, so those firmwares come without WiFi settings. The captive portal (on the SW1 button) will come later.

## Status

LED of the pico module (Pico W / 2 W only - on the EVB the LED pin carries the debug output):
- fast blinking - starting, joining WiFi, looking for / connecting to the host
- on - connected to the host
- N blinks then a pause - error: 1 = network chip not working (halted), 2 = can't join the WiFi network (retrying),
  3 = no IP from DHCP (retrying), 4 = PIO setup failed (halted), 5 = no WiFi network set - `secrets.h` missing (halted).
  2 on the EVB = no ethernet cable

While not connected to the host the drive answers on its lowest enabled SCSI ID with the boot status sector (as
`ce_pico_hdd`), the Atari shows the WiFi / host state then.

The activity LED of the BlueSCSI board is lit while BSY is asserted.

## Debug output

115200 8N1, connect a 3.3 V USB-UART RX there:
- Pico W / 2 W: the SDA pin of the Qwiic connector J6 (pin 3, GND is pin 1)
- W6100-EVB-Pico / -Pico2: GP25 - the LED pin, not on the header: the LED's resistor (or the LED blinks with the data)

## How it works

- `bridge_scsi.cpp` - the SCSI target on the BlueSCSI board (pins in `board.h`), PIO programs `scsi_read.pio` /
  `scsi_write.pio`. The control outputs of the board are enabled only while we hold BSY; C/D and MSG share their GPIOs
  with the SEL and BSY inputs.
- `net.cpp` - the network behind the Arduino `Client` / `UDP` classes: WiFi there, W6100 in `net_w6100.cpp` (own file -
  the Ethernet library and arduino-pico's WiFi headers clash). The Ethernet library is the patched one from
  `third_party/Ethernet`, as for `ce_pico_hdd`
- `sting_net.cpp` - STinG / MiNTnet raw network on the W6100, copy of `ce_pico_hdd/sting_net.cpp`
- `connection.cpp`, `command_handling.cpp`, `core1_main_loop.cpp`, `boot_status.cpp`, `fw_update.cpp` - from
  `ce_pico_hdd`, SCSI only
- core 1 (`setup1()` / `loop1()`) handles the bus, core 0 the network and host protocol

## Compiling

- Arduino IDE: settings at the top of `ce_bluescsi.ino` (board, flash / FS size, CPU speed per pico module, USB stack
  Pico SDK), the patched Ethernet library linked into the Arduino libraries dir (see `ce_pico_hdd.ino`)
- docker: `firmware/build.sh third_party_hardware/ce_bluescsi` - builds all 3, `firmware/out/ce_pico_hdd_BLUESCSI-*.uf2`
  (the web UI names the HDD firmwares by device type). Not in the default `firmware/build.sh` list yet (untested).
- after changing a `.pio` file: `firmware/pioasm.sh third_party_hardware/ce_bluescsi`

## TODO

- tested on TT (2026-11-17): boots, reads / writes, test_acsi 'D' mostly OK, WiFi timeouts as above. Not tested: bus
  reset, OTA update
- W6100-EVB-Pico / -Pico2: test on HW - detection (Pico W build on the EVB-Pico), DHCP, cable out / in, the same tests
  as on WiFi, STinG
- WiFi: captive portal on SW1 - dropped for now (WiFi not reliable enough), `secrets.h` stays. Maybe STinG over WiFi
