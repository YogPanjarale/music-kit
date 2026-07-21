/*
 * Touch Music Kit  ->  Menu-driven synth
 * ------------------------------------------------------------
 * INPUT MODES (how a pad triggers a note):
 *   - Capacitive : ESP32 touchRead() on the 7 pads (default; matches the PCB).
 *   - Digital    : same 7 GPIOs read as active-HIGH digital inputs
 *                  (comparator / LDR / laser-harp / switch outputs).
 *
 * INSTRUMENT TYPES (how a note sounds, all rendered to the DAC):
 *   - String     : plucked Karplus-Strong, rings out and decays.
 *   - Piano      : short, bright plucked variant with an octave harmonic.
 *   - Keyboard   : sustained organ-ish tone; holds while the pad is held,
 *                  releases when let go (only Capacitive/Digital press-hold).
 *   - Percussion : drum-style noise+pitch hit, one-shot.
 *
 * Rotary encoder menu:  GPIO5 = SW   GPIO18 = CLK   GPIO19 = DT
 * Scale buttons:        GPIO16 = DOWN (prev)   GPIO17 = UP (next)
 *   Wire each button between the pin and GND (internal pull-ups are used).
 *
 * Audio renders in small chunks in loop() between scans, so notes ring out
 * continuously without blocking input.
 *
 * Board: ESP32-DevKitC. DAC = GPIO25 -> PAM8403 -> speaker. Serial @ 115200.
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <string.h>

// ============================================================
//  CONFIG
// ============================================================
const int DAC_PIN = 25;

const int NUM = 7;
// Pad order follows the PCB net map S1..S7 (see readme "Firmware Pin Map").
const uint8_t PAD_PIN[NUM] = {
    4,   // S1: GPIO4  / touch T0
    15,  // S2: GPIO15 / touch T3
    13,  // S3: GPIO13 / touch T4
    14,  // S4: GPIO14 / touch T6
    27,  // S5: GPIO27 / touch T7
    33,  // S6: GPIO33 / touch T8
    32   // S7: GPIO32 / touch T9
};

// Capacitive touch detection with hysteresis: touchRead returns LOWER when touched.
const int TOUCH_ON   = 30;   // below this = touched
const int TOUCH_OFF  = 45;   // above this = released
const uint32_t COOLDOWN_MS = 120;   // min time between triggers on the same pad

// Digital input mode: same 7 GPIOs, active-HIGH by default.
const bool DIGITAL_INPUT_PULLDOWNS  = true;
const bool DIGITAL_INPUT_ACTIVE_LOW = false;
uint8_t    lastDigitalActiveMask    = 0xFF; // force one raw GPIO status print at boot

enum PlayMode {
  MODE_CAPACITIVE = 0,   // ESP32 touchRead pads
  MODE_DIGITAL,          // active-HIGH GPIO inputs (comparator / LDR / switch)
  PLAY_MODE_COUNT
};
PlayMode playMode = MODE_CAPACITIVE;

// Scale-select buttons (active LOW, internal pull-up)
const int BTN_PIN[2] = { 16, 17 };   // [0] = DOWN/prev, [1] = UP/next

// Rotary encoder (active LOW switch, internal pull-ups)
const int ENC_SW_PIN  = 5;
const int ENC_CLK_PIN = 18;
const int ENC_DT_PIN  = 19;
const bool ENCODER_REVERSE = false;  // flip if clockwise feels backwards

// 0.91" OLED (SSD1306, 128x32) on I2C: SDA=21, SCL=22, address 0x3C
const int OLED_SDA  = 21;
const int OLED_SCL  = 22;
const int OLED_ADDR = 0x3C;
Adafruit_SSD1306 display(128, 32, &Wire, -1);
bool oledOK = false;

// NeoPixel pin-activity display: one RGB LED per pad on GPIO26 (PCB WS_DATA net).
// LED index matches pad index; active pads stay lit, triggers flash briefly.
const int LED_PIN   = 26;
const int LED_COUNT = NUM;
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
const uint8_t LED_BRIGHTNESS = 70; // global cap (keeps current draw sane)
const uint8_t CAP_ACTIVE_RGB[3] = {   0, 220, 255 }; // capacitive touch active
const uint8_t DIG_ACTIVE_RGB[3] = { 255,  40,   0 }; // digital GPIO active

// LED signal state: active pads stay lit, fresh triggers fade from white.
float    ledLevel[NUM] = { 0 };
bool     ledsDirty     = true;     // forces one redraw after state changes
uint32_t lastFrame     = 0;
const uint32_t FRAME_MS = 33;      // ~30fps, only redraws on changes/flash decay

// ============================================================
//  SCALES  (root + 7 semitone offsets)
// ============================================================
const float ROOT = 261.63f;          // C4 -- change to transpose everything

// 7 semitone steps per scale, relative to ROOT.
// Add a row here and matching names below -- NUM_SCALES updates itself.
const int SCALE_STEPS[][NUM] = {
  {  0,  2,  4,  5,  7,  9, 11 },   // Major (Ionian)
  {  0,  2,  3,  5,  7,  9, 10 },   // Dorian
  {  0,  1,  3,  5,  7,  8, 10 },   // Phrygian
  {  0,  2,  4,  6,  7,  9, 11 },   // Lydian
  {  0,  2,  4,  5,  7,  9, 10 },   // Mixolydian
  {  0,  2,  3,  5,  7,  8, 10 },   // Natural Minor (Aeolian)
  {  0,  1,  3,  5,  6,  8, 10 },   // Locrian
  {  0,  2,  3,  5,  7,  8, 11 },   // Harmonic Minor
  {  0,  2,  4,  7,  9, 12, 14 },   // Major Pentatonic (runs into next octave)
  {  0,  3,  5,  7, 10, 12, 15 },   // Minor Pentatonic (runs into next octave)
  {  0,  3,  5,  6,  7, 10, 12 },   // Blues
  {  0,  2,  4,  6,  8, 10, 12 },   // Whole Tone
  {  0,  1,  4,  5,  7,  8, 10 },   // Phrygian Dominant (Spanish/Arabic)
  {  0,  2,  3,  7,  8, 12, 14 },   // Hirajoshi (Japanese)
};
const char* SCALE_NAME[] = {
  "Major", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Natural Minor",
  "Locrian", "Harmonic Minor", "Major Pentatonic", "Minor Pentatonic",
  "Blues", "Whole Tone", "Phrygian Dominant", "Hirajoshi"
};
const char* SCALE_SHORT_NAME[] = {
  "Major", "Dorian", "Phryg", "Lydian", "Mixolyd", "NatMin",
  "Locrian", "HarmMin", "MajPent", "MinPent", "Blues", "Whole",
  "PhrDom", "Hirajo"
};
const int NUM_SCALES = sizeof(SCALE_STEPS) / sizeof(SCALE_STEPS[0]);

// ============================================================
//  INSTRUMENTS  (voice families)
// ============================================================
enum VoiceKind {
  VK_PLUCK,   // Karplus-Strong pluck (self-decaying)
  VK_TONE,    // sustained additive oscillator with attack/release envelope
  VK_PERC,    // percussive noise + pitched body, one-shot
  VK_FLUTE    // sustained breathy near-sine tone with gentle vibrato
};

// Sustained voices hold while the pad is held and fade on release.
inline bool isSustainedKind(VoiceKind k) { return k == VK_TONE || k == VK_FLUTE; }

struct Instrument {
  const char* name;
  const char* detail;
  VoiceKind   kind;
  // --- pluck params ---
  float decay;
  int   smoothPasses;
  float lifeSec;
  bool  harmonic;      // add an octave-up pluck
  // --- tone params ---
  float attackSec;
  float releaseSec;
  // --- perc params ---
  float percDecaySec;
};

const Instrument INSTRUMENTS[] = {
  // name          detail          kind      decay    sm  life   harm    atk     rel    perc
  { "String",     "Plucked",      VK_PLUCK, 0.9968f, 5,  3.2f,  false,  0.0f,   0.0f,  0.0f  },
  { "Piano",      "Bright pluck", VK_PLUCK, 0.9928f, 1,  1.4f,  true,   0.0f,   0.0f,  0.0f  },
  { "Keyboard",   "Sustained",    VK_TONE,  0.0f,    0,  0.0f,  false,  0.006f, 0.18f, 0.0f  },
  { "Percussion", "Drum hits",    VK_PERC,  0.0f,    0,  0.0f,  false,  0.0f,   0.0f,  0.32f },
  { "Flute",      "Breathy",      VK_FLUTE, 0.0f,    0,  0.0f,  false,  0.040f, 0.12f, 0.0f  },
};
const int NUM_INSTRUMENTS = sizeof(INSTRUMENTS) / sizeof(INSTRUMENTS[0]);

int   scaleIdx      = 0;
int   instrumentIdx = 0;
float noteFreq[NUM];                 // current pad frequencies (built from scale)

enum MenuItem {
  MENU_MODE = 0,
  MENU_INSTRUMENT,
  MENU_SCALE,
  MENU_SCREEN,
  MENU_COUNT
};

int  menuIdx     = MENU_MODE;
bool menuEditing = false;
bool screenFlipped = false;   // true = OLED rotated 180 for viewing from the far side
const char* scaleButtonMsg = "";
uint32_t scaleButtonMsgUntil = 0;

// ---- forward declarations ----
void drawMenu();
void configurePadInputs();
void resetPadStates();
void startPluck(float freq, float decay, int smoothPasses, float lifeSec);

int wrapIndex(int value, int count) {
  while (value < 0) value += count;
  while (value >= count) value -= count;
  return value;
}

void pulseAllLeds(float level = 220.0f) {
  for (int i = 0; i < NUM; i++) ledLevel[i] = level;
  ledsDirty = true;
}

void applyScale() {
  for (int i = 0; i < NUM; i++) {
    noteFreq[i] = ROOT * powf(2.0f, SCALE_STEPS[scaleIdx][i] / 12.0f);
  }
  Serial.printf("scale -> %s\n", SCALE_NAME[scaleIdx]);
  pulseAllLeds();
  drawMenu();
}

void changeScale(int delta) {
  if (delta == 0) return;
  scaleIdx = wrapIndex(scaleIdx + delta, NUM_SCALES);
  applyScale();
}

void changeInstrument(int delta) {
  if (delta == 0) return;
  instrumentIdx = wrapIndex(instrumentIdx + delta, NUM_INSTRUMENTS);
  Serial.printf("voice -> %s\n", INSTRUMENTS[instrumentIdx].name);
  pulseAllLeds();
  drawMenu();
}

const char* playModeName() {
  return playMode == MODE_CAPACITIVE ? "Capacitive" : "Digital";
}

void setPlayMode(PlayMode mode) {
  if (playMode == mode) return;
  playMode = mode;
  configurePadInputs();
  resetPadStates();
  Serial.printf("mode -> %s\n", playModeName());
  drawMenu();
}

void changePlayMode(int delta) {
  if (delta == 0) return;
  setPlayMode((PlayMode)wrapIndex((int)playMode + delta, PLAY_MODE_COUNT));
}

void applyScreenRotation() {
  if (!oledOK) return;
  display.setRotation(screenFlipped ? 2 : 0);   // SSD1306: 2 = 180 degrees
}

void toggleScreenFlip() {
  screenFlipped = !screenFlipped;
  applyScreenRotation();
  Serial.printf("screen -> %s\n", screenFlipped ? "flipped 180" : "normal");
  drawMenu();
}

// ============================================================
//  OLED MENU
// ============================================================
const int OLED_W = 128;
const int OLED_CHAR_COLS = 21;       // 128px / 6px font width at text size 1

void printClipped(const char* text, int maxChars = OLED_CHAR_COLS) {
  for (int i = 0; i < maxChars && text[i] != '\0'; i++) {
    display.write(text[i]);
  }
}

const char* menuLabel() {
  switch (menuIdx) {
    case MENU_MODE:       return "Input";
    case MENU_INSTRUMENT: return "Voice";
    case MENU_SCALE:      return "Scale";
    case MENU_SCREEN:     return "Screen";
    default:              return "Menu";
  }
}

const char* menuValue() {
  switch (menuIdx) {
    case MENU_MODE:       return playModeName();
    case MENU_INSTRUMENT: return INSTRUMENTS[instrumentIdx].name;
    case MENU_SCALE:      return SCALE_SHORT_NAME[scaleIdx];
    case MENU_SCREEN:     return screenFlipped ? "Flipped" : "Normal";
    default:              return "";
  }
}

const char* menuDetail() {
  switch (menuIdx) {
    case MENU_MODE:
      return playMode == MODE_CAPACITIVE ? "Touch pads" : "GPIO inputs";
    case MENU_INSTRUMENT:
      return INSTRUMENTS[instrumentIdx].detail;
    case MENU_SCALE:
      return scaleButtonMsgUntil ? scaleButtonMsg : SCALE_NAME[scaleIdx];
    case MENU_SCREEN:
      return screenFlipped ? "180 view" : "Upright";
    default:
      return "";
  }
}

void drawCenteredText(const char* text, int y, uint8_t size) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  int x = (OLED_W - (int)w) / 2 - x1;
  if (x < 0) x = 0;
  display.setCursor(x, y);
  display.print(text);
}

void drawMenuHeader() {
  char header[24];
  snprintf(header, sizeof(header), "%s %d/%d %s", menuEditing ? "EDIT" : "SEL", menuIdx + 1, MENU_COUNT, menuLabel());

  display.setTextSize(1);
  if (menuEditing) {
    display.fillRect(0, 0, OLED_W, 9, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(0, 0);
  printClipped(header);
  display.setTextColor(SSD1306_WHITE);
}

void drawMenu() {
  if (!oledOK) return;

  display.clearDisplay();
  drawMenuHeader();
  drawCenteredText(menuValue(), 11, 2);
  drawCenteredText(menuDetail(), 25, 1);
  display.display();
}

// ============================================================
//  SYNTH ENGINE  (polyphonic, multi-family)
// ============================================================
const int     SR         = 20000;
const int     MAX_LEN    = 600;
const int     MAX_VOICES = 10;
const uint8_t DC_MID     = 128;

// Sine lookup table (power-of-two length for cheap phase masking).
const int   SINE_N = 256;
float       sineTab[SINE_N];

inline float sineLookup(float cycles) {
  // cycles >= 0; masking handles values >= 1.0 (harmonics).
  int idx = (int)(cycles * SINE_N) & (SINE_N - 1);
  return sineTab[idx];
}

// Shared low-frequency oscillator for flute vibrato.
const float VIB_RATE  = 5.0f;          // Hz
const float VIB_DEPTH = 0.004f;        // +/- pitch modulation (~7 cents)
const float VIB_INC   = VIB_RATE / SR;
float       vibratoPhase = 0.0f;

// Fast noise for percussion (xorshift; avoids per-sample random()).
uint32_t rngState = 0x9E3779B9u;
inline float fastNoise() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return (int32_t)rngState * (1.0f / 2147483648.0f);  // ~ -1..1
}

struct Voice {
  bool      active;
  VoiceKind kind;
  int       pad;        // owning pad for sustained note-off, -1 if none
  // Karplus buffer (pluck)
  float     buf[MAX_LEN];
  int       len, idx;
  float     decay;
  uint32_t  life;
  // Oscillator (tone / perc)
  float     phase;      // cycles [0,1)
  float     phaseInc;   // cycles per sample
  float     env;        // amplitude envelope
  float     env2;       // perc noise-transient envelope
  bool      sustaining; // tone: pad still held
  float     attackRate, releaseRate;
};
Voice voices[MAX_VOICES];
int   nextVoice = 0;
int   padVoice[NUM];    // sustained voice index per pad, -1 = none

void smoothBuffer(float *b, int len, int passes) {
  for (int p = 0; p < passes; p++) {
    float prev = b[len - 1];
    for (int i = 0; i < len; i++) {
      float cur = b[i];
      int n = i + 1;
      if (n >= len) n = 0;
      float nv = (prev + 2.0f * cur + b[n]) * 0.25f;
      prev = cur;
      b[i] = nv;
    }
  }
}

// Pick a free voice, or steal the round-robin one. Clears any pad ownership.
int allocVoice() {
  int v = -1;
  for (int i = 0; i < MAX_VOICES; i++) {
    if (!voices[i].active) { v = i; break; }
  }
  if (v < 0) {
    v = nextVoice;
    nextVoice = (nextVoice + 1) % MAX_VOICES;
  }
  Voice &vc = voices[v];
  if (vc.pad >= 0 && vc.pad < NUM && padVoice[vc.pad] == v) padVoice[vc.pad] = -1;
  vc.pad = -1;
  return v;
}

void startPluck(float freq, float decay = 0.9968f, int smoothPasses = 5, float lifeSec = 3.2f) {
  if (freq <= 0) return;
  int v = allocVoice();
  Voice &vc = voices[v];
  vc.kind = VK_PLUCK;
  vc.len = SR / freq;
  if (vc.len > MAX_LEN) vc.len = MAX_LEN;
  if (vc.len < 2) vc.len = 2;
  for (int i = 0; i < vc.len; i++) {
    vc.buf[i] = (random(0, 2001) / 1000.0f) - 1.0f;
  }
  smoothBuffer(vc.buf, vc.len, smoothPasses);
  vc.idx = 0;
  vc.decay = decay;
  vc.life = (uint32_t)(SR * lifeSec);
  vc.active = true;
}

// Start a sustained voice (Keyboard tone or Flute) owned by a pad.
void startTone(VoiceKind kind, float freq, float attackSec, float releaseSec, int pad) {
  if (freq <= 0) return;
  int v = allocVoice();
  Voice &vc = voices[v];
  vc.kind = kind;
  vc.phase = 0.0f;
  vc.phaseInc = freq / SR;
  vc.env = 0.0f;
  vc.attackRate  = 1.0f / (attackSec  * SR);
  vc.releaseRate = 1.0f / (releaseSec * SR);
  vc.sustaining = true;
  vc.pad = pad;
  vc.life = 0;
  vc.active = true;
  if (pad >= 0 && pad < NUM) padVoice[pad] = v;
}

void startPerc(float freq, float decaySec) {
  if (freq <= 0) return;
  int v = allocVoice();
  Voice &vc = voices[v];
  vc.kind = VK_PERC;
  vc.phase = 0.0f;
  vc.phaseInc = freq / SR;
  vc.env = 1.0f;
  vc.env2 = 1.0f;
  vc.releaseRate = 1.0f / (decaySec * SR);
  vc.pad = -1;
  vc.life = 0;
  vc.active = true;
}

// Trigger the current instrument on a pad press.
void playNote(int pad, float freq) {
  const Instrument &ins = INSTRUMENTS[instrumentIdx];
  switch (ins.kind) {
    case VK_PLUCK:
      startPluck(freq, ins.decay, ins.smoothPasses, ins.lifeSec);
      if (ins.harmonic) startPluck(freq * 2.0f, 0.9900f, 1, 0.35f);
      break;
    case VK_TONE:
    case VK_FLUTE:
      startTone(ins.kind, freq, ins.attackSec, ins.releaseSec, pad);
      break;
    case VK_PERC:
      startPerc(freq, ins.percDecaySec);
      break;
  }
}

// Release a sustained (Keyboard) note when the pad is let go. No-op otherwise.
void noteOff(int pad) {
  int v = padVoice[pad];
  if (v < 0) return;
  padVoice[pad] = -1;
  if (voices[v].active && isSustainedKind(voices[v].kind) && voices[v].pad == pad) {
    voices[v].sustaining = false;
  }
}

inline void deactivate(Voice &vc, int i) {
  vc.active = false;
  if (vc.pad >= 0 && vc.pad < NUM && padVoice[vc.pad] == i) padVoice[vc.pad] = -1;
}

inline float renderSample() {
  // Advance the shared vibrato LFO once per output sample.
  vibratoPhase += VIB_INC;
  if (vibratoPhase >= 1.0f) vibratoPhase -= 1.0f;
  float vibMod = 1.0f + VIB_DEPTH * sineLookup(vibratoPhase);

  float mix = 0.0f;
  for (int i = 0; i < MAX_VOICES; i++) {
    Voice &vc = voices[i];
    if (!vc.active) continue;

    float out = 0.0f;
    if (vc.kind == VK_PLUCK) {
      int n = vc.idx + 1;
      if (n >= vc.len) n = 0;
      out = vc.buf[vc.idx];
      vc.buf[vc.idx] = 0.5f * (vc.buf[vc.idx] + vc.buf[n]) * vc.decay;
      vc.idx = n;
      if (vc.life) vc.life--; else deactivate(vc, i);

    } else if (vc.kind == VK_TONE) {
      // Organ-ish additive tone: fundamental + octave + fifth-above.
      float s = sineLookup(vc.phase)
              + 0.45f * sineLookup(vc.phase * 2.0f)
              + 0.20f * sineLookup(vc.phase * 3.0f);
      s *= 0.5f;                       // headroom for the harmonic sum
      vc.phase += vc.phaseInc;
      if (vc.phase >= 1.0f) vc.phase -= 1.0f;
      if (vc.sustaining) {
        vc.env += vc.attackRate;
        if (vc.env > 1.0f) vc.env = 1.0f;
      } else {
        vc.env -= vc.releaseRate;
        if (vc.env <= 0.0f) { vc.env = 0.0f; deactivate(vc, i); }
      }
      out = s * vc.env;

    } else if (vc.kind == VK_FLUTE) {
      // Near-pure tone (fundamental + faint octave) + breath noise + vibrato.
      float s = sineLookup(vc.phase) + 0.12f * sineLookup(vc.phase * 2.0f);
      s = s * 0.85f + fastNoise() * 0.05f;
      vc.phase += vc.phaseInc * vibMod;
      if (vc.phase >= 1.0f) vc.phase -= 1.0f;
      if (vc.sustaining) {
        vc.env += vc.attackRate;
        if (vc.env > 1.0f) vc.env = 1.0f;
      } else {
        vc.env -= vc.releaseRate;
        if (vc.env <= 0.0f) { vc.env = 0.0f; deactivate(vc, i); }
      }
      out = s * vc.env;

    } else { // VK_PERC
      float body = sineLookup(vc.phase);
      vc.phase += vc.phaseInc;
      if (vc.phase >= 1.0f) vc.phase -= 1.0f;
      vc.phaseInc *= 0.99985f;         // pitch drops -> drum "thump"
      float noise = fastNoise();
      out = 0.6f * body * vc.env + 0.9f * noise * vc.env2;
      vc.env  -= vc.releaseRate;
      vc.env2 *= 0.9988f;              // noise transient decays fast
      if (vc.env <= 0.0f) { vc.env = 0.0f; deactivate(vc, i); }
    }

    mix += out;
  }
  return mix;
}

// ---- Render a small chunk of audio (paced to sample rate) ----
const int CHUNK = 32;          // 32 samples @ 20kHz ~= 1.6 ms between scans
uint32_t audioNext = 0;

inline int shapeSample(float v) {
  v *= 0.5f;
  if (v > 1.0f) v = 1.0f;
  else if (v < -1.0f) v = -1.0f;
  else v = 1.5f * v - 0.5f * v * v * v;   // soft clip
  int d = (int)(v * 120.0f) + DC_MID;
  if (d < 0) d = 0;
  if (d > 255) d = 255;
  return d;
}

void renderChunk() {
  uint32_t periodUs = 1000000UL / SR;
  for (int i = 0; i < CHUNK; i++) {
    dacWrite(DAC_PIN, shapeSample(renderSample()));
    audioNext += periodUs;
    while ((int32_t)(micros() - audioNext) < 0) { }
  }
  if ((int32_t)(micros() - audioNext) > 2000) audioNext = micros();
}

// ============================================================
//  INPUT STATE
// ============================================================
bool     touchHeld[NUM];    // capacitive state after hysteresis
bool     padHeld[NUM];      // selected input mode held state
uint32_t lastTrig[NUM];     // last trigger time (for cooldown)

bool     btnWas[2]  = { false, false };
uint32_t btnLast[2] = { 0, 0 };

int      encLastClk = HIGH;
bool     encSwWas   = false;
uint32_t encSwLast  = 0;

void resetPadStates() {
  for (int i = 0; i < NUM; i++) {
    touchHeld[i] = false;
    padHeld[i] = false;
    // release any sustained notes owned by pads when the mode changes
    if (padVoice[i] >= 0) { noteOff(i); }
    padVoice[i] = -1;
  }
  lastDigitalActiveMask = 0xFF;
  ledsDirty = true;
}

void configurePadInputs() {
  for (int i = 0; i < NUM; i++) {
    if (playMode == MODE_DIGITAL && DIGITAL_INPUT_PULLDOWNS) {
      pinMode(PAD_PIN[i], INPUT_PULLDOWN);
    } else {
      pinMode(PAD_PIN[i], INPUT);
    }
  }
}

bool readDigitalPad(int i) {
  if (playMode != MODE_DIGITAL) return false;
  pinMode(PAD_PIN[i], DIGITAL_INPUT_PULLDOWNS ? INPUT_PULLDOWN : INPUT);
  int level = digitalRead(PAD_PIN[i]);
  return DIGITAL_INPUT_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

void logDigitalActiveMask(uint8_t mask) {
  if (mask == lastDigitalActiveMask) return;
  lastDigitalActiveMask = mask;

  Serial.print("digital GPIO active: ");
  if (mask == 0) {
    Serial.println("none");
    return;
  }
  for (int i = 0; i < NUM; i++) {
    if (mask & (1 << i)) {
      Serial.printf("pad%d(GPIO%d) ", i + 1, PAD_PIN[i]);
    }
  }
  Serial.println();
}

bool readCapacitivePad(int i, int &touchVal) {
  touchVal = touchRead(PAD_PIN[i]);
  if (!touchHeld[i] && touchVal < TOUCH_ON) {
    touchHeld[i] = true;
  } else if (touchHeld[i] && touchVal > TOUCH_OFF) {
    touchHeld[i] = false;
  }
  return touchHeld[i];
}

// ---- Scan pads in the selected mode, edge trigger + cooldown + note-off ----
void scanKeys() {
  uint32_t now = millis();
  uint8_t digitalActiveMask = 0;

  for (int i = 0; i < NUM; i++) {
    int touchVal = -1;
    bool pressed = false;

    if (playMode == MODE_DIGITAL) {
      pressed = readDigitalPad(i);
      if (pressed) digitalActiveMask |= (1 << i);
    } else {
      pressed = readCapacitivePad(i, touchVal);
    }

    bool wasHeld = padHeld[i];
    if (pressed != wasHeld) ledsDirty = true;

    if (!wasHeld && pressed) {
      if (now - lastTrig[i] >= COOLDOWN_MS) {
        playNote(i, noteFreq[i]);
        ledLevel[i] = 255.0f;
        lastTrig[i] = now;
        if (playMode == MODE_DIGITAL) {
          Serial.printf("[%8lu] digital pad%d  %.1f Hz\n", now, i + 1, noteFreq[i]);
        } else {
          Serial.printf("[%8lu] touch pad%d  %.1f Hz  (touch=%d)\n", now, i + 1, noteFreq[i], touchVal);
        }
      }
    } else if (wasHeld && !pressed) {
      noteOff(i);   // release sustained (Keyboard) notes; no-op for others
    }
    padHeld[i] = pressed;
  }

  if (playMode == MODE_DIGITAL) logDigitalActiveMask(digitalActiveMask);
}

void showScaleButtonAction(int buttonIndex) {
  menuIdx = MENU_SCALE;
  menuEditing = false;
  scaleButtonMsg = buttonIndex == 0 ? "GPIO16 DOWN" : "GPIO17 UP";
  scaleButtonMsgUntil = millis() + 900;
}

void updateScaleButtonMessage() {
  if (!scaleButtonMsgUntil) return;
  if ((int32_t)(millis() - scaleButtonMsgUntil) < 0) return;
  scaleButtonMsgUntil = 0;
  if (menuIdx == MENU_SCALE) drawMenu();
}

// ---- Optional scale buttons: cycle scale up/down (debounced, wraps around) ----
void scanButtons() {
  uint32_t now = millis();
  for (int b = 0; b < 2; b++) {
    bool pressed = (digitalRead(BTN_PIN[b]) == LOW);
    if (pressed && !btnWas[b] && (now - btnLast[b]) > 150) {
      showScaleButtonAction(b);
      changeScale((b == 0) ? -1 : +1);
      btnLast[b] = now;
    }
    btnWas[b] = pressed;
  }
}

void changeSelectedMenuValue(int delta) {
  switch (menuIdx) {
    case MENU_INSTRUMENT: changeInstrument(delta); break;
    case MENU_SCALE:      changeScale(delta);      break;
    case MENU_MODE:       changePlayMode(delta);   break;
    case MENU_SCREEN:     toggleScreenFlip();      break;   // two states: any turn flips
    default: break;
  }
}

void handleEncoderTurn(int delta) {
  if (delta == 0) return;
  if (menuEditing) {
    changeSelectedMenuValue(delta);
  } else {
    menuIdx = wrapIndex(menuIdx + delta, MENU_COUNT);
    drawMenu();
  }
}

// ---- Rotary encoder: rotate selects/edits, switch toggles edit mode ----
void scanEncoder() {
  uint32_t now = millis();
  int clk = digitalRead(ENC_CLK_PIN);
  if (clk != encLastClk) {
    if (clk == LOW) {
      int delta = (digitalRead(ENC_DT_PIN) != clk) ? +1 : -1;
      if (ENCODER_REVERSE) delta = -delta;
      handleEncoderTurn(delta);
    }
    encLastClk = clk;
  }

  bool swPressed = (digitalRead(ENC_SW_PIN) == LOW);
  if (swPressed && !encSwWas && (now - encSwLast) > 180) {
    menuEditing = !menuEditing;
    drawMenu();
    encSwLast = now;
  }
  encSwWas = swPressed;
}

// ============================================================
//  NEOPIXEL  (per-pad active signal + brief trigger flash)
// ============================================================
void startupLedAnimation() {
  strip.clear();
  strip.show();
  delay(80);

  for (int pass = 0; pass < 2; pass++) {
    for (int head = 0; head < NUM; head++) {
      strip.clear();
      strip.setPixelColor(head, strip.Color(255, 255, 255));
      if (head > 0) strip.setPixelColor(head - 1, strip.Color(DIG_ACTIVE_RGB[0], DIG_ACTIVE_RGB[1], DIG_ACTIVE_RGB[2]));
      if (head > 1) strip.setPixelColor(head - 2, strip.Color(35, 6, 0));
      strip.show();
      delay(55);
    }
    for (int head = NUM - 2; head >= 0; head--) {
      strip.clear();
      strip.setPixelColor(head, strip.Color(255, 255, 255));
      if (head < NUM - 1) strip.setPixelColor(head + 1, strip.Color(CAP_ACTIVE_RGB[0], CAP_ACTIVE_RGB[1], CAP_ACTIVE_RGB[2]));
      if (head < NUM - 2) strip.setPixelColor(head + 2, strip.Color(0, 18, 22));
      strip.show();
      delay(55);
    }
  }

  for (int pulse = 0; pulse < 2; pulse++) {
    for (int i = 0; i < NUM; i++) strip.setPixelColor(i, strip.Color(255, 255, 255));
    strip.show();
    delay(70);
    strip.clear();
    strip.show();
    delay(90);
  }
  ledsDirty = true;
}

// LED n follows pad n: off = inactive, color = active, white = fresh trigger.
void renderLeds() {
  const uint8_t* activeColor = (playMode == MODE_CAPACITIVE) ? CAP_ACTIVE_RGB : DIG_ACTIVE_RGB;
  for (int i = 0; i < NUM; i++) {
    float flash = ledLevel[i] / 255.0f;
    uint8_t base[3] = { 0, 0, 0 };
    if (padHeld[i]) {
      base[0] = activeColor[0];
      base[1] = activeColor[1];
      base[2] = activeColor[2];
    }

    uint8_t rgb[3];
    for (int c = 0; c < 3; c++) {
      float val = base[c] + (255.0f - base[c]) * flash;
      if (val < 0)   val = 0;
      if (val > 255) val = 255;
      rgb[c] = (uint8_t)val;
    }
    strip.setPixelColor(i, strip.Color(rgb[0], rgb[1], rgb[2]));
  }
  strip.show();
}

// Frame ticker: redraw when pad active states change or trigger flashes decay.
void updateLeds() {
  uint32_t now = millis();
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;
  bool anim = false;
  for (int i = 0; i < NUM; i++) {
    if (ledLevel[i] > 0.0f) {
      ledLevel[i] *= 0.82f;
      if (ledLevel[i] < 4.0f) ledLevel[i] = 0.0f;
      anim = true;
    }
  }
  if (anim || ledsDirty) {
    renderLeds();
    if (!anim) ledsDirty = false;              // stable pin state drawn, go quiet
  }
}

// ---- Boot intro: play the current scale so you know it's alive ----
void introPlayFor(int ms) {
  uint32_t samples = (uint64_t)SR * ms / 1000;
  uint32_t periodUs = 1000000UL / SR;
  uint32_t next = micros();
  for (uint32_t s = 0; s < samples; s++) {
    dacWrite(DAC_PIN, shapeSample(renderSample()));
    next += periodUs;
    while ((int32_t)(micros() - next) < 0) { }
  }
}

void playScaleIntro() {
  Serial.printf("Boot intro: voice=%s scale=%s mode=%s\n",
                INSTRUMENTS[instrumentIdx].name, SCALE_NAME[scaleIdx], playModeName());
  for (int i = 0; i < NUM; i++) {
    startPluck(noteFreq[i]);         // intro always uses a pluck so it self-decays
    for (int k = 0; k < NUM; k++) ledLevel[k] = 0.0f;
    ledLevel[i] = 255.0f;
    renderLeds();                    // walk a white trigger flash across the strip
    introPlayFor(300);
  }
  startPluck(noteFreq[0] * 2.0f);    // root an octave up
  for (int k = 0; k < NUM; k++) ledLevel[k] = 0.0f;
  renderLeds();                      // settle to the current pin state
  ledsDirty = true;
  introPlayFor(1200);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nTouch Music Kit - menu synth (Capacitive/Digital modes)");

  // Build the sine table.
  for (int i = 0; i < SINE_N; i++) {
    sineTab[i] = sinf(2.0f * (float)M_PI * i / SINE_N);
  }

  randomSeed(micros());
  for (int i = 0; i < MAX_VOICES; i++) { voices[i].active = false; voices[i].pad = -1; }
  for (int i = 0; i < NUM; i++) { lastTrig[i] = 0; padVoice[i] = -1; }
  resetPadStates();
  configurePadInputs();

  pinMode(BTN_PIN[0], INPUT_PULLUP);
  pinMode(BTN_PIN[1], INPUT_PULLUP);
  pinMode(ENC_SW_PIN, INPUT_PULLUP);
  pinMode(ENC_CLK_PIN, INPUT_PULLUP);
  pinMode(ENC_DT_PIN, INPUT_PULLUP);
  encLastClk = digitalRead(ENC_CLK_PIN);

  dacWrite(DAC_PIN, DC_MID);

  // ---- OLED init ----
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);                      // fast I2C = short refresh blips
  oledOK = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOK) {
    Serial.println("SSD1306 not found - check wiring / address 0x3C");
  } else {
    applyScreenRotation();
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 8);
    display.print(" Music Kit");
    display.display();
    delay(600);
  }

  // ---- NeoPixel init ----
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.clear();
  strip.show();
  startupLedAnimation();

  applyScale();             // build noteFreq[] and show the starting menu

  Serial.printf("input mode: %s\n", playModeName());
  Serial.print("pad GPIOs: ");
  for (int i = 0; i < NUM; i++) Serial.printf("pad%d=GPIO%d ", i + 1, PAD_PIN[i]);
  Serial.println();
  if (playMode == MODE_CAPACITIVE) {
    Serial.print("resting touch levels: ");
    for (int i = 0; i < NUM; i++) Serial.printf("pad%d=%d ", i + 1, touchRead(PAD_PIN[i]));
    Serial.printf("\n(touch a pad: value should drop well below %d)\n", TOUCH_ON);
    configurePadInputs();
  } else {
    Serial.println("digital mode: comparator/LDR/switch output to pad GPIO; active HIGH with internal pulldowns");
  }
  Serial.println("scale buttons: GPIO16 = down, GPIO17 = up");
  Serial.println("rotary: GPIO5 = switch, GPIO18 = CLK, GPIO19 = DT");
  Serial.printf("NeoPixel pin activity: %d LEDs on GPIO%d\n", LED_COUNT, LED_PIN);

  playScaleIntro();

  audioNext = micros();
  Serial.printf("Ready - %s mode.\n\n", playModeName());
}

void loop() {
  renderChunk();    // ~1.6 ms of audio
  scanKeys();       // trigger notes in Capacitive or Digital mode
  scanButtons();    // optional scale up/down buttons
  scanEncoder();    // OLED menu navigation/editing
  updateScaleButtonMessage();
  updateLeds();     // show active pads and decay trigger flashes
}
