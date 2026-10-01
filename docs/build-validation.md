# Combined firmware build and validation

`heltec_v4_companion_radio_ble` is the firmware environment. It includes BLE
companion services, GY-521/MPU6050 fall detection, SOS, PRG acknowledgement,
public-channel retries, motion-health diagnostics and low-battery reminders.

Run `verify.ps1` from any working directory. It anchors to this project's folder,
builds the combined image and merged flash image, then runs `native` and
`native_delivery`. A C++17-capable GCC compiler is required for the host suites.
See the [Windows setup](../README.md#build-and-test-on-windows).

PlatformIO uses `.pio/core`, `.pio/p`, `.pio/build` and project-local board pins
under `boards/variants/heltec_v4`. The firmware outputs are
`.pio/build/heltec_v4_companion_radio_ble/firmware.bin` and
`.pio/build/heltec_v4_companion_radio_ble/firmware-merged.bin`.

## Current verification

Verified on **2026-09-30** in `C:\Users\usrnm\SeniorHealthNode-MC` by invoking
`verify.ps1` from the sibling directory. Log: `.pio/combined-verification.log`.

| Check | Result |
| --- | --- |
| Combined firmware and merged flash image | Passed |
| RAM / flash usage | 174,272 / 1,273,709 bytes |
| `native` | 194/194 passed |
| `native_delivery` | 22/22 passed |
| Total | **216/216 passed** |
| Linked firmware symbols | BLE initialization, motion polling, SOS/acknowledgement, battery monitoring and packet TX callbacks present in the same ELF |
| Relative documentation links | 115 file links resolved |
| Ignore policy | Generated images/logs ignored; new source, tests and docs remain eligible for Git |

The four companion-button regressions exercise normal navigation, cancellation
of pending UI clicks, triple-click alarm acknowledgement without UI actions, and
held-button suppression. All eight relocated health headers retain their existing
algorithms, thresholds, timers and message encoding.

The obsolete application, build target, dependency cache, source snapshot and
associated generated logs/probes have been removed. Current documentation and
source contain no references to that retired application. Git history is retained.

Windows initially denied launching native test executables; the final full gate
passed with execution permission. No firmware was flashed or changes published.

The host suites exercise production algorithms and message transport with fake
sensor/radio/time boundaries. They do not prove simultaneous BLE and motion
operation on hardware, RF delivery, electrical behavior or fall-detection accuracy.
No firmware is flashed by the verification script.
