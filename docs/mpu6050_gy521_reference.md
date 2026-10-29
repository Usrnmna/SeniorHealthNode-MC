# MPU-6050 and GY-521 source reference

## Archived reference documents

Unmodified copies of the reference PDFs are preserved in this repository:

| Source | Identity and scope |
| --- | --- |
| [MPU-6000.PDF](references/mpu6050/MPU-6000.PDF) | InvenSense, *MPU-6000 and MPU-6050 Register Map and Descriptions*, RM-MPU-6000A-00, revision 4.2, 2013-08-19, 46 pages. Despite the filename, it covers both chips. |
| [GY-521.pdf](references/mpu6050/GY-521.pdf) | *MPU 6050 GY-521 3 Axis Gyro Accelerometer Sensor Module Arduino*, 2 pages. No publisher, revision or release date is identified in the archived sheet. Includes features, an Arduino Uno wiring illustration and a module schematic. |

Page references below use the PDFs' one-based page order. These are hardware
documents, not recorded motion samples, a training dataset, or fall-detection
validation data. The register manual refers to a separate product specification
for electrical limits and self-test acceptance criteria; that specification is
not included here.

## Sensor and module distinction

The MPU-6050 is the sensor IC; GY-521 is a breakout module containing the IC,
power circuitry and connectors. The module sheet lists three accelerometer axes,
three gyroscope axes and 16-bit output. Its claim of accuracy is not a measured
accuracy result for this project; bit depth alone does not establish accuracy.

The register manual, page 5, distinguishes MPU-6050 I2C communication (up to
400 kHz, with a VLOGIC supply reference) from MPU-6000, which also supports SPI.
This project's MPU-6050 driver uses I2C at 100 kHz. No SPI sensor substitution
or firmware-target change follows from the shared manual.

## GY-521 wiring and power

Use the project connections below, also maintained in the
[sensor guide](gy521_sensor.md) and [pin reference](pin_schematic.md).
SeniorHealthNode remains one `heltec_v4_companion_radio_ble` image, with motion
monitoring and BLE companion functionality on the same Heltec node.

| Module signal | Project connection | Purpose |
| --- | --- | --- |
| VCC | Heltec 3V3 | Module input power |
| GND | Heltec GND | Common signal/power ground |
| SDA | GPIO4, Wire1 | Host I2C data |
| SCL | GPIO6, Wire1 | Host I2C clock |
| AD0 | GND | Select 7-bit address 0x68 |
| INT | Unconnected | Driver reads FIFO count/status over I2C; no host interrupt input |
| XDA, XCL | Unconnected | Auxiliary I2C pins; not the host SDA/SCL pair |

GY-521 page 1 advertises a **3-5 V module input** and an onboard LDO. Page 2
labels its input `VCC_5V` and shows a 3.3 V LDO supplying the MPU-6050's VDD and
VLOGIC. R4/R5 are 4.7 kohm SDA/SCL pull-ups to the module's `VCC_3.3V` rail;
R6 is a 4.7 kohm AD0 pull-down. The sheet does not identify the LDO part number.

These module-input labels do not specify 5 V tolerance for the bare IC or Heltec
GPIO. Keep the existing 3V3 connection; do not substitute the Arduino Uno example's
power or GPIO connections. With 3.3 V at the module input, regulator dropout may
lower the sensor rail. Check the fitted module's regulator, actual sensor rail and
I2C pull-up voltage when validating hardware. This sheet alone does not guarantee
the circuitry of every module sold as GY-521 or validate operation at that rail.
Avoid adding parallel pull-ups without checking those already fitted to the bus.

Follow the module's signal labels, not IC package pin numbers or an assumed
header order. The page 2 schematic and photograph depict connectors differently;
they are not a universal connector-position specification. The Arduino example
uses an interrupt wire, but this project's INT pin remains disconnected.

The register manual, page 45, says AD0 changes the address's low bit but does not
change `WHO_AM_I`, which remains `0x68`. AD0 HIGH selects address `0x69`; this
driver currently hardcodes `0x68`, so changing the strap alone is insufficient.

## Current driver configuration and register evidence

Source owner: [`MPU6050.h`](../src/helpers/sensors/MPU6050.h).
Bus setup: [`EnvironmentSensorManager.cpp`](../src/helpers/sensors/EnvironmentSensorManager.cpp).
Timing settings: [`HealthNodeConfig.h`](../include/HealthNodeConfig.h).
The following values describe the current driver configuration.
Register addresses and write values below are hexadecimal unless marked decimal.

| Register | Current operation | Meaning | Register-manual pages |
| --- | --- | --- | --- |
| WHO_AM_I, 0x75 | Read; require 0x68 | Expected identity, independently of AD0 address bit | 45 |
| PWR_MGMT_1, 0x6B | Write 0x01 | Clear sleep, select PLL with X-axis gyro reference | 40-41 |
| PWR_MGMT_2, 0x6C | Write 0x00 | Enable all accelerometer and gyro axes | 42 |
| CONFIG, 0x1A | Write 0x03 | DLPF_CFG=3, FSYNC sampling disabled | 13 |
| SMPLRT_DIV, 0x19 | Write 19 decimal (0x13) | 1000 / (1 + 19) = 50 Hz sensor output rate | 11-12 |
| GYRO_CONFIG, 0x1B | Write 0x08 | FS_SEL=1, +/-500 degrees/s | 14, 31 |
| ACCEL_CONFIG, 0x1C | Write 0x10 | AFS_SEL=2, +/-8 g | 15, 29 |
| INT_PIN_CFG, 0x37 | Write 0x00 | INT_RD_CLEAR=0: clear status only by reading INT_STATUS | 26 |
| INT_ENABLE, 0x38 | Write 0x11 | Enable data-ready and FIFO-overflow status; INT remains unconnected | 27 |
| FIFO_EN, 0x23 | Write 0xF8 | Queue acceleration, temperature and all gyro axes | 16-17 |
| USER_CTRL, 0x6A | Disable/reset FIFO, then write 0x40 | Begin FIFO capture after sensor settling | 38-39 |
| INT_STATUS, 0x3A | Read before/after a FIFO burst | Detect overflow before accepting a batch | 28 |
| FIFO_COUNT_H/L, 0x72-0x73 | Read the count | Determine complete 14-byte records and detect excessive backlog | 43 |
| FIFO_R_W, 0x74 | Read up to 28 bytes | Consume at most two complete records in FIFO order | 44 |

Initialization writes and reads back the configuration registers above,
including FIFO source and control settings. Status, count and data registers are
read-only observations for this driver.
A mismatch in the relevant writable bits or a failed read prevents readiness
and triggers the five-second initialization retry. Reserved bits are ignored.

DLPF_CFG=3 corresponds to accelerometer bandwidth/delay of **44 Hz / 4.9 ms**
and gyro bandwidth/delay of **42 Hz / 4.8 ms** (page 13). Those bandwidths are
not the 50 Hz register-output rate or a guarantee of host sampling timing.

## Measurement format and scale factors

Each 14-byte FIFO record holds seven signed,
two's-complement, 16-bit values, high byte
first: acceleration X/Y/Z, temperature, then gyro X/Y/Z. FIFO ordering follows
the enabled sensor sources (pages 16-17 and 44); pages 29-31 describe the
corresponding sensor register formats. Complete FIFO records determine freshness,
not numerical changes or rereading the latest register cache. A partial trailing
record is not consumed. Identical stationary samples can still be fresh.

| Accelerometer range | AFS_SEL | Counts per g |
| --- | --- | --- |
| +/-2 g | 0 | 16384 |
| +/-4 g | 1 | 8192 |
| **+/-8 g (current)** | **2** | **4096** |
| +/-16 g | 3 | 2048 |

| Gyroscope range | FS_SEL | Counts per degree/s |
| --- | --- | --- |
| +/-250 degrees/s | 0 | 131 |
| **+/-500 degrees/s (current)** | **1** | **65.5** |
| +/-1000 degrees/s | 2 | 32.8 |
| +/-2000 degrees/s | 3 | 16.4 |

The driver therefore divides acceleration counts by **4096** and gyro counts
by **65.5**. Page 30 gives chip temperature in Celsius as
`signed_temperature_count / 340 + 36.53`, matching the driver. This is sensor
chip temperature, not a body-temperature measurement. Acceleration includes
gravity; the driver's vector magnitude is not gravity-subtracted acceleration.
Saved [stationary gyroscope calibration](gyro_calibration.md) subtracts per-axis
zero-rate offsets after scaling. `motion raw` retains uncorrected scaled values;
acceleration and temperature are unchanged. Changing a full-scale register
requires changing the corresponding conversion factor and calibration storage
signature and reassessing detector assumptions.

## Timing, unused capabilities and validation boundary

The driver waits at least 100 ms after successful configuration with FIFO
capture disabled, then resets/enables FIFO. It collects nominal 20 ms samples
in two-record bursts and the companion processes at most four per motion poll.
INT remains unconnected. Chronological acquisition timestamps are estimated from
the FIFO count and 20 ms period. The completed count-read time includes I2C work;
the newest sample is dated one period before that observation. The nominal phase
uncertainty is 20 ms, with additional count-read and clock uncertainty. Capture
and processing times are distinct.

Overflow, failed/partial reads, contradictory timestamps and backlogs beyond
one second explicitly invalidate continuity. Discard diagnostics are lower bounds
where unreadable/overflowed data prevents exact accounting. Three consecutive
read failures trigger reinitialization; failed initialization waits five seconds.
No FIFO output for one second enters the same recovery path. The ESP32 I2C timeout
is 5 ms per transaction, not a whole-loop deadline. See
[acquisition and recovery](fall_detection.md#acquisition-and-sample-timing).

The manual also describes host interrupt acquisition, self-test, DMP and
auxiliary-bus features. Host interrupt timestamps, factory self-test, DMP motion
fusion and an external magnetometer remain unimplemented. The selected filter's
42/44 Hz bandwidth exceeds the 25 Hz Nyquist frequency of the configured output;
it does not by itself establish adequate rejection of vibration or narrow-impact
aliasing. Sampling/filter changes require recorded and physical comparison rather
than assumed accuracy gains. Near-register-limit samples are flagged as saturated.

Physical FIFO behavior, oscillator timing, phase error, bus contention, mounting,
buzzer coupling and simultaneous BLE/RF load remain bench-validation needs.
Software tests exercise the register/FIFO contract using fake devices; they do
not establish actual sensor timing or detection performance.
