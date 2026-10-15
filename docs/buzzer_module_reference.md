# DIYables passive buzzer module reference

## Product identification and published data

The hardware assumption is the **Passive, 5 pieces** selection of the
[Amazon listing for ASIN B0DHGP95K4](https://www.amazon.com/Buzzer-Module-Arduino-ESP8266-Raspberry/dp/B0DHGP95K4?th=1),
inspected on 2026-10-14. The listing also offers an Active selection; the
passive selection is the basis of this configuration.

| Listing field | Published information |
| --- | --- |
| Brand and manufacturer | DIYables |
| Item model number | DIY-BUZZER-MODULE-PASSIVE-5 |
| ASIN | B0DHGP95K4 |
| Type and quantity | Passive piezoelectric buzzer module; 5 pieces |
| Supply | 3.3–5 V; title describes a 5 V module |
| Pitch control | Variable input frequency |
| Listed platforms | Arduino, ESP32, ESP8266 and Raspberry Pi |
| Package dimensions | 5.75 × 3.43 × 0.71 inches |
| Listed item weight | 0.634 ounces |
| Date first available | September 19, 2024 |

Package dimensions do not specify individual PCB dimensions. The listing's
low-power description is qualitative; it supplies no numerical current rating.

The [manufacturer product page](https://diyables.io/products/passive-buzzer-module)
specifies 3.3–5 V DC, PWM pitch control, VCC/GND/signal connections and mounting
holes. The [ESP32-S3 tutorial](https://newbiely.com/tutorials/esp32-s3/esp32-s3-piezo-buzzer)
describes the onboard driver and tone/noTone operation. These sources do not
establish a numerical sound-pressure rating or a guaranteed silent input polarity.

## Firmware connections and operation

| Module terminal | Heltec V4 connection |
| --- | --- |
| VCC | 3V3 |
| GND | GND, shared with the Heltec |
| I/O (signal) | GPIO47 |

Connect by terminal labels, rather than assuming pin order from a photograph.
The onboard driver powers the transducer; GPIO47 supplies its control waveform.
The tutorial's GPIO42 example does not replace GPIO47 in this firmware.

[`HealthNodeConfig.h`](../include/HealthNodeConfig.h) contains the compiled
settings: `buzzer_enabled = true`, `buzzer_active_low = false`, GPIO47,
LEDC channel 0 and 2000 Hz for ordinary beeps and SOS. A passive module requires
a square wave for sustained sound; a constant input can produce only a click
or no audible output. The firmware uses 50% duty during tones, disconnects PWM
during silence and restores LOW with the default polarity setting.

Each tone attaches the pin before writing its sounding duty. In the pinned
Arduino ESP32 2.x implementation, attachment configures the channel using
hardware duty readback. Writing sounding duty afterward prevents an older
readback value from replacing that request. See the
[Arduino LEDC implementation](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/esp32-hal-ledc.c)
and [ESP32-S3 duty readback implementation](https://github.com/espressif/esp-idf/blob/v4.4.7/components/hal/esp32s3/include/hal/ledc_ll.h).

Playback remains nonblocking. Ordinary beeps last 150 ms. SOS uses 150 ms dots,
450 ms dashes and a 5-second pause between sequences. Low battery playback uses
2093, 1760 and 1480 Hz tones, each lasting 450 ms with 150 ms gaps. Idle polling
performs no GPIO or PWM writes. No buzzer connection or acoustic feedback is
required for fall sensing or channel alerts.

The default LOW idle level is a firmware configuration, not a manufacturer
polarity guarantee. `buzzer_active_low = true` instead selects HIGH during
silence; both settings use 50% duty during sound. An incorrect idle setting can
keep the module's driver energized between tones, potentially increasing idle
current or heating even without sustained sound. See the
[polarity and optional-output guide](gy521_sensor.md#buzzer-trigger-polarity).
Changing compiled settings requires rebuilding and flashing the combined image.

## Verification

After flashing, run USB `beep` and check for a short, clear tone followed by
silence. A scope or logic analyzer on GPIO47 should show approximately 2000 Hz
at 50% duty during the beep and LOW afterward with the default configuration.
Check VCC and common ground if the result is only clicking or weak rattling.
A successful command confirms software acceptance, not audible output.

Host tests exercise both idle polarities, stale duty readback during attachment,
pitch changes, SOS priority, disabled output and idle polling. They do not
verify physical waveform quality, module polarity, sound level or supply current.
Use the [USB debugging guide](usb_serial_debugging.md) for connection procedures
and the [sensor/buzzer guide](gy521_sensor.md) for complete alarm behavior.
