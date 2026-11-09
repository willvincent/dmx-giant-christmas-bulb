#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------- Pins
constexpr uint8_t PIN_NRF_CE   = 4;
constexpr uint8_t PIN_NRF_CSN  = 5;
constexpr uint8_t PIN_NRF_IRQ  = 27;  // active low on RX; saves SPI polling
// SPI (VSPI default): SCK=18, MISO=19, MOSI=23
constexpr uint8_t PIN_OLED_SDA = 21;
constexpr uint8_t PIN_OLED_SCL = 22;
constexpr uint8_t PIN_LED_DATA = 25;  // -> level shifter -> strip DIN
constexpr uint8_t PIN_BTN_UP   = 32;  // buttons to GND, internal pull-ups
constexpr uint8_t PIN_BTN_DOWN = 33;
constexpr uint8_t PIN_BTN_MODE = 13;

// ---------------------------------------------------------------- LEDs
constexpr uint16_t NUM_PIXELS     = 72;
#define LED_COLOR_ORDER (NEO_GRBW + NEO_KHZ800)  // SK6812 RGBW is normally GRBW
// Total LED current budget. Supply is 5V/3A shared with ESP32 + radio + OLED
// (~0.15A), so 2650mA leaves headroom. All pixels are driven identically.
constexpr uint32_t LED_MAX_MA     = 2650;
constexpr float    MA_PER_CHANNEL = 20.0f;  // SK6812: ~20mA per channel at 255

// ---------------------------------------------------------------- Signal loss
constexpr uint32_t SIGNAL_HOLD_MS = 5000;   // hold last DMX frame this long...
constexpr uint32_t SIGNAL_FADE_MS = 1000;   // ...then fade to black over this long
constexpr uint32_t LOCK_LOST_MS   = 500;    // no packets for this long -> rescan

// ---------------------------------------------------------------- Radio
constexpr uint32_t SCAN_DWELL_MS  = 8;      // listen time per RF channel while scanning

// ---------------------------------------------------------------- UI / power
constexpr uint32_t OLED_TIMEOUT_MS = 15000; // display off after idle; any button wakes
constexpr uint8_t  OLED_CONTRAST   = 75;    // 0-255, lower = less power
constexpr uint8_t  CPU_MHZ         = 80;
constexpr uint32_t SAVE_DELAY_MS   = 2000;  // flash write after settings stop changing

// ---------------------------------------------------------------- Color mode timing (ms)
constexpr uint32_t FADE_SLOW_MS = 4000;     // per color transition
constexpr uint32_t FADE_FAST_MS = 1000;
constexpr uint32_t STEP_SLOW_MS = 2500;     // time each color is shown
constexpr uint32_t STEP_FAST_MS = 600;
