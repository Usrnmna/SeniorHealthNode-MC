# SeniorHealthNode simple pin schematic

Applies to `heltec_v4_sensor`, Heltec V4.3 OLED + external GY-521.
Numbers below are **ESP32 GPIO numbers**, not connector positions or IC package
pin numbers. Arrows show signal direction; `<-->` is bidirectional.
Onboard connections are already wired on the Heltec PCB.

## External motion sensor and buzzer

```text
HELTEC / ESP32-S3                         GY-521 MODULE (MPU6050)
3V3 ------------------------------------ VCC
GND ------------------------------------ GND
GPIO4  (Wire1 SDA) <--------------------> SDA
GPIO6  (Wire1 SCL) ---------------------> SCL
GND ------------------------------------ AD0   [I2C address 0x68]
                                         INT   [unconnected; polling used]
                                         XDA   [unconnected]
                                         XCL   [unconnected]

HELTEC / ESP32-S3                         PASSIVE BUZZER DRIVER
GPIO47 (PWM) --------------------------> IN    [3.3 V logic, HIGH = on]
GND ------------------------------------ GND
Buzzer-rated supply -------------------- V+
                                         OUT -- passive buzzer
                                         (per driver's output wiring)

Discrete NPN alternative:

GPIO47 ----[base resistor]----+---- B
                             |     Q1 NPN
                       [pull-down] E -------- GND
                             |     C -------- buzzer (-)
GND -------------------------+                buzzer (+) -- rated supply
```

Use module header labels for the GY-521, not bare MPU6050 package pads.
All external grounds join Heltec GND; I2C pull-ups must go to 3.3 V.
The existing buzzer guide suggests 1 kohm base and 47 kohm pull-down resistors
as starting values; size the driver for the selected buzzer. An inductive buzzer
requires appropriate flyback protection. GPIO47 supplies a control signal only.

## Onboard display, radio and controls

```text
ESP32-S3                                 OLED (SSD1306 interface)
GPIO17 (Wire SDA) <---------------------> SDA   [I2C address 0x3C]
GPIO18 (Wire SCL) ----------------------> SCL
GPIO21 --------------------------------> RESET

ESP32-S3                                 SX1262 LoRa RADIO
GPIO8  (SPI select) --------------------> NSS
GPIO9  (SPI clock) ---------------------> SCK
GPIO10 (SPI data out) ------------------> MOSI
GPIO11 (SPI data in) <------------------- MISO
GPIO12 --------------------------------> NRESET
GPIO13 <-------------------------------- BUSY
GPIO14 <-------------------------------- DIO1
                                         DIO2 --> onboard RF-switch control
                                         DIO3 --> TCXO supply/control (1.8 V)

ESP32-S3                                 V4.3 RF FRONT END (KCT8103L)
GPIO2 ---------------------------------> CSD   [chip enable]
GPIO5 ---------------------------------> CTX   [TX / RX-gain control]
GPIO7 ---------------------------------> FEM regulator enable

ESP32-S3                                 BOARD CONTROLS
GPIO0  <-------------------------------- PRG button ---- GND when pressed
GPIO35 --------------------------------> TX LED circuit
GPIO36 --------------------------------> Vext power enable [HIGH = on]
GPIO37 --------------------------------> Battery divider enable [HIGH = on]
GPIO1  <-------------------------------- Battery voltage divider output
```

Power and ground for onboard devices are supplied by the PCB. Battery voltage
reaches GPIO1 through the onboard divider, not by a direct battery connection.
RF matching, antenna routing, decoupling and regulator internals are omitted.
The firmware also supports older V4.2 boards: GC1109 replaces KCT8103L, with
GPIO2 to CSD and GPIO46 to CPS; its CTX is controlled by SX1262 DIO2.
These are alternative board versions, not two front-end chips to connect together.

## Optional GPS and RTC

```text
ESP32-S3                                 COMPATIBLE GPS MODULE / INTERFACE
GPIO38 (UART RX) <----------------------- TX
GPIO39 (UART TX) -----------------------> RX
GPIO42 --------------------------------> Reset control [active LOW]
GPIO34 --------------------------------> Power-enable control [active LOW]
GND ------------------------------------ GND
                                         Supply: per actual module/board

ESP32-S3                                 OPTIONAL 3.3 V-COMPATIBLE RTC MODULE
GPIO17 (Wire SDA) <---------------------> SDA
GPIO18 (Wire SCL) ----------------------> SCL
3V3 ------------------------------------ VCC
GND ------------------------------------ GND
```

GPS is enabled in the build, but the source does not identify the fitted module
or its physical connector pin order. Enable/reset may operate board circuitry;
check the actual module before wiring those signals. RTC discovery supports
DS3231 (0x68), RV3028 (0x52), PCF8563 (0x51), or RX8130CE (0x32).
An external RTC is optional; the ESP32 clock is the fallback. DS3231 and MPU6050
can both use 0x68 because they are on separate buses.

## Other sensor chips supported by this build (optional)

These drivers are included, but that does not mean the chips are installed.
For a suitable 3.3 V I2C breakout, the common connections are:

```text
Heltec GPIO4 <--> module SDA     Heltec 3V3 --> module 3.3 V supply input
Heltec GPIO6 ---> module SCL     Heltec GND --- module GND
```

| Supported chip | Address used by this build |
| --- | --- |
| AHT10 / AHT20 | 0x38 |
| BME680, BME280, BMP280 | 0x76 (alternatives at this address) |
| BMP085 | 0x77 |
| SHTC3 | 0x70 |
| SHT4X | 0x44 |
| LPS22HB | 0x5C |
| INA3221 | 0x42 |
| INA219 | 0x40 |
| INA260 | 0x41 |
| INA226 | 0x44 (conflicts with SHT4X) |
| MLX90614 | 0x5A |
| VL53L0X | 0x29 |

Do not connect two devices with the same address to this bus unchanged.
Changing an address strap alone is insufficient if the driver still probes the
address above. Current-sense inputs/shunts, address straps, interface-selection
pins and other module-specific connections require the selected module's pinout.

## Power and charging boundary

The existing project wiring guide identifies the onboard charger as CN3165:
its CHRG output drives the charge LED circuit, with no ESP32 connection.
Charging-status sensing remains disabled (`charging_status_pin = -1`).
No additional charging-status wire is specified here.

This is a firmware-derived connection guide, not a complete PCB netlist of every
power, USB, memory or protection IC. The manufacturer schematic could not be
retrieved during this check; those IC package connections are not inferred.
Hardware wiring and operation have not been physically verified.

Sources in this checkout:

- [Board GPIO definitions](../variants/heltec_v4/platformio.ini)
- [RF front-end control](../variants/heltec_v4/LoRaFEMControl.cpp)
- [Buzzer configuration](../include/HealthNodeConfig.h)
- [Existing wiring and charging guide](gy521_sensor.md)
- [Optional sensor addresses](../src/helpers/sensors/EnvironmentSensorManager.cpp)
- [RTC discovery](../src/helpers/AutoDiscoverRTCClock.cpp)
