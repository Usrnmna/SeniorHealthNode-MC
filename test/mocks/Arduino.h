#pragma once

#include <cstdint>
#include <cmath>
#include "Stream.h"

inline uint32_t g_mock_millis = 0;

using std::isnan;

inline uint32_t millis() {
  return g_mock_millis;
}

inline void delay(uint32_t ms) {
  g_mock_millis += ms;
}

#define LOW 0
#define HIGH 1
#define INPUT 0
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
inline int g_mock_button_level = HIGH;
inline int digitalRead(int) { return g_mock_button_level; }
inline int analogRead(int) { return g_mock_button_level; }
inline void pinMode(int, int) {}
