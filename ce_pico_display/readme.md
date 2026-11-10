# CosmosEx pico - FDD front panel

- RP2040-Zero (Waveshare or its clones - same pinout), 0.91" 128x32 SSD1306 I2C OLED, 3 buttons or a rotary encoder
- or 2 chained HUB75 LED panels (64x32 P4, 1/16 scan = 128x32) instead of the OLED - devices/pico_display_hub75, the
  same firmware
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

- protocol.cpp, keys.cpp and hub75_frame.cpp are hardware independent and tested on the host -
  `make -C ce_pico_tests test-display`
- another display: change the u8g2 setup in screenInit(), the fonts and LINE_HEIGHT in screen.cpp - the FDD adapts to
  the lines and columns the panel reports in its `I` message
- strap (read at start): GP29 to GND = rotary encoder instead of the buttons
- at start it shows COSMO2EX / waiting for FDD... until the FDD sends its screen
- neopixel on the board (GP16): orange during setup, green once the first screen was drawn. If the colors look
  swapped (green during setup), set NEOPIXEL_RGB in defs.h - some RP2040-Zero boards have an RGB order LED
- screen orientation: upside down by default on the OLED (how the encoder panel is mounted), normal on the LED panels; hold enter (button or encoder push)
  for 10 s to flip it - it turns right away and is stored in the emulated EEPROM (last flash sector, settings.cpp).
  The long press at 1 s is still sent to the FDD on the way (starts / leaves the WiFi setup), so a press of enter
  just to change the orientation also toggles that
- encoder direction / clicks: ENC_REVERSE and ENC_STEPS_PER_DETENT in defs.h

## License

Open design, non-commercial license - see the License section of the [main README](../README.md#license).

## HUB75 LED panels

- found at start: the OLED board has 2k2 pull-ups on SDA (GP4), on the HUB75 board GP4 goes only to a 74HCT245 input -
  with the internal pull-down GP4 reads L there (hub75Detect())
- pins: GP2-7 R1 G1 B1 R2 G2 B2, GP8-12 address A-E (E not connected for 32 row panels), GP13-15 CLK LAT OE - through
  2x 74HCT245 to 5V
- u8g2 draws into its buffer as for the OLED (same fonts, 128x32, the FDD sees no difference), hub75_frame.cpp turns
  it into a word stream (row address, pixels, on time), the PIO program (hub75.pio) shows it, 2 chained DMA channels
  feed it again and again - no CPU time for the refresh; a new screen is built in the other of 2 frames and swapped
- 1 bit per color: HUB75_COLOR in defs.h is the color of the text
- brightness (the `D` command and the dimming) = how long each row is lit, up to HUB75_MAX_ON_CYCLES
- panels: input from the board on panel 1, its output to panel 2; x = 0 is shifted in first, so it ends up on panel 2.
  Text mirrored - HUB75_MIRROR_X in defs.h; the two halves swapped - swap the panels (or the cables)
- until the firmware starts, the panels may show random pixels (OE is L through the pad pull-down), OE goes H first
  thing in hub75Init()
