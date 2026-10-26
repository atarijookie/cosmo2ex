# CosmosEx pico - FDD front panel

- RP2040-Zero (Waveshare or its clones - same pinout), 0.91" 128x32 SSD1306 I2C OLED, 3 buttons or a rotary encoder
- connected to the FDD (ce_pico_fdd) with 4 wires: GND, 5V, TX, RX - the FDD sends text, the panel sends button
  events (the beeper is on the FDD board); connections and the protocol are in [protocol.md](protocol.md)
- update: drag & drop `ce_pico_display.uf2` onto the RPI-RP2 drive - hold BOOT while plugging in the USB, or send
  `!BOOT` to the panel (from the FDD, or from a terminal on the panel's USB serial)

## Compiling under Linux

- from the repo root: `make firmware` (or `firmware/build.sh ce_pico_display`), needs just docker - see firmware/README.md
- the result is firmware/out/ce_pico_display.uf2

## Compiling under Windows and Arduino env

- install https://github.com/earlephilhower/arduino-pico into Arduino boards via:
> Tools -> Board -> Boards Manager

(The firmware was developed using Raspberry Pi Pico Arduino core version 5.4.2)

- install the U8g2 library (version 2.36.19) via:
> Tools -> Manage Libraries

- open ce_pico_display/ce_pico_display.ino

## Arduino env settings

(the Linux build uses the same settings, keep them in sync with FQBN_DISPLAY in firmware/compile.sh)

- board: Generic RP2040 - not "Waveshare RP2040 Zero", the clones have various flash chips, the generic slow flash
  access works with all of them
- boot stage 2: Generic SPI /4
- flash size: 2MB (no FS)
- CPU speed: 133 MHz
- debug level: None
- debug port: disabled
- C++ exceptions: disabled
- optimize: Small (-Os)
- Operating system: None
- Profiling: Disabled
- RTTI: Disabled
- Stack protection: Disabled
- USB stack: Pico SDK (USB serial for testing, and the 1200 baud reset into the bootloader for the Arduino upload)
- Upload method: Default (UF2)

## Testing without the FDD

Open a terminal on the panel's USB serial port (any baud rate) and type the messages from protocol.md, e.g.:

```
?
T0C2:COSMO2EX
T2L:A: GAMES1.ST
T3RI:12/40
```

The key events (`KES`, `KLH`, ...) and the `I...` answer show up in the terminal too.

## Notes

- protocol.cpp and keys.cpp are hardware independent and tested on the host - `make -C ce_pico_tests test-display`
- another display: change the U8g2 constructor, the fonts and LINE_HEIGHT in screen.cpp - the FDD adapts to the lines
  and columns the panel reports in its `I` message
- strap (read at start): GP29 to GND = rotary encoder instead of the buttons
- screen orientation: upside down by default (how the encoder panel is mounted); hold enter (button or encoder push)
  for 10 s to flip it - it turns right away and is stored in the emulated EEPROM (last flash sector, settings.cpp).
  The long press at 1 s is still sent to the FDD on the way (starts / leaves the WiFi setup), so a press of enter
  just to change the orientation also toggles that
- encoder direction / clicks: ENC_REVERSE and ENC_STEPS_PER_DETENT in defs.h

## License

Open design, non-commercial license - see the License section of the [main README](../README.md#license).
