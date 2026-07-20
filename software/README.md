# Touch Music Kit — Firmware

ESP32-DevKitC firmware for the Touch Music Kit PCB. A menu-driven polyphonic
synth: seven pads trigger notes, a rotary encoder drives an OLED menu, and audio
renders to the on-chip DAC (GPIO25) into the PAM8403 amplifier and speaker.

## Build & Flash

Requires [PlatformIO Core](https://platformio.org/install) (`pio`).

```bash
pio run                 # compile
pio run -t upload       # compile + flash over USB
pio device monitor      # serial console @ 115200 baud
pio run -t upload -t monitor   # flash then open the monitor
```

The first `pio run` downloads the ESP32 platform and the three Adafruit
libraries (GFX, SSD1306, NeoPixel), pinned in `platformio.ini`.

## Controls

| Control | GPIO | Action |
| --- | ---: | --- |
| Rotary encoder — rotate | CLK 18 / DT 19 | Move between menu items, or edit the selected value |
| Rotary encoder — press | SW 5 | Toggle **SEL** ⇄ **EDIT** (rotate then edits the value) |
| Button 1 (DOWN) | 16 | Previous scale (jumps the menu to Scale) |
| Button 2 (UP) | 17 | Next scale |
| Pads S1–S7 | see pin map | Play notes in the selected input mode |

Encoder feels backwards? Flip `ENCODER_REVERSE` in `src/main.cpp`.

## Menu

Three items, shown on the 128×32 OLED as `SEL/EDIT  n/3  Label`:

### 1. Input — how a pad triggers a note
- **Capacitive** *(default)* — ESP32 `touchRead()` on the seven pads. This is
  the normal mode for the touch PCB.
- **Digital** — the same seven GPIOs read as active-HIGH digital inputs, for
  comparator / LDR / laser-harp / switch outputs wired into the pad connector.

### 2. Voice — how a note sounds (all rendered to the DAC)
| Voice | Character | Behaviour |
| --- | --- | --- |
| **String** | Plucked, warm | Karplus-Strong; rings out and decays (one-shot) |
| **Piano** | Bright pluck | Shorter pluck + an octave harmonic (one-shot) |
| **Keyboard** | Sustained organ-ish | Sounds while the pad is **held**, releases when let go |
| **Percussion** | Drum hit | Noise burst + pitch-dropping body (one-shot) |

> Only **Keyboard** sustains while held; the other three trigger and decay on
> their own. Note-off is tracked per pad, so releasing a held Keyboard note
> fades it out cleanly.

### 3. Scale — 7 semitone offsets mapped to the pads
14 scales (Major, Dorian, … Hirajoshi). The two buttons also change this
directly. Root note is C4 (`ROOT` in `src/main.cpp`).

## Pin Map

Matches the PCB net map (`../readme.md`).

| Signal | GPIO | Notes |
| --- | ---: | --- |
| Pad S1 | 4 | touch T0 |
| Pad S2 | 15 | touch T3 |
| Pad S3 | 13 | touch T4 |
| Pad S4 | 14 | touch T6 |
| Pad S5 | 27 | touch T7 |
| Pad S6 | 33 | touch T8 |
| Pad S7 | 32 | touch T9 |
| DAC audio out | 25 | → PAM8403 → speaker |
| NeoPixel data | 26 | `WS_DATA`, one LED per pad |
| Button 1 / 2 | 16 / 17 | active-LOW, internal pull-ups |
| Encoder SW / CLK / DT | 5 / 18 / 19 | active-LOW switch, pull-ups |
| OLED SDA / SCL | 21 / 22 | SSD1306 @ I2C 0x3C |

## Tuning

Everything is `const` at the top of `src/main.cpp`:

- **Touch sensitivity** — `TOUCH_ON` / `TOUCH_OFF` (hysteresis; lower = touched).
  Check the "resting touch levels" line printed at boot and set `TOUCH_ON` well
  below the resting value.
- **Digital input polarity** — `DIGITAL_INPUT_ACTIVE_LOW`, `DIGITAL_INPUT_PULLDOWNS`.
- **Retrigger rate** — `COOLDOWN_MS` (min gap between triggers on one pad).
- **Voice character** — the `INSTRUMENTS[]` table: pluck `decay`/`smoothPasses`/
  `lifeSec`, Keyboard `attackSec`/`releaseSec`, Percussion `percDecaySec`. The
  Keyboard harmonic mix lives in `renderSample()` (`VK_TONE` branch).
- **Polyphony / audio** — `MAX_VOICES`, `SR` (sample rate), `LED_BRIGHTNESS`.

Serial (115200) prints every trigger with pad, frequency, and (in Capacitive
mode) the raw touch value — useful for calibrating `TOUCH_ON`/`TOUCH_OFF`.

## Layout

```
software/
├── platformio.ini   # esp32dev / Arduino env + pinned libs
├── src/main.cpp      # firmware
├── old.cpp           # previous single-mode sketch, kept for reference (not built)
└── README.md
```

`old.cpp` sits outside `src/`, so PlatformIO does not compile it.
