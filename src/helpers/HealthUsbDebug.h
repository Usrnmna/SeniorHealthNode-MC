#pragma once

// Optional companion diagnostic sink. Formatting and queue admission are bounded;
// the health/main and BLE callback tasks never wait for USB transmission.
void healthUsbDebug(const char* format, ...);
