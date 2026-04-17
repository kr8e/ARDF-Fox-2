/*This one had the best promise - Matthew
  ============================================================
  "The Simple" ARDF Fox Transmitter  —  ESP32-C3 Fork
  Callsign : KR8E
  Hardware : ESP32-C3 dev board (3.3V logic)
  RF        : Kenwood-compatible handheld via 3.5mm TRS plug
  Cycle     : 30s TX (melody + ID + carrier tail) / 30s silent
  ============================================================

  KENWOOD / BAOFENG UV-5R ACCESSORY JACK PINOUT:
  ───────────────────────────────────────────────
  3.5mm TRS plug (used for mic audio + PTT):
    Tip    = +5V bias from radio     ⚠ DO NOT CONNECT — leave floating
    Ring   = Mic audio IN            ⬅ Connect tone output here
    Sleeve = Ground AND PTT          ⬅ Pull to GND to key the radio
                                       (sleeve IS the PTT line)

  2.5mm TS plug (speaker output — not used by fox):
    Tip    = Speaker audio OUT       — leave unconnected
    Sleeve = Ground                  — leave unconnected

  HOW PTT WORKS ON THIS RADIO:
  ─────────────────────────────
  PTT is activated by connecting the 3.5mm Sleeve to circuit ground
  via an NPN transistor switch. When the NPN is OFF the sleeve
  floats (radio in RX). When the NPN is ON the sleeve is pulled to
  GND and the radio transmits. Mic audio on Ring is active whenever
  PTT is keyed.

  ⚠ The 3.5mm Tip carries +5V from the radio. Never connect it.

  WIRING:
  ───────
  3.5mm Tip    → LEAVE UNCONNECTED (+5V bias from radio)

  3.5mm Ring   ←──[1kΩ]── GPIO5    (mic audio / tone output)
               (opt: 10kΩ trimmer pot in series for deviation trim)

  3.5mm Sleeve ←──────── NPN Collector  (PTT — pulled to GND to TX)
  ESP32 GND    ──────── NPN Emitter
  GPIO4        ──[1kΩ]── NPN Base        (HIGH = TX, LOW = RX)

  NPN: 2N2222 or BC547

  POWER:
    Option A: 9V battery → 5V buck converter → ESP32-C3 5V pin
    Option B: USB power bank → ESP32-C3 USB-C port

  TX SEQUENCE EACH CYCLE:
  ───────────────────────
  1. Key PTT: GPIO4 HIGH → NPN on → 3.5mm Sleeve to GND
  2. Play BeepBox melody (A5→G5→E5→C5→C4→G3, 5×, 150 BPM, ~9s)
  3. 200ms gap
  4. Send "KR8E" in Morse at 1000 Hz, 2x (~2s)
  5. Carrier tail: 1000 Hz fills remainder of 30s window
  6. Unkey PTT: GPIO4 LOW → NPN off → Sleeve floats
  7. 30s silent
  8. Repeat
  ============================================================
*/

#include "driver/ledc.h"

// ── Pin assignments ───────────────────────────────────────
#define PTT_PIN    4    // HIGH = TX → NPN on → 3.5mm Sleeve to GND (PTT)
#define AUDIO_PIN  5    // LEDC PWM tone → 1kΩ → 3.5mm Ring (mic in)
#define LED_PIN    10   // Status LED (on during TX)

// ── LEDC config ───────────────────────────────────────────
#define LEDC_CHANNEL    LEDC_CHANNEL_0
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_SPEED_MODE LEDC_LOW_SPEED_MODE
#define LEDC_RESOLUTION LEDC_TIMER_10_BIT

// ── Timing ────────────────────────────────────────────────
const unsigned long TX_TIME     = 20000UL;
const unsigned long SILENT_TIME = 30000UL;

// ── Morse parameters ──────────────────────────────────────
const int MORSE_FREQ = 1000;
const int DOT_MS     = 80;
const int DASH_MS    = DOT_MS * 3;
const int SYM_GAP    = DOT_MS;
const int CHAR_GAP   = DOT_MS * 3;
const int WORD_GAP   = DOT_MS * 7;

// ── Callsign ──────────────────────────────────────────────
const char* CALLSIGN = "KR8E";

// ── BeepBox melody ────────────────────────────────────────
// 150 BPM, A5→G5→E5→C5→C4→G3 descending, 5 repeats
// {frequency_hz, duration_ms} | freq=0 → rest
const int MELODY[][2] = {
  {880,190},{0,20},{784,190},{0,20},{659,190},{0,20},
  {523,190},{0,220},{262,190},{0,20},{196,190},{0,800},
  {880,190},{0,20},{784,190},{0,20},{659,190},{0,20},
  {523,190},{0,220},{262,190},{0,20},{196,190},{0,800},
  {880,190},{0,20},{784,190},{0,20},{659,190},{0,20},
  {523,190},{0,220},{262,190},{0,20},{196,190},{0,800},
  {880,190},{0,20},{784,190},{0,20},{659,190},{0,20},
  {523,190},{0,220},{262,190},{0,20},{196,190},{0,800},
  {880,190},{0,20},{784,190},{0,20},{659,190},{0,20},
  {523,190},{0,220},{262,190},{0,20},{196,190},{0,400},
  {0,0}
};

// ── Morse lookup ──────────────────────────────────────────
const char* MORSE_ALPHA[] = {
  ".-",   "-...", "-.-.", "-..",  ".",
  "..-.", "--.",  "....", "..",   ".---",
  "-.-",  ".-..", "--",   "-.",   "---",
  ".--.", "--.-", ".-.",  "...",  "-",
  "..-",  "...-", ".--",  "-..-", "-.--",
  "--.."
};
const char* MORSE_DIGIT[] = {
  "-----", ".----", "..---", "...--", "....-",
  ".....", "-....", "--...", "---..", "----."
};

// ── LEDC tone helpers ─────────────────────────────────────
void toneOn(int freqHz) {
  ledc_set_freq(LEDC_SPEED_MODE, LEDC_TIMER, freqHz);
  ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, 512);  // 50% duty
  ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
}

void toneOff() {
  ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL, 0);
  ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL);
}

// ── Radio control ─────────────────────────────────────────
void radioOn()  { digitalWrite(PTT_PIN, HIGH); delay(150); }
void radioOff() { toneOff(); delay(50); digitalWrite(PTT_PIN, LOW); }

// ── Melody ────────────────────────────────────────────────
void playMelody() {
  for (int i = 0; MELODY[i][1] != 0; i++) {
    if (MELODY[i][0] > 0) toneOn(MELODY[i][0]);
    else                  toneOff();
    delay(MELODY[i][1]);
  }
  toneOff();
}

// ── Morse ─────────────────────────────────────────────────
void dit() { toneOn(MORSE_FREQ); delay(DOT_MS);  toneOff(); delay(SYM_GAP); }
void dah() { toneOn(MORSE_FREQ); delay(DASH_MS); toneOff(); delay(SYM_GAP); }

void sendChar(char c) {
  c = toupper(c);
  const char* code = nullptr;
  if      (c >= 'A' && c <= 'Z') code = MORSE_ALPHA[c - 'A'];
  else if (c >= '0' && c <= '9') code = MORSE_DIGIT[c - '0'];
  else if (c == ' ')             { delay(WORD_GAP); return; }
  if (!code) return;
  for (int i = 0; code[i]; i++) { if (code[i] == '.') dit(); else dah(); }
  delay(CHAR_GAP);
}

void sendMorse(const char* msg) { for (int i = 0; msg[i]; i++) sendChar(msg[i]); }

// ── Setup ─────────────────────────────────────────────────
void setup() {
  pinMode(PTT_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(PTT_PIN, LOW);

  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode      = LEDC_SPEED_MODE;
  timer_cfg.duty_resolution = LEDC_RESOLUTION;
  timer_cfg.timer_num       = LEDC_TIMER;
  timer_cfg.freq_hz         = 1000;
  timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&timer_cfg);

  ledc_channel_config_t ch_cfg = {};
  ch_cfg.speed_mode = LEDC_SPEED_MODE;
  ch_cfg.channel    = LEDC_CHANNEL;
  ch_cfg.timer_sel  = LEDC_TIMER;
  ch_cfg.intr_type  = LEDC_INTR_DISABLE;
  ch_cfg.gpio_num   = AUDIO_PIN;
  ch_cfg.duty       = 0;
  ch_cfg.hpoint     = 0;
  ledc_channel_config(&ch_cfg);

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
  }
}

// ── Main loop ─────────────────────────────────────────────
void loop() {
  unsigned long txStart = millis();
  digitalWrite(LED_PIN, HIGH);
  radioOn();
  delay(100);
  playMelody();
  delay(300);
  sendMorse(CALLSIGN);
  delay(300);
  sendMorse(CALLSIGN);
  delay(WORD_GAP);
  unsigned long elapsed = millis() - txStart;
  if (TX_TIME > elapsed + 200UL) {
    toneOn(MORSE_FREQ);
    delay(TX_TIME - elapsed - 200UL);
    toneOff();
  }
  radioOff();
  digitalWrite(LED_PIN, LOW);
  unsigned long txElapsed = millis() - txStart;
  if (txElapsed < TX_TIME) delay(TX_TIME - txElapsed);
  delay(SILENT_TIME);
}
