// board.h — the only file that knows which ATOM this build is for.
//
// agri-temp-wifi started on the M5 ATOM U (ESP32-PICO-D4) and now also builds
// for the M5Stack AtomS3 Lite (ESP32-S3). The two boards differ in exactly
// three places, and all three live here:
//
//                    ATOM U                     AtomS3 Lite
//   status LED       SK6812 on G27, driven      WS2812C on G35, driven by
//                    by M5Atom's M5.dis         FastLED directly
//   button           G39, via M5.Btn            G41, active-low, read directly
//   1-Wire default   G25 (hand-wired, with      G1 (Grove white, built-in
//                    an external 4.7k pull-up)  pull-up on the Grove unit)
//
// Why the S3 build depends on nothing but FastLED: the M5Atom library is
// ESP32-only — it hard-codes the PICO-D4's pins and pulls in an MPU6886 driver
// with no S3 path — so it is left out of the AtomS3 env entirely. The button is
// a bare GPIO and the LED is one WS2812, so neither needs a board library.
//
// The G1 choice is not a guess: agri-temp-poe (AtomS3 Lite + Atomic PoE base,
// three units in the field) verified on hardware that the Grove DS18B20 unit
// answers on G1 — the white wire — and that G2, which the silkscreen's
// yellow/SIG labelling suggests, reads nothing.
//
// Colours are 0xRRGGBB on both boards. The S3's raw WS2812 is far brighter
// than the ATOM U's diffused SK6812, so it is dimmed to a comparable level
// rather than being driven at the literal byte values.

#pragma once

#include <Arduino.h>

#if defined(BOARD_ATOMS3)
  #include <FastLED.h>
#elif defined(BOARD_ATOMU)
  #include <M5Atom.h>
#else
  #error "Define BOARD_ATOMU or BOARD_ATOMS3 in the env's build_flags"
#endif

namespace board {

#if defined(BOARD_ATOMS3)

static const char   *NAME           = "M5Stack AtomS3 Lite";
static const uint8_t LED_PIN        = 35;   // on-board WS2812C
static const uint8_t BTN_PIN        = 41;   // on-board button, active low
static const uint8_t LED_BRIGHTNESS = 40;   // full scale is blinding at 5 cm

inline CRGB &pixelBuf() { static CRGB px[1]; return px[0]; }

inline void begin() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  FastLED.addLeds<WS2812, LED_PIN, GRB>(&pixelBuf(), 1);
  FastLED.setBrightness(LED_BRIGHTNESS);
  pixelBuf() = CRGB::Black;
  FastLED.show();
}

// Nothing to poll: the button is read straight from the pin, and there is no
// IMU or I2C bus to service. Kept so main.cpp stays board-agnostic.
inline void update() {}

inline bool buttonPressed() { return digitalRead(BTN_PIN) == LOW; }

inline void pixel(uint32_t rgb) { pixelBuf() = CRGB(rgb); FastLED.show(); }

#else   // BOARD_ATOMU

static const char *NAME = "M5Stack ATOM U";

inline void begin()             { M5.begin(true, false, true); }  // Serial, no I2C, display
inline void update()            { M5.update(); }
inline bool buttonPressed()     { return M5.Btn.isPressed(); }
inline void pixel(uint32_t rgb) { M5.dis.fillpix(rgb); }

#endif

} // namespace board
