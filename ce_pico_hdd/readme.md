# CosmosEx pico - HDD version (ACSI or SCSI)

- for the merged board devices/v40_pico_hdd - one PCB, assembled either as ACSI or as SCSI variant
- one firmware for both: on boot it reads the variant strap on GP26 (R125 pull-up = ACSI, R225 pull-down = SCSI)
- based on W6100-EVB-Pico (to simplify soldering)
- the older v40 boards (devices/v40_pico_acsi, devices/v40_pico_scsi) have a different pinout, they need ce_pico_hdd_acsi / ce_pico_hdd_scsi

## Status LED (neopixel)

- orange - initializing, brighter with each step: setup started, bus handling running, waiting for ethernet link + DHCP, got IP, looking for / connecting to host
- green (ACSI) / blue (SCSI) - connected to host, the color shows which variant the board runs
  - both colors (and brightness) can be changed on the web Settings page - global for all devices. The host sends them
    (CMD_LED_COLORS, ACSI R G B + SCSI R G B) after the ACSI config on each connect and when changed; the device stores
    them in EEPROM (TSettings.ledAcsi / ledScsi, own check byte - garbage = defaults) only when they differ
- red blinks - error, the count of blinks is the error code, then a longer pause:
  - 1 - ethernet chip not found (halted)
  - 2 - ethernet cable not connected (keeps retrying)
  - 3 - no IP from DHCP (keeps retrying)
  - 4 - PIO setup failed (halted)

The LED gets updated only between commands, when its color changes. ACSI sends the data on GP9
(IC6 gate held open by R151). SCSI sends it on OUT_LE1 (IC6.A) with OUT_LE2 (IC6.B) as the gate - the firmware never has both
H except for the LED update, so the bus activity doesn't get to the LED. On SCSI the device ignores the bus until it's
connected to the host (the Atari doesn't see the drive till then).

## Debug output

The firmware writes debug messages as a serial stream, **115200 baud, 8N1, 3.3V levels**. By default they go to the
LED pin (GP25) of the W6100-EVB-Pico. To use it, open the case and solder a wire to that pin.

### Debug dongle on the keyboard header J1 (no opening, no recompile)

J1 pins: 1 = KEYB_TX_ORIG (from the keyboard), 2 = KEYB_TX (to the Atari), 3 = KEYB_RX (from the Atari), 4 = GND.

1. Unplug the keyboard / Atari keyboard cable from J1.
2. **Short J1 pin 2 with J1 pin 3** (a jumper wire).
3. Connect a 3.3V USB-serial adapter: its **RX to J1 pin 2**, its **GND to J1 pin 4**. Leave its TX unconnected.
4. Open a terminal on the adapter at 115200 8N1 and power the device on (or reset it).

The device checks for the dongle on every boot, as the first step of `setup()` (`debugDongleDetect()` in utils.cpp, takes
~5 ms). It sends 3 bytes on KEYB_TX at the IKBD baud rate (7812) and checks that each bit comes back on KEYB_RX.
The bytes are the key break codes of ESC, SPACE and RETURN, so an Atari that's connected instead of the dongle ignores them.
- **loopback detected**: all the debug output goes to J1 pin 2 at 115200 baud, and the IKBD functionality is off
  (no keyboard / mouse pass-through, no IKBD connection to the host). The first lines say `debug output: KEYB_TX ...`.
- **not detected**: debug output stays on the LED pin, and IKBD works normally.

A real Atari can't trigger it. When powered, it drives KEYB_RX itself. When off, it holds KEYB_RX low. With nothing connected,
KEYB_RX is pulled high by R2. None of those echo the test bytes. Remove the jumper and reset to go back to normal operation.

Compiling with `DEBUG_TO_IKBD 1` (defs.h) forces the KEYB_TX output without the dongle.

## Network for STinG

`sting_net.cpp` answers the HOSTMOD 7 (`HOSTMOD_RAW_NET`) ACSI / SCSI commands of the STinG driver `CE_ETH.STX` itself,
without the host. It sends / receives raw ethernet frames on W6100 socket 0 (MACRAW), with its own MAC address for STinG,
and does ARP for it. The other sockets (HDD, IKBD, UDP) keep working as before. Protocol and details: [ce_sting_stx_driver/README.md](../ce_sting_stx_driver/README.md).

The MiNTnet driver [CE_ETH.XIF](../ce_mintnet_xif_driver/README.md) uses the same commands in frame mode: whole ethernet frames, ARP is done by MiNT.

## Firmware update from the web UI (OTA)

`fw_update.cpp` - uses arduino-pico's `Updater` + `PicoOTA`: the new image goes into a file in LittleFS, the OTA stub
(first 12 kB of every arduino-pico image) copies it over the app on the next boot. A power loss during the copy just
restarts the copy. The BOOTSEL / USB update always stays possible.

- needs the flash size setting with the file system (512 kB) - a firmware built without it reports OTA as not possible
- the image comes over the existing HDD connection: CMD_FW_UPDATE_START / DATA (1 kB chunks, each confirmed with
  ATN_FW_UPDATE_STATUS) / END / ABORT, host side: `ce_cores/chipinterface/fwupdatesender.cpp`
- on END the device refuses the image when its identity (`fwIdent`: `CEFWID:dev=HDD;type=W6100-EVB-PICO;...`) is for
  another device / board, when its flash layout block (FS / EEPROM position at 0x2ff0) differs from the running one,
  or when the MD5 doesn't match
- once per connection the device reports `ATN_FW_INFO` - identity, size + MD5 of the running image, FS position - the
  host stores it in the device's settings dir (`FW_*` keys) for the web UI
- the device type (`FW_DEVICE_TYPE` in fw_update.h) is also in the release file name: `ce_pico_hdd_W6100-EVB-PICO.uf2`
- while the image is written to flash, core 1 is parked (as for the settings) - the drive doesn't answer the ST for
  ~50 ms per 4 kB
- rollback: before the reboot the running image is stored as `fw_prev.bin` + `fw_state.bin` (trial, boot count). The new
  firmware counts its boots in setup(), runs a watchdog (8 s) from the main loop and confirms itself after 10 s connected
  to the host; not confirmed in 3 min = reboot. After 3 unconfirmed boots `fw_prev.bin` goes to the OTA stub - the
  previous firmware runs again and reports it (ATN_FW_INFO flags, web UI shows it). A hang in setup() needs a power cycle
  (that counts as a boot too). A firmware that can't reach the host (e.g. host off) also gets rolled back.

## Compiling under Linux

- from the repo root: `make firmware` (or `firmware/build.sh ce_pico_hdd`), needs just docker - see firmware/README.md
- the result is firmware/out/ce_pico_hdd_W6100-EVB-PICO.uf2

## Compiling under Windows and Arduino env

- install https://github.com/earlephilhower/arduino-pico into Arduino boards via:
> Tools -> Board -> Boards Manager

(The firmware was developed using Raspberry Pi Pico Arduino core version 5.4.2)

- symlink the (patched) Ethernet library from third_party/Ethernet into Arduino libraries dir:

> mklink /D "C:\Users\jookie\Documents\Arduino\libraries\Ethernet" "C:\!miro\hobby\github\cosmo2ex\third_party\Ethernet"

## Arduino env settings

(the Linux build uses the same settings, keep them in sync with FQBN in firmware/compile.sh)

- board: Raspberry Pi Pico
- debug level: None
- debug port: disabled
- C++ exceptions: disabled
- flash size: 2MB (Sketch: 1536KB, FS: 512KB) - the FS holds the new firmware during the update from the web UI, and the previous one for the rollback
- CPU speed: 133 MHz
- IP/Bluetooth stack: IPv4 only
- optimize: Small (-Os)
- Operating system: None
- Profiling: Disabled
- RTTI: Disabled
- Stack protection: Disabled
- Upload method: Picoprobe / Debugprobe (CMSIS-DAP)
- USB stack: No USB

## License

Open design, non-commercial license - see the License section of the [main README](../README.md#license).
