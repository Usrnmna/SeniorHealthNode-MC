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

## Fall-detection research

The seven supplied PDFs contain **five distinct studies**. File comparisons confirmed that `fall_detect_paper7.pdf` duplicates paper 1 and `fall_detect_paper3.pdf` duplicates paper 2. The bibliography below maps every supplied filename to its study. These studies provide research context and possible development directions; their algorithms and reported results are **not implemented or validated in SeniorHealthNode**.

### Findings from the supplied studies

**Smartphone orientation and voting across multiple rules.** Astriani et al. [1](#study-1) combined five threshold-based methods (AGVeSR, Alim, alpha-angle, GyroReDi, and AGPeak), declaring a fall when at least three agreed. They collected 252 activity records (84 falls and 168 non-falls) in six phone orientations from adults aged 25-35, with simulated falls onto a mattress. Table V reports 76.67% accuracy for acceleration alone, 91.67% for AGVeSR, and 95.82% for the five-method vote. Jumping and sitting/standing transitions remained sources of confusion. The paper describes both a 180/72 training/testing split and leave-one-out validation, so its evaluation protocol should not be assumed to be an independently replicated benchmark. For this project, the useful direction is to evaluate orientation changes and agreement between several motion features rather than assume one mounting orientation will work for everyone. See PDF pp. 4-6.

**Posture plus the preceding motion transition.** Li et al. [2](#study-2) used two sensor nodes, on the chest and thigh, sampled at 120 Hz. Their algorithm identifies a static posture and examines the preceding five seconds of acceleration and angular velocity when the posture is classified as lying. In experiments involving three men in their twenties, they reported 91% sensitivity from 70 fall records and 92% specificity from 72 non-fall records. Falling against a wall into a seated position could be missed; quickly lying down could trigger false alarms. This supports investigating temporal context and posture, but the two-body-location method cannot be reproduced directly by SeniorHealthNode's single MPU6050. See PDF sections 3-4, pp. 2-6.

**Learning from sequences does not guarantee better detection.** Wisesa and Mahardika [3](#study-3) trained a long short-term memory (LSTM) recurrent neural network on waist-sensor data from UMAFall. They retained 617 valid recordings, randomly dividing them into 494 training and 123 validation samples. Their X-axis accelerometer model's prediction table identifies all 38 falls and misclassifies 2 of 85 daily activities as falls. In contrast, their combined accelerometer/gyroscope model misses 31 of 38 falls and falsely flags 10 daily activities (Tables 3-4). The reported peak validation accuracy of 100% for the single-axis model is distinct from those saved-model prediction results. For SeniorHealthNode, an LSTM is an experimental alternative requiring suitable data, evaluation on unseen wearers, and resource measurements; simply adding sensor channels or a neural network is not evidence of improvement. See PDF pp. 5-9 (article pp. 4-8).

**A timed low-acceleration, impact, and rotation sequence.** Huynh et al. [4](#study-4) tested a chest-mounted accelerometer/gyroscope system on 36 adults aged 18-28 and 38-56, collecting 702 laboratory movements and using half the dataset for development and half for assessment. Their rule first detects low acceleration, then requires both an acceleration peak and an angular-velocity peak within a 0.5-second window. The optimized study thresholds were 0.30-0.35 g for low acceleration, 2.4 g for impact, and 240 degrees/s for rotation. Table 3 reports 96.3% sensitivity and 96.2% specificity; the optimized acceleration-only comparison achieved 97.36% and 82.72%, respectively. The main improvement was fewer false alarms, with a small sensitivity reduction. These are chest-mounted, simulated-fall results, not thresholds established for this hardware or its intended users. See PDF pp. 3-7.

**A smartphone prototype with limited testing.** Rakhman et al. [5](#study-5) combined acceleration, rotation, posture, and orientation checks using a phone at the left chest. One male participant performed 120 simulated falls and 210 daily-activity trials. Table III records 112 detected falls out of 120 (93.33%); backward falls were detected in 26 of 30 trials. Daily activities including running, lying on a bed, and descending stairs caused false alarms. The paper's claimed 98% daily-activity accuracy is not consistent with Table IV: its alarm column lists seven false alarms, its running row totals 31 despite listing 30 trials, and its downstairs percentage does not match its counts. That aggregate claim should therefore be treated cautiously. The conclusion describes the work as limited to detection and leaves SMS/GPS extensions for future work, despite broader alert language in the abstract. See PDF pp. 4-5 (proceedings pp. 102-103).

### Implications for SeniorHealthNode

The following are project development proposals drawn from the studies, **not features added to the firmware**:

- **Evaluate sequences as well as peaks.** Compare the current rule with a bounded event window combining acceleration and rotation, and potentially subsequent posture or stillness [2](#study-2), [4](#study-4). The current rule triggers on acceleration **or** rotation; its quiet period only rearms the rule after an alert. It does not confirm a fall before sending an alert.
- **Define how the node is worn.** Chest, thigh, waist, and phone-orientation results are not interchangeable [1](#study-1)-[5](#study-5). A nearby, unworn sensor measures its own movement and cannot directly infer the person's body motion. Placement, fastening, units, sensor ranges, and sampling timing need to be specified before adapting published thresholds.
- **Compare methods on the same held-out recordings.** Evaluate a simple temporal rule before adding voting or an LSTM, and keep test wearers separate from threshold tuning or model training. Study [3](#study-3) demonstrates why performance must be measured rather than inferred from model complexity.
- **Measure missed falls and false alerts separately.** Sensitivity is the fraction of falls detected; specificity is the fraction of non-fall activities correctly rejected. Overall accuracy alone can hide missed falls. A future evaluation should also report false alerts per wear-day, detection delay, and performance across routine activities and intended mounting positions.
- **Test detection and delivery separately.** Published classifier results do not establish MeshCore delivery reliability. Record whether an event was detected, whether its alert was queued, and whether the intended recipient received it, alongside battery and radio conditions.

The studies use different participants, hardware, placements, datasets, and evaluation procedures, so their percentages are not a ranking or a performance estimate for this project. None validates the current SeniorHealthNode firmware in everyday use by older adults. The existing **2.5 g or 250 degrees/s** demonstration thresholds remain unchanged.

### Bibliography

The entries cite the five supplied works, rather than reproducing the reference lists inside them. Page locators above refer to the supplied PDFs; published pagination is included below where available.

<a id="study-1"></a>

1. Astriani, Maria Seraphina; Heryadi, Yaya; Kusuma, Gede Putra; and Abdurachman, Edi. (2019). **Human Fall Detection using Accelerometer and Gyroscope Sensors in Unconstrained Smartphone Positions.** *International Journal of Recent Technology and Engineering*, 8(3), 69-75. [DOI: 10.35940/ijrte.C3877.098319](https://doi.org/10.35940/ijrte.C3877.098319). Supplied files: `fall_detect_paper1.pdf` and identical `fall_detect_paper7.pdf`.

<a id="study-2"></a>

2. Li, Qiang; Stankovic, John A.; Hanson, Mark; Barth, Adam; Lach, John; and Zhou, Gang. (2009). **Accurate, Fast Fall Detection Using Gyroscopes and Accelerometer-Derived Posture Information.** *Body Sensor Networks (BSN 2009)*, 138-143. [DOI: 10.1109/BSN.2009.46](https://doi.org/10.1109/BSN.2009.46). Supplied files: `fall_detect_paper2.pdf` and identical `fall_detect_paper3.pdf`.

<a id="study-3"></a>

3. Wisesa, I Wayan Wiprayoga, and Mahardika, Genggam. (2019). **Fall detection algorithm based on accelerometer and gyroscope sensor data using Recurrent Neural Networks.** *IOP Conference Series: Earth and Environmental Science*, 258, 012035. [DOI: 10.1088/1755-1315/258/1/012035](https://doi.org/10.1088/1755-1315/258/1/012035). Supplied file: `fall_detect_paper4.pdf`.

<a id="study-4"></a>

4. Huynh, Quoc T.; Nguyen, Uyen D.; Irazabal, Lucia B.; Ghassemian, Nazanin; and Tran, Binh Q. (2015). **Optimization of an Accelerometer and Gyroscope-Based Fall Detection Algorithm.** *Journal of Sensors*, 2015, Article ID 452078, 8 pages. [DOI: 10.1155/2015/452078](https://doi.org/10.1155/2015/452078). Supplied file: `fall_detect_paper5.pdf`.

<a id="study-5"></a>

5. Rakhman, Arkham Zahri; Nugroho, Lukito Edi; Widyawan; and Kurnianingsih. (2014). **Fall Detection System Using Accelerometer and Gyroscope Based on Smartphone.** *2014 1st International Conference on Information Technology, Computer and Electrical Engineering (ICITACEE)*, 99-104. [IEEE publication record](https://ieeexplore.ieee.org/document/7065722/). Supplied file: `fall_detect_paper6.pdf`.

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
