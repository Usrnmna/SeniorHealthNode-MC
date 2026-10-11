# Heltec WiFi LoRa 32 V4 hardware specification reference

## Source and applicability

Primary source: Heltec Automation, `WiFi_LoRa_32_V4.3.1_Datasheet.pdf`, 16 pages.
The revision table on page 2 identifies V4.3.1, dated 2026-06-16. Footers retain
older revision/date labels, including Rev 1.1.0, Rev 1.4 and Rev 4.2.0.
Page references below use the primary PDF's one-based page order unless an
older source is explicitly named.

Secondary source: `WiFi_LoRa_32_V4.2.0.pdf`, 17 pages,
with revision-table date 2025-09-11. It is retained only for explicitly
identified comparisons and details absent from the newer PDF.

Electrical reference: Heltec's separate
[HTIT-WB32LAF V4.3 schematic](https://resource.heltec.cn/download/WiFi_LoRa_32_V4/Schematic/HTIT-WB32LAF_V4.3.pdf),
one sheet, inspected on 2026-10-06. Its battery-divider and RF-control nets
resolve the two datasheet errors below for the ESP32-S3R2 V4.3 board.

This document records manufacturer specifications for project reference, with
source inconsistencies called out below. It does not establish measured behavior
of the assembled SeniorHealthNode.

SeniorHealthNode uses one `heltec_v4_companion_radio_ble` image with BLE companion
functionality and fall detection together. The [project pin guide](pin_schematic.md)
describes the firmware's V4.3 wiring and older V4.2 RF front-end support. The
V4.3.1 PDF is the primary manufacturer reference for this work, but it does not
contain a complete embedded schematic. This reference update does not change
the firmware target, board configuration, or sensor wiring.

## Main specifications

Source: pages 5, 7 and 12, including tables 1.5 and 3.1.

| Item | Manufacturer specification |
| --- | --- |
| MCU | ESP32-S3R2 |
| LoRa transceiver | SX1262 |
| Memory | 384 KB ROM, 512 KB SRAM, 16 KB RTC SRAM, 16 MB external flash, 2 MB PSRAM |
| Wi-Fi | 802.11 b/g/n, up to 150 Mbps |
| Bluetooth | Bluetooth LE, Bluetooth 5 and Bluetooth mesh listed |
| Display | SSD1315, 0.96 inch, 128 x 64 pixels |
| Peripheral resources | 7 ADC1 channels, 2 ADC2 channels, 7 touch channels, 3 UART, 2 I2C, 2 I2S, 4 SPI |
| Connectors | USB-C; SH1.25-2P battery and solar; SH1.25-8P GNSS; two IPEX1.0 antenna connectors for LoRa and 2.4 GHz |
| Headers | Two 18-pin headers and two 2-pin headers, 2.54 mm pitch |
| Temperature | Board: -40 to +85 C; OLED: -40 to +70 C |
| Overall dimensions | 51.7 x 25.4 x 10.7 mm in table 3.1 |

Hardware capabilities do not imply that this firmware enables Wi-Fi, Bluetooth
mesh, or LoRaWAN. Regional compatibility labels are hardware information.

| Model (table 1.5) | Frequency | Maximum TX power |
| --- | --- | --- |
| HTIT-WB32LAF | 868-928 MHz | 28 +/- 1 dBm |

Table 3.5.1 on page 14 gives the high-frequency range as **863-928 MHz**,
whereas table 1.5 starts at **868 MHz**. Preserve that source difference when
selecting a specific radio variant. Table 1.5 now lists only HTIT-WB32LAF,
but table 3.5.1 still includes HTIT-WB32LAF-N at 433/470-510/863-928 MHz,
21 +/- 1 dBm. Table 3.1 also retains 433-510 MHz and 863-928 MHz ranges.
These tables do not establish that every frequency variant is available for
V4.3.1. The older V4.2 table separately lists -N-HF, -N-LF and -N-433 variants.

The V4.3.1 general specification gives maximum receive sensitivity of -137 dBm,
without bandwidth or spreading-factor conditions. Its contents page mentions
a sensitivity section, but the supplied PDF has no detailed sensitivity table.
For comparison only, the **older V4.2 PDF**, table 3.5.2 on page 14, lists
typical sensitivity at 125 kHz: -137 dBm at SF12, -131 dBm at SF10 and
-122 dBm at SF7. These detailed conditions are not restated in V4.3.1.

The V4.3.1 variant description lists EU868, IN865, US915, AU915, AS923 and
KR920 compatibility, but contains no detailed LoRaWAN frequency-plan table.
For comparison only, **older V4.2** table 3.6 (page 14) lists region ranges:
IN868 (as printed), 865-867 MHz;
EU868, 863-870; US915, 902-928; AU915, 915-928; KR920, 920-923;
AS923, 920-925; CN470, 470-510. The older page 6 calls the Indian region IN865.
These labels are recorded from the source, not selected project radio settings.

## Power inputs and outputs

Source: page 13, tables 3.2 and 3.3. GPIO36 HIGH enables Ve, matching the
firmware's `PIN_VEXT_EN_ACTIVE=HIGH` definition.

| Input | Minimum | Typical | Maximum | Listed source-current requirement |
| --- | --- | --- | --- | --- |
| USB-C | 4.7 V | 5 V | 6 V | At least 500 mA |
| Lithium battery | 3.3 V | 3.7 V | 4.2 V | At least 250 mA |
| 5V header | 4.7 V | 5 V | 6 V | At least 500 mA |
| 3V3 header | 2.7 V | 3.3 V | 3.5 V | At least 150 mA |
| Solar | 4.7 V | 5 V | 6 V | Not specified in table |

The PDF says to connect only one of USB and the 5V input; solar and battery
may coexist with the selected USB/5V input. Table 3.3 lists a maximum of
500 mA for each of 3.3V, 5V@USB and Ve. This is not a statement that all three
can supply 500 mA simultaneously. The input-current figures also do not cover
every transmit operating point: the PDF lists 750 mA for 27 dBm TX below.

## Typical current consumption

Source: page 14, table 3.4, at 25 C. These are manufacturer figures, not measured
SeniorHealthNode consumption or battery-life estimates.

| Mode and condition | Typical current |
| --- | --- |
| Wi-Fi scan, USB | 119 mA |
| Wi-Fi AP, USB | 170 mA |
| Bluetooth, USB | 115 mA |
| 868 MHz TX, 14 dBm, USB | 290 mA |
| 868 MHz TX, 17 dBm, USB | 330 mA |
| 868 MHz TX, 22 dBm, USB | 400 mA |
| 868 MHz TX, 27 dBm, USB | 750 mA |
| Receive enabled, TX disabled | 75 mA |
| Sleep, USB | 2 mA |
| Sleep, battery or 3.3V header | 20 microamps |
| Charging, USB at 5 V | 500 mA |

Page 5 advertises sleep below 20 microamps; table 3.4 specifies 20 microamps
typical. Neither figure describes continuous BLE companion and motion monitoring.

## Header positions and onboard signals

Source: pages 8-11, tables 2.2.1-2.2.3, with V4.3 FEM roles corrected from the
separate schematic. Header positions below are distinct from
ESP32 GPIO numbers. Use the page 8 orientation diagram before locating pins.
Alternative ESP32 IOMUX functions do not mean an onboard-connected pin is free.

| Position | J2 | J3 |
| --- | --- | --- |
| 1 | GND | GND |
| 2 | 5V | 3V3 |
| 3 | Ve | 3V3 |
| 4 | Ve | GPIO37, ADC control |
| 5 | GPIO44, UART0 RX | GPIO46; not V4.3 KCT8103L CTX |
| 6 | GPIO43, UART0 TX | GPIO45 |
| 7 | RST / CHIP_PU | GPIO42, GNSS reset |
| 8 | GPIO0, PRG button | GPIO41, GNSS PPS |
| 9 | GPIO36, Vext control | GPIO40, GNSS wakeup |
| 10 | GPIO35, LED | GPIO39, GNSS TX |
| 11 | GPIO34, GNSS control in pin diagram | GPIO38, GNSS RX |
| 12 | GPIO33 | GPIO1, battery ADC |
| 13 | GPIO47 | GPIO2, V4.3 KCT8103L CSD |
| 14 | GPIO48 | GPIO3 |
| 15 | GPIO26 | GPIO4 |
| 16 | GPIO21, OLED reset | GPIO5, V4.3 KCT8103L CTX; reserved onboard |
| 17 | GPIO20, USB D+ | GPIO6 |
| 18 | GPIO19, USB D- | GPIO7, FEM supply control |

Additional pins: GPIO18 is OLED SCL, GPIO17 is OLED SDA, GPIO16 is
XTAL_32K_N and GPIO15 is XTAL_32K_P (table 2.2.3).

The page 8 radio diagram assigns SX1262 NSS/SCK/MOSI/MISO/reset/BUSY/DIO1
to GPIO8/9/10/11/12/13/14 respectively. Its GNSS diagram lists GND,
GPIO34 control, 3V3, GPIO39 TX, GPIO38 RX, GPIO40 wakeup, GPIO41 PPS and
GPIO42 reset. These are board-side labels; verify connector orientation and
the module's TX/RX direction before attaching a module.

For the project's GY-521 and buzzer assignments, use the
[firmware-derived pin guide](pin_schematic.md); the manufacturer PDF does not
specify SeniorHealthNode's external motion-sensor or buzzer wiring.

## Corrected battery equation and RF control pins

Use the separate official V4.3 schematic for these connections. The source
datasheet retains errors; the corrected project interpretation is:

| Item | Correct V4.3 meaning | Schematic evidence |
| --- | --- | --- |
| Battery divider | `VADC = VBAT * 100 / 490`; `VBAT = VADC * 4.9` | R27 = 390 kohm above ADC_IN; R28 = 100 kohm to GND |
| Battery ADC | GPIO1 reads ADC_IN; GPIO37 HIGH enables the divider | U7 GPIO1 / ADC_IN and GPIO37 / ADC_Ctrl, Q5/Q6 switching |
| FEM chip enable | GPIO2 = KCT8103L CSD | U7 GPIO2 / PA_CSD to U8 CSD |
| FEM transmit/receive control | GPIO5 = KCT8103L CTX | U7 GPIO5 / PA_CTX to U8 CTX |
| Older-board control | GPIO46 CPS is for V4.2 GC1109 | Older V4.2 schematic and the separate GC1109 firmware branch |

The V4.3.1 page 10 formula incorrectly puts the attenuation factor on the
battery side: `VBAT = 100 / (100+390) * VADC_IN1`. The correct relationship
gives **3.92 V battery for 0.80 V at the ADC pin**, before resistor tolerances
and measurement errors. These are physical voltages, not raw ADC counts.

The page 8 `FEM_PA` label on GPIO46 must not be applied to V4.3 KCT8103L CTX.
Table 2.2.2 omits the GPIO5 FEM role. The firmware already uses the schematic's
GPIO2/5 assignments, with GPIO2/46 retained only for older GC1109 boards.
See [board definitions](../variants/heltec_v4/platformio.ini) and
[FEM control](../variants/heltec_v4/LoRaFEMControl.cpp). GPIO5 is not free for an
external sensor on the V4.3 board, even though the datasheet table omits its role.

### Battery conversion in the firmware

[`HeltecV4Board`](../variants/heltec_v4/HeltecV4Board.cpp) averages eight raw
10-bit `analogRead()` counts with the divider enabled. Its existing conversion is:

```text
battery_mV = raw_average * (3.3 / 1024.0) * adc_multiplier * 1000
default adc_multiplier = 5.42
```

The factor **5.42** is the existing effective raw-count scale, combining the
divider and ADC correction in this approximate conversion. It is not a resistor
ratio or a verified calibration for an individual board. `analogRead()` returns
uncalibrated counts, not measured ADC-pin volts, so the nominal `3.3 / 1024`
conversion and the physical divider ratio must not be conflated.

The firmware preserves the existing default and meaning of `adc.multiplier`: changing only
5.42 to 4.9 would reduce the reported voltage by about 9.6%, affecting battery
warnings and shutdown. A calibrated ADC-pin millivolt reading would instead use
the physical multiplier 4.9. The current firmware does not use that API.

For a board-specific adjustment, compare a stable, nonzero reported battery
voltage with a meter at the battery terminals under the same conditions:
`new_multiplier = current_multiplier * measured_voltage / reported_voltage`.
Use the [USB battery diagnostics and calibration workflow](cli_commands.md#battery-diagnostics-and-calibration-usb).
`battery` shows cached raw counts and their age; `battery calibrate <meter_mV>`
suggests a multiplier without applying it. `set adc.multiplier <value>` saves
and applies an explicit selection. Older companion preferences without this
setting use the board default; `set adc.multiplier 0` restores that selection.
Check multiple battery voltages; a single scaling adjustment does not prove
ADC accuracy over its range. Staged acquisition timing and warning thresholds
are unchanged.

## Other source differences and dimensions

The V4.3.1 PDF has no embedded schematic. Its contents page has broken bookmark
references for schematic and frequency-plan sections, page 6 is otherwise blank,
and page 16 offers a schematic resource link. The separate official V4.3
schematic linked above was retrieved and inspected for the battery and FEM
corrections. The **older V4.2 PDF**, page 15, contains the V4.2
schematic, including ESP32-S3R2, SX1262, GC1109 RF front end, CN3165 charger,
power switching, battery divider, USB, flash, OLED and GNSS connections.
That older schematic is not evidence of the complete V4.3 netlist.

- **Vext enable polarity resolved in the newer text:** V4.3.1 page 13 says
  GPIO36 HIGH enables Ve. The current
  [board configuration](../variants/heltec_v4/platformio.ini) defines
  `PIN_VEXT_EN_ACTIVE=HIGH`, so the documentation now agrees. The older V4.2
  page 12 said LOW. This is documentary agreement, not a physical polarity test.
- **Display naming:** table 3.1 identifies SSD1315. The project pin guide calls
  its software interface SSD1306; the controller specification and driver
  interface name should remain distinguishable.
- **Mechanical envelope:** page 15 shows 51.69 mm length, 25.40 mm width and
  2.54 mm header pitch, with a 15.60 mm total vertical dimension in the side
  view including the projecting header pins. Table 3.1's 10.7 mm dimension
  should not be used as the complete installed header clearance.

The primary document's manufacturer resource links are on page 16. No physical wiring,
power draw, RF output, board revision, or enclosure fit was verified during this
reference work. Schematic verification establishes documented connections,
not physical testing or calibration of an individual board.
