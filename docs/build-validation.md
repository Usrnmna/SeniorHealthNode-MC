# Build and path validation

Validated on Windows on 2026-09-28 in `C:\Users\usrnm\SeniorHealthNode-MC`.
This folder has no Git metadata; this review did not commit or publish changes.

## Corrections

- `platformio.ini` uses project-local `.pio/core` and shorter `.pio/p` toolchain
  paths. The previous long package path caused the ESP32 compiler to fail when
  launching its subprocess. Native source filters now name files relative to `src`.
- `boards/heltec_v4.json` explicitly selects `boards/variants/heltec_v4`.
  `pins_arduino.h` moved there without changing its contents. The application board
  drivers remain in `variants/heltec_v4`, compiled through the existing source filter.
  Keeping these separate avoids compiling application drivers as Arduino core sources.
- The motion, fall-response, and battery suites now provide a GoogleTest entry
  point in native mode. Their standalone entry points remain available.
- `merge-bin.py` passes paths as separate process arguments and returns the merge
  process's exit code, so spaces are supported and failures reach PlatformIO.
- `verify.ps1` anchors builds and tests to its own project directory. The README
  and sensor guide describe the local setup and compiler requirement.

## Results

| Check | Result |
| --- | --- |
| `heltec_v4_companion_radio_ble` build and merged image | Passed |
| `heltec_v4_sensor` build and merged image | Passed |
| `pio test -e native` | 70/70 cases passed across all seven suites |
| Standalone C++11 motion/fall-response/battery suites | 18 + 21 + 6 passed with `-Wall -Wextra -Werror` |
| Relative includes and project documentation paths | 61 resolved |
| Arduino pin variant and local OTA library | Resolved inside this workspace |
| Compiler dependency files | 588 scanned; no references to the older `SeniorHealthNode` folder |
| Merge argument boundaries and failure return | Passed with a mocked nonzero process result |
| Full verification invoked from the older sibling folder | Passed; builds/tests used this primary workspace |
| Real merge output paths containing spaces | Passed for both targets; SHA-256 hashes match the default merged images |

The 45 standalone cases are also included in the 70 native cases; they are not
45 additional unique tests. Native suites cover configuration serialization,
fall response, battery reminders, mesh tables, motion detection, UTF-8 helpers,
and hexadecimal formatting.

The local tools are Python 3.13, PlatformIO 6.2.0, Espressif32 platform 6.11.0,
and [WinLibs GCC](https://winlibs.com/) 16.2.0 for native tests. The old MinGW 5.1
compiler was insufficient for GoogleTest 1.17. PlatformIO and the compatible host
compiler are installed under this project's ignored `.venv` and `.pio` folders.
Cached ESP32 packages were copied locally; builds no longer use the older folder.

Run `verify.ps1` from PowerShell to repeat the firmware builds, merged images,
and native suites. See the [setup instructions](../README.md#build-and-test-on-windows)
when preparing a fresh checkout. Logs are in `.pio/verification.log`.
The outside-directory and path-with-spaces check is in `.pio/verification-paths.log`.
Firmware images are in `.pio/build/<environment>/firmware.bin` and
`.pio/build/<environment>/firmware-merged.bin`.

No hardware was flashed. These checks establish buildability and synthetic
software behavior, not physical sensor/buzzer/GPS operation, RF delivery, or
fall-detection accuracy.
