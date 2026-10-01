#pragma once
#include <cstdint>
#include <cmath>
#include <Stream.h>
using std::isnan;
inline uint32_t g_mock_millis = 0;
inline uint32_t millis() { return g_mock_millis; }
inline uint32_t micros() { return g_mock_millis * 1000; }
inline void delay(uint32_t ms) { g_mock_millis += ms; }
