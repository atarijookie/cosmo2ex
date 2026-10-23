# FDD front panel - connection and protocol (proposal, v1)

The front panel is a separate small board with its own MCU (RP2040-Zero), display, 3 buttons (or a rotary
encoder) and an optional piezo. The FDD (main board) only sends text and beeps, and receives button events.
The panel handles everything display/button specific (fonts, rendering, debounce, long press, burn-in).

## 1. Cable (FDD <-> panel)

4 wires, keyed connector (e.g. JST-XH 4p) so 5V can't end up on a data pin.

| Pin | Signal | FDD side (Pico 2 W)  | Panel side (RP2040-Zero)      |
|-----|--------|----------------------|-------------------------------|
| 1   | GND    | GND                  | GND                           |
| 2   | 5V     | `5V` net             | -> Schottky -> `5V` pin       |
| 3   | TX     | GP16 (FDD -> panel)  | GP1 (UART0 RX)                |
| 4   | RX     | GP17 (panel -> FDD)  | GP0 (UART0 TX)                |

- data lines are 3.3V on both sides, 5V is power only
- FDD: uart0 is the debug port (GP0/GP1), uart1 the keyboard (GP8/GP9), so the panel UART on GP16/GP17 is done
  via PIO (one TX + one RX state machine)
- FDD: 10k pull-up on GP17 (RX), so a missing panel doesn't produce garbage; the current 2k2 (R20/R21) can stay
  or be replaced by 10k. GP27 (ANALOG_BTNS), R32 and C3 are no longer needed for the panel.

## 2. Panel board (RP2040-Zero, Waveshare pinout)

Only the castellated edge pins are used, not the pads on the bottom (the clones don't always have them).

| RP2040-Zero pin | Function                        | Notes                                                       |
|-----------------|---------------------------------|-------------------------------------------------------------|
| 5V              | power in                        | from cable 5V through a Schottky (e.g. BAT60A, MBR0520), so USB and FDD 5V don't fight |
| GND             | GND                             |                                                             |
| 3V3             | display VCC                     | from the Zero's LDO                                         |
| GP0             | UART0 TX -> FDD                 | 220R in series (cable protection)                           |
| GP1             | UART0 RX <- FDD                 | 220R in series                                              |
| GP4             | I2C0 SDA -> display             | SSD1306 modules usually have pull-ups; add 4k7 pads anyway  |
| GP5             | I2C0 SCL -> display             |                                                             |
| GP26            | BTN_LEFT  / ENC_A               | to GND, internal pull-up, optional 100 nF to GND            |
| GP27            | BTN_ENTER / ENC_SW              | to GND, internal pull-up, optional 100 nF to GND            |
| GP28            | BTN_RIGHT / ENC_B               | to GND, internal pull-up, optional 100 nF to GND            |
| GP29            | strap: input type               | open = 3 buttons, GND = rotary encoder                      |
| GP14            | BEEP (PWM7 A)                   | -> 1k -> NPN (BC847 / MMBT3904) base; piezo between cable 5V and collector |
| GP15            | strap: beeper type              | open = passive piezo (PWM tone), GND = active buzzer (DC on/off) |
| GP9..GP13       | reserved: SPI1 display          | GP10 SCK, GP11 MOSI, GP12 DC, GP13 CS, GP9 RST - for future bigger SPI displays |
| GP16            | onboard WS2812                  | not used in v1                                              |

- Buttons and encoder share pins, the encoder version only differs in the strap on GP29
- Beeper: passive piezo transducer (no built-in generator) is recommended so the frequency can be set; an active
  buzzer (e.g. LPB1475B, 3-14V) also works at 5V, then the frequency is ignored. For a magnetic buzzer add a
  flyback diode.

## 3. Serial settings

- 115200 baud, 8N1, no flow control
- ASCII, one message per line, terminated with `\n` (`\r` is ignored), max 80 bytes including `\n`
- unknown or too long lines are dropped, the receiver just waits for the next `\n` (that's the resync)
- no ACKs - the FDD sends the whole screen again whenever the panel (re)announces itself

## 4. FDD -> panel

| Message                     | Meaning                                                                  |
|-----------------------------|--------------------------------------------------------------------------|
| `?`                         | identify - panel answers with `I...`                                     |
| `C`                         | clear all lines                                                          |
| `T<n><j>[flags]:<text>`     | set line `n` to `text`                                                   |
| `B<ms>[,<hz>]`              | beep for `ms` (1-5000), optional frequency `hz` (100-10000, default 2700); `B0` stops. A new beep replaces the running one. |
| `D<0-255>`                  | display brightness / contrast                                            |
| `!BOOT`                     | reboot the panel into the UF2 bootloader (for updates over its USB)      |

`T` details:

- `n` - line `0`..`3` (lines the panel doesn't have are ignored, see `I`)
- `j` - justify: `L` left, `C` center, `R` right
- `flags` - any combination, any order:
  - `B` - bold
  - `I` - inverted (e.g. selected menu item)
  - `2` - double size; uses lines `n` and `n+1` (while it's shown, line `n+1` isn't drawn)
  - `S` - scroll the text if it's longer than the line (otherwise it's cut off)
- `text` - everything after the first `:` up to `\n`, so it can contain `:`. Chars `0x20`-`0x7E` are shown
  as-is, `0x01`-`0x1F` are reserved for icons (wifi, floppy, ...), unknown ones are shown as a space. An empty
  text clears the line.

Examples:

```
T0C2:COSMO2EX
T2L:A: GAMES1.ST
T3RI:12/40
B50,2000
```

The panel coalesces changes - it redraws ~20 ms after the last command, so a whole screen sent as 4 `T` lines
won't flicker.

## 5. Panel -> FDD

| Message                                 | Meaning                                                    |
|-----------------------------------------|------------------------------------------------------------|
| `I<proto>,<lines>,<cols>,<cols2>,<caps>,<fwver>` | identification; sent on `?` and also on its own after panel boot (= "send me the screen") |
| `K<b><t>`                               | key event                                                  |

`I` fields:

- `proto` - protocol version, `1` for this one
- `lines` - text lines in normal size (e.g. `4` on the 128x32 OLED, `1` on a 12x1 display)
- `cols` / `cols2` - chars per line in normal / double size
- `caps` - capability letters: `B` has beeper, `E` encoder (otherwise buttons), `S` supports scroll, `D` supports
  brightness
- `fwver` - panel firmware version, e.g. `0.1`

Example: `I1,4,25,18,BSD,0.1` (0.91" 128x32 OLED: 4 lines of 25 chars, 18 in double size)

`K` events:

- `b` - button: `L` left, `E` enter, `R` right
- `t` - type: `S` short press, `H` long press (held)

| Input             | Short press         | Long press          |
|-------------------|---------------------|---------------------|
| left button       | `KLS`               | `KLH`               |
| enter button      | `KES`               | `KEH`               |
| right button      | `KRS`               | `KRH`               |
| encoder turn CCW  | `KLS` (per detent)  | -                   |
| encoder turn CW   | `KRS` (per detent)  | -                   |
| encoder push      | `KES`               | `KEH`               |

Timing (panel side, not visible to the FDD):

- debounce 20 ms
- long press = held for 1000 ms; `H` is sent the moment the threshold is reached (not on release), so the user
  gets immediate feedback; the release after that sends nothing
- short press = released before 1000 ms, sent on release
- no auto-repeat in v1

## 6. Startup

1. FDD boots and sends `?`
2. Panel boots and sends `I...` on its own; it also answers `?`
3. Whenever the FDD receives `I...`, it adapts its text to `lines` / `cols` and sends the full screen (`C` + `T`
   lines) again - this covers panel hotplug, panel reset and the FDD booting first or second
4. No `I` received -> no panel, the FDD works without it

## 7. Panel-local behavior

- burn-in: the panel dims the display after a few minutes without a change, and restores it on the next change
  or key press
- the panel never blocks the FDD - all messages are fire-and-forget
