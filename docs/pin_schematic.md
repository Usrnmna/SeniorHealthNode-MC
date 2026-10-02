# SeniorHealthNode simple pin schematic

Applies to `heltec_v4_companion_radio_ble`, Heltec V4.3 OLED + external GY-521.
Numbers below are **ESP32 GPIO numbers**, not connector positions or IC package
pin numbers. Arrows show signal direction; `<-->` is bidirectional.
Onboard connections are already wired on the Heltec PCB.

## At-a-glance connection chart

One Heltec runs both MeshCore BLE companion and fall detection. Connections
below use module signal labels, not the left-to-right order of header pins.

```text
                         HELTEC V4 OLED / ESP32-S3
                         ========================
EXTERNAL CONNECTIONS
  GY-521 / MPU6050 VCC  ---------------- 3V3
                  GND  ---------------- GND
                  SDA  ---------------- GPIO4  (Wire1 data)
                  SCL  ---------------- GPIO6  (Wire1 clock)
                  AD0  ---------------- GND    (address 0x68)
                  INT, XDA, XCL -------- no connection

  Passive buzzer driver IN ------------ GPIO47 (PWM control)
                        GND ----------- GND
                        V+ ------------ buzzer-rated supply
                        OUT ----------- passive buzzer (driver-specific)

OPTIONAL EXTERNAL CONNECTIONS
  GPS module TX ----------------------- GPIO38 (ESP32 RX)
             RX ----------------------- GPIO39 (ESP32 TX)
             GND ---------------------- GND
             reset interface ---------- GPIO42 (active LOW)*
             enable interface --------- GPIO34 (active LOW)*
             supply ------------------- module-specific*

  RTC module SDA ---------------------- GPIO17 (Wire data)
             SCL ---------------------- GPIO18 (Wire clock)
             VCC ---------------------- 3V3 (compatible breakout only)
             GND ---------------------- GND

ALREADY CONNECTED ON THE HELTEC PCB
  OLED       SDA / SCL / RESET --------- GPIO17 / GPIO18 / GPIO21
  SX1262     NSS / SCK ----------------- GPIO8 / GPIO9
             MOSI / MISO --------------- GPIO10 / GPIO11
             NRESET / BUSY / DIO1 ------ GPIO12 / GPIO13 / GPIO14
  V4.3 FEM   CSD / CTX ----------------- GPIO2 / GPIO5
  FEM supply enable ------------------- GPIO7
  PRG button -------------------------- GPIO0 (pressed = GND)
  TX LED ------------------------------ GPIO35
  Vext enable ------------------------- GPIO36
  Battery divider enable / ADC -------- GPIO37 / GPIO1

  * GPS connector, supply, and control circuitry require the actual module
    pinout. These are firmware assignments, not verified connector positions.
```

Use the explicit bus assignments above: generic Arduino `SDA`/`SCL` defaults
in `pins_arduino.h` are not this firmware's external sensor connections.

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

```text
OPTIONAL SENSOR BREAKOUT       SDA TO     SCL TO     SUPPLY*  GROUND  ADDRESS
----------------------------  ---------  ---------  -------  ------  -------
AHT10 / AHT20                 GPIO4      GPIO6      3V3      GND     0x38
BME680                        GPIO4      GPIO6      3V3      GND     0x76
BME280                        GPIO4      GPIO6      3V3      GND     0x76
BMP280                        GPIO4      GPIO6      3V3      GND     0x76
BMP085                        GPIO4      GPIO6      3V3      GND     0x77
SHTC3                         GPIO4      GPIO6      3V3      GND     0x70
SHT4X                         GPIO4      GPIO6      3V3      GND     0x44
LPS22HB                       GPIO4      GPIO6      3V3      GND     0x5C
INA3221                       GPIO4      GPIO6      3V3      GND     0x42
INA219                        GPIO4      GPIO6      3V3      GND     0x40
INA260                        GPIO4      GPIO6      3V3      GND     0x41
INA226                        GPIO4      GPIO6      3V3      GND     0x44
MLX90614                      GPIO4      GPIO6      3V3      GND     0x5A
VL53L0X                       GPIO4      GPIO6      3V3      GND     0x29

OPTIONAL RTC BREAKOUT          SDA TO     SCL TO     SUPPLY*  GROUND  ADDRESS
----------------------------  ---------  ---------  -------  ------  -------
DS3231                        GPIO17     GPIO18     3V3      GND     0x68
RV3028                        GPIO17     GPIO18     3V3      GND     0x52
PCF8563                       GPIO17     GPIO18     3V3      GND     0x51
RX8130CE                      GPIO17     GPIO18     3V3      GND     0x32

* Supply column applies only to a breakout accepting 3.3 V power and logic.
  Use its documented power-input label; VIN/VCC/3V3 are not interchangeable
  on every module. Current-monitor supply is separate from measured inputs.
```

Each bus is shared: SDA connections join in parallel, as do SCL connections.
BME680/BME280/BMP280 are alternatives at 0x76; SHT4X and INA226 conflict at
0x44. These rows list supported choices, not a requirement to fit every chip.

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
