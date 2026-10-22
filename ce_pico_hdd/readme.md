# CosmosEx pico - HDD version (ACSI or SCSI)

- for the merged board devices/v40_pico_hdd - one PCB, assembled either as ACSI or as SCSI variant
- one firmware for both: on boot it reads the variant strap on GP26 (R125 pull-up = ACSI, R225 pull-down = SCSI)
- based on W6100-EVB-Pico (to simplify soldering)
- the older v40 boards (devices/v40_pico_acsi, devices/v40_pico_scsi) have a different pinout, they need ce_pico_hdd_acsi / ce_pico_hdd_scsi

## Status LED (neopixel)

- orange - initializing, brighter with each step: setup started, bus handling running, waiting for ethernet link + DHCP, got IP, looking for / connecting to host
- green - connected to host
- red blinks - error, the count of blinks is the error code, then a longer pause:
  - 1 - ethernet chip not found (halted)
  - 2 - ethernet cable not connected (keeps retrying)
  - 3 - no IP from DHCP (keeps retrying)
  - 4 - PIO setup failed (halted)

The LED gets updated only between commands (on SCSI its pin is shared with RST / REQ / C/D). During init / error it's also refreshed
once per second, once running the green is sent just once - so the bus is not disturbed by the LED while running.

## Compiling under Linux

- from the repo root: `make firmware` (or `firmware/build.sh ce_pico_hdd`), needs just docker - see firmware/README.md
- the result is firmware/out/ce_pico_hdd.uf2

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
- flash size: 2MB (no FS)
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
