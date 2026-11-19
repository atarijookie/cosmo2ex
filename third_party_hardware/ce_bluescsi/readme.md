# CosmosEx HDD on the BlueSCSI v2 board

The [BlueSCSI v2](https://github.com/BlueSCSI/BlueSCSI-v2) board as a Cosmo2ex SCSI hard drive: the disk images live on
the CE host (like with `ce_pico_hdd`), the BlueSCSI's SD card is not used. The host sees it as an HDD device (SCSI).

- **status: works** - tested on a TT (2026-11-19, W6100-EVB-Pico2): 200 MB read + write (test_acsi) without a failure,
  1585 kB/s read
- board: BlueSCSI v2, 2023.09a or newer layout (the `hardware/Desktop_50_Pin` of BlueSCSI-v2 was used as reference)
- pico: **W6100-EVB-Pico** or **W6100-EVB-Pico2** (Ethernet) on the adapter `../evb_to_w_adapter_for_bluescsi`: the
  EVB's GP16..GP21 (the W6100) don't reach the BlueSCSI, BlueSCSI GP19 / GP18 / GP20 / GP21 go to EVB GP10 / GP11 / GP12 /
  GP13, BlueSCSI GP16 (Qwiic SDA) to EVB GP15 (debug output) - see `board.h`
- Pico W / Pico 2 W (WiFi): **not supported** - tried, a lost WiFi packet costs 1 - 3 s (lwIP retransmit timer) and the
  TT driver gives a command 3 s, so commands (mostly big writes) time out too often
- SD card: not used, leave the slot empty (its pins carry SCSI signals on the EVB). IKBD, neopixel: not on this board
- STinG / MiNTnet raw network: `sting_net.cpp`, a copy of `ce_pico_hdd`'s

The firmware is written for this project, not taken from the BlueSCSI firmware (that one is GPLv3) - only the pinout
and the electrical behaviour of the board come from the BlueSCSI schematic. Most of the code is `ce_pico_hdd`'s.

## Builds

One firmware per pico module (the board picked in the Arduino IDE / `firmware/compile.sh` decides, details at the top of
`ce_bluescsi.ino`):

| pico module | Arduino board | release file |
|---|---|---|
| W6100-EVB-Pico | Raspberry Pi Pico | `ce_pico_hdd_BLUESCSI-W6100-EVB-PICO.uf2` |
| W6100-EVB-Pico2 | Raspberry Pi Pico 2 | `ce_pico_hdd_BLUESCSI-W6100-EVB-PICO2.uf2` |

## Status

LED of the EVB (GP25):
- fast blinking - starting, waiting for IP, looking for / connecting to the host
- on - connected to the host
- N blinks then a pause - error: 1 = W6100 not found (halted), 2 = no ethernet cable, 3 = no IP from DHCP (retrying),
  4 = PIO setup failed (halted)

While not connected to the host the drive answers on its lowest enabled SCSI ID with the boot status sector (as
`ce_pico_hdd`), the Atari shows the network / host state then.

The activity LED of the BlueSCSI board is lit while BSY is asserted.

## Debug output

115200 8N1 on the SDA pin of the BlueSCSI's Qwiic connector J6 (pin 3, GND is pin 1) - EVB GP15 through the adapter.
Connect a 3.3 V USB-UART RX there.

## How it works

- `bridge_scsi.cpp` - the SCSI target on the BlueSCSI board (pins in `board.h`), PIO programs `scsi_read.pio` /
  `scsi_write.pio`. The control outputs of the board are enabled only while we hold BSY; C/D and MSG share their GPIOs
  with the SEL and BSY inputs.
- `net.cpp` - the W6100 (DHCP, link) behind the Arduino `Client` / `UDP` classes, the patched Ethernet library from
  `third_party/Ethernet`, as for `ce_pico_hdd`
- `sting_net.cpp` - STinG / MiNTnet raw network (W6100 MACRAW), copy of `ce_pico_hdd/sting_net.cpp`
- `connection.cpp`, `command_handling.cpp`, `core1_main_loop.cpp`, `boot_status.cpp`, `fw_update.cpp` - from
  `ce_pico_hdd`, SCSI only. The bridge waits up to 2.5 s for the host's answer (`CMD_TIMEOUT_HOST`), and drops the late
  answer of a command it gave up on instead of sending it to the next one.
- core 1 (`setup1()` / `loop1()`) handles the bus, core 0 the network and host protocol

## Compiling

- Arduino IDE: settings at the top of `ce_bluescsi.ino` (board, flash / FS size, CPU speed per pico module, USB stack
  No USB), the patched Ethernet library linked into the Arduino libraries dir (see `ce_pico_hdd.ino`)
- docker: `firmware/build.sh third_party_hardware/ce_bluescsi` - builds both, `firmware/out/ce_pico_hdd_BLUESCSI-*.uf2`
  (the web UI names the HDD firmwares by device type). In the default `firmware/build.sh` list - `make firmware`, CI and
  the releases build both.
- after changing a `.pio` file: `firmware/pioasm.sh third_party_hardware/ce_bluescsi`

## TODO

- not tested on HW yet: W6100-EVB-Pico (RP2040) build, cable out / in, bus reset, OTA update, STinG
