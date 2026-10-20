# CosmosEx pico - SCSI version

- based on W6100-EVB-Pico (to simplify soldering)

## Compiling under Linux

- from the repo root: `make firmware` (or `firmware/build.sh ce_pico_hdd_scsi`), needs just docker - see firmware/README.md
- the result is firmware/out/ce_pico_hdd_scsi.uf2

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
