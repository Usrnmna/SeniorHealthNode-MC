# SeniorHealthNode

**MeshCore-based motion monitoring and mesh alerts for people who may be unable to reach a phone.**

SeniorHealthNode explores how MeshCore's decentralized radio mesh can help older adults and people with disabilities call attention to a possible need for help. A wearable or nearby node can notice a significant movement event and send an alert through a MeshCore network to registered contacts, including in places where a phone is out of reach or conventional connectivity is unavailable. The aim is to give people and caregivers another way to stay connected while supporting greater independence.

This repository is based on **MeshCore 1.17.0**. It retains the normal MeshCore radio, identity, telemetry, and contact-based messaging foundations. The current motion-monitoring firmware is a **sensor node** that works with a separate MeshCore companion; it is not itself a modified BLE companion app or a one-device replacement for a caregiver's companion.

The GY-521/MPU6050 motion sensor measures acceleration and rotation so the sensor node can flag a sudden movement that might accompany a fall, collision, or other accident. When the example rule crosses its configured threshold, the firmware queues a high-priority alert to permitted MeshCore contacts, giving caregivers a chance to check on the person even when a phone is out of reach. This is an indication of significant motion, not a determination that a fall or crash occurred; ordinary activity can trigger it, and an accident can go undetected.

Future firmware could use the existing motion readings and telemetry to recognize patterns such as a strong impact followed by prolonged stillness, distinguish routine movement from likely falls or crashes, and allow sensitivity to be tailored to the wearer or mounting location. It could also add inactivity alerts, a manual help button, or richer event histories for caregivers. These features are possibilities for further development and validation; they have not been programmed into the current sensor firmware.

## Supported builds in this folder

`SeniorHealthNode-MC` contains two firmware targets for the Heltec V4 OLED board:

- `heltec_v4_companion_radio_ble`: MeshCore BLE companion, including its existing display, contacts, messaging, GPS, and environmental telemetry support.
- `heltec_v4_sensor`: the modified GY-521/MPU6050 motion-monitoring node, including telemetry, encrypted contact alerts, serial commands, and its existing update support.

Build from this folder with `pio run -e heltec_v4_companion_radio_ble` or `pio run -e heltec_v4_sensor`. A plain `pio run` builds the BLE companion. The `native` environment retains the shared software tests, including the motion-rule tests.

Other boards, TFT targets, USB/Wi-Fi companion targets, repeaters, room servers, terminal chat, and KISS modem builds have been removed. Shared firmware and enabled sensor drivers are retained so the two supported targets keep their existing behavior. The two targets remain separate firmware images; motion alerts are implemented in the sensor target.

> [!IMPORTANT]
> This is a prototype, not a validated fall detector, medical device, or guaranteed emergency service. An alert can be missed because of sensor placement, radio coverage, power, configuration, or an unavailable recipient. Use an established emergency plan for situations where a missed alert could cause harm.

## How it fits into MeshCore

| Part | Role |
| --- | --- |
| `heltec_v4_sensor` | Heltec V4.3 OLED sensor firmware that reads a GY-521/MPU6050, publishes telemetry, and sends motion alerts. |
| MeshCore companion | A regular BLE, USB, or Wi-Fi companion used to discover the sensor, configure access, and receive messages. The `heltec_v4_companion_radio_ble` target still builds the standard companion firmware. |
| MeshCore network | Compatible nodes provide the radio path between the sensor and a registered recipient, subject to normal network coverage and radio settings. |

The sensor sends encrypted direct alerts through MeshCore's existing contact, permission, acknowledgment, and retry logic. Recipients must be registered and permitted to receive high-priority alerts. The motion events are not posted to a public channel.

## Changes from the MeshCore 1.17.0 BLE companion release

The health-related additions are built into the **sensor** target. They do not change the BLE companion's client interface or add motion detection to the companion firmware.

| Added or changed code | What it does |
| --- | --- |
| [`src/helpers/sensors/MPU6050.h`](src/helpers/sensors/MPU6050.h) | Adds a GY-521/MPU6050 driver on `Wire1` at I²C address `0x68`. It checks the device ID, reads acceleration, rotation, and chip temperature, and retries after a failed read. |
| [`src/helpers/sensors/EnvironmentSensorManager.cpp`](src/helpers/sensors/EnvironmentSensorManager.cpp) and [`.h`](src/helpers/sensors/EnvironmentSensorManager.h) | Make the motion sensor available to the sensor firmware and add its latest valid readings to environmental telemetry. |
| [`examples/simple_sensor/MotionRule.h`](examples/simple_sensor/MotionRule.h) | Adds an example rule for significant acceleration or rotation, with a cooldown and a quiet period before rearming. |
| [`examples/simple_sensor/main.cpp`](examples/simple_sensor/main.cpp) | Calls `MyMesh::pollMotion()` from the sensor loop, evaluates fresh readings, queues a high-priority alert through `alertIf()`, and adds the serial `motion` command for inspecting readings. |
| [`examples/simple_sensor/SensorMesh.h`](examples/simple_sensor/SensorMesh.h) | Adds `isAlertPending()` so the rule waits for an earlier alert task before evaluating another event. |
| [`variants/heltec_v4/platformio.ini`](variants/heltec_v4/platformio.ini) | Enables the MPU6050 for `heltec_v4_sensor` and assigns its I²C pins to GPIO4 and GPIO6. The OLED/RTC bus remains separate. |
| [`docs/gy521_sensor.md`](docs/gy521_sensor.md) and [`test/test_motion_rule/test_motion_rule.cpp`](test/test_motion_rule/test_motion_rule.cpp) | Document wiring, configuration, and checks for the motion rule. |

The default example rule triggers when acceleration magnitude reaches **2.5 g** or rotation magnitude reaches **250°/s**. These thresholds are demonstration settings, not a confirmed fall-detection algorithm. The device polls approximately every 20 ms when its main loop can run; very brief events may be missed. The sensor's temperature reading is the **chip temperature**, not a person's body temperature. See the [sensor guide](docs/gy521_sensor.md) for thresholds, cooldown behavior, and telemetry details.

## Getting started

1. Use a **Heltec V4.3 OLED** board and connect a GY-521: `VCC → 3V3`, `GND → GND`, `SDA → GPIO4`, `SCL → GPIO6`, and `AD0 → GND` for address `0x68`.
2. Set radio parameters to match your MeshCore network. Build and upload the sensor firmware with PlatformIO:

   ```powershell
   pio run -e heltec_v4_sensor
   pio run -e heltec_v4_sensor -t upload --upload-port COM_PORT
   ```

3. Use a separate MeshCore companion to discover the sensor and register the contacts who should receive alerts. Change the sensor's default admin password (`password`) during setup, and grant recipients the high-priority alert permission.
4. Use a serial monitor at **115200 baud** and enter `motion` to inspect the latest sensor reading. Test alert delivery with the intended recipient and at the intended location before relying on the setup.

Replace `COM_PORT` with your board's port. The [GY-521 sensor guide](docs/gy521_sensor.md) has the full wiring table, recipient setup, commands, and validation steps. A serial `Motion alert` message means an alert was queued; verify that the recipient actually receives it.

## Project status

A prior local validation built the `heltec_v4_sensor` firmware and passed the motion-rule checks. The board was **not** flashed as part of that validation, and live sensor readings, radio coverage, and recipient delivery have **not** been verified. There is currently no dedicated panic button, inactivity detector, caregiver dashboard, or automatic call to emergency services in this repository.

## MeshCore resources and license

SeniorHealthNode builds on [MeshCore](https://github.com/ripplebiz/MeshCore) and its [documentation](https://docs.meshcore.io). For the base companion, repeater, and client features, refer to the upstream project. The repository retains the [MIT license](license.txt).
