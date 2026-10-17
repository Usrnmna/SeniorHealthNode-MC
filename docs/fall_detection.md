# Possible-fall detection and research

The staged detector runs locally in `heltec_v4_companion_radio_ble`. A PC is needed for building, flashing and setup; motion decisions run on the Heltec.

## Detector flow

```text
Settling -- continuous quiet and cooldown complete --> Armed
   ^                                                    |
   |                                             low acceleration
   |                                                    |
   |                                                    v
   |                                  Peaks: impact AND rotation in window
   |                                                    |
   |                                                    v
   +--- emit one event <--- Confirming: continuous quiet before deadline
```

Invalid samples, excessive sample gaps and expired candidates return the rule
to settling. Cooldown applies after an emitted event; it can overlap quiet rearming. Setting `confirm_quiet_ms = 0` bypasses only
post-sequence confirmation. The [tuning guide](gy521_sensor.md#review-and-tune-the-algorithm)
provides configuration constraints and tradeoffs.

## Algorithm stages and default settings

The `STAGE 0` through `STAGE 5` comments in [`MotionRule.h`](../examples/companion_radio/health/MotionRule.h) identify each part of the implementation. Internally, the rule moves through `Settling`, `Armed`, `Peaks`, and `Confirming` states.

| Code stage | Behavior | Configuration defaults |
| --- | --- | --- |
| 0: validate samples | Reject invalid, negative, or nonfinite magnitudes; discard partial evidence across a long sample gap. Ignore duplicate timestamps. | `max_sample_gap_ms = 100` |
| 1: settle and arm | Require continuous quiet at startup, after interrupted or expired evidence, and before rearming after an alert. | `rearm_quiet_ms = 2000` |
| 2: start an event | Acceleration at or below the low threshold opens a fixed window. Repeated low readings cannot extend it. | `low_g = 0.35`, `event_window_ms = 500` |
| 3: collect both peaks | Require acceleration at or above the impact threshold **and** rotation at or above its threshold within the event window. Peaks may occur on different samples and in either order. | `impact_g = 2.4`, `rotation_dps = 240` |
| 4: confirm quiet | Require one continuous quiet interval, completed before the deadline measured from when both peaks have been observed. Movement restarts the quiet timer, not the deadline. | `confirm_quiet_ms = 1000`, `confirm_timeout_ms = 5000` |
| 5: emit and suppress repeats | Emit one possible-fall event, then require cooldown and quiet before rearming. These periods can overlap. | `cooldown_ms = 60000` |

Quiet means acceleration between `quiet_min_g = 0.7` and `quiet_max_g = 1.3`, inclusive, with rotation below `quiet_max_dps = 30`. Acceleration is the three-axis vector magnitude in **g, including gravity**; rotation is the three-axis angular-velocity magnitude in **degrees/second**. A sample gap exactly equal to 100 ms is tolerated; a longer gap resets evidence. Timing uses unsigned elapsed milliseconds to handle timer rollover.

Stages 2-3 adapt the timed sequence from Huynh et al. [4](#study-4). Quiet confirmation, startup settling, data guards, and cooldown are **project-specific extensions**. This is an adaptation for evaluation, not a reproduction of the paper's validated experimental setup or its reported performance. Quiet magnitudes do not establish lying posture or injury. Soft/sliding falls without the required low acceleration, impact, or rotation will be missed; quiet confirmation can also miss falls followed by continued movement. A dropped device can resemble a fall.

## Runtime behavior

- [`MPU6050.h`](../src/helpers/sensors/MPU6050.h) configures filtered 50 Hz sampling with ranges of +/-8 g and +/-500 degrees/second, then verifies the configuration by readback. Each burst contains data-ready status and measurements; only a fresh sample reaches the detector. Failed/partial reads invalidate the sample and retry after 20 ms; three consecutive failures trigger reinitialization. One second without new data enters the same recovery path. Failed initialization uses five-second backoff. Polling is cooperative, so other firmware work can delay it; host interrupt and FIFO capture are not used.
- [`MyMesh::pollMotion()` in HealthMonitor.cpp](../examples/companion_radio/HealthMonitor.cpp) passes fresh vector magnitudes to `MotionRule::update()` independently of pending alarms or delivery. Notifications coalesce while an earlier alert is pending. The detector's 60-second cooldown and quiet-rearm requirements remain in force.
- The combined firmware collects environmental readings in a background task and serves telemetry from a bounded-age cache. OLED frames transfer in small pieces; battery readings use staged ADC acquisition and a shared cache. Configuration, methods, files, and validation limits are listed in [the timing implementation report](fall_timing_changes.md).
- [`MotionRule.h`](../examples/companion_radio/health/MotionRule.h) holds the configuration and decision logic without Arduino dependencies, dynamic allocation, or blocking waits. `peakAcceleration()` and `peakRotation()` preserve event-window peaks for the alert, rather than reporting the later quiet readings.
- The initial channel alert uses `Possible fall: peak accel=%.2fg rotation=%.1fdeg/s`. The independent delivery controller retains it through local failures, then waits for repeater evidence. `health` reports both message states and motion outages/gaps. The [delivery implementation guide](message_delivery_changes.md) maps directives to code and adjustment points.

Chip temperature remains available in telemetry and the `motion` command; it is not body temperature. Gyroscope telemetry clips individual axes to the Cayenne encoding range of -327.68 to 327.67 degrees/second, while the detector, serial readings, and alert text retain the full measured range.

## Tuning and software checks

Edit `FallDetectionConfig` at the top of [`MotionRule.h`](../examples/companion_radio/health/MotionRule.h) and rebuild to change the active defaults. Custom code can instead pass a configuration to the `MotionRule` constructor. These settings are compile-time configuration; there is no runtime command to change them.

Set `confirm_quiet_ms = 0` to compare the low-acceleration/impact/rotation sequence without post-event quiet confirmation. Startup settling, sample validation, and cooldown still apply. Use the [tuning guide](gy521_sensor.md#review-and-tune-the-algorithm) for configuration constraints and tradeoffs. Firm torso/chest attachment is the intended starting point; different placements require separate evaluation.

The [motion-rule tests](../test/test_motion_rule/test_motion_rule.cpp) replay synthetic 50 Hz recordings. They cover settling, ordinary motion and isolated peaks, missing sequence features, peak order, timing boundaries, repeated low readings, interrupted quiet, invalid data, sample gaps, duplicate timestamps, cooldown/reconnection, timer rollover, and custom settings including sequence-only mode. From the repository root, run:

```powershell
pio test -e native -f test_motion_rule
pio test -e native -f test_fall_response
pio test -e native -f test_low_battery
```

This requires a host C++ compiler; the [sensor guide](gy521_sensor.md#polling-and-rule-customization) also documents the standalone C++11 harness. Passing these checks establishes software behavior, not sensitivity, specificity, false alerts per wear-day, or successful radio delivery. Voting, LSTM inference, and body-posture classification remain unimplemented; the studies below explain those possible future directions.

## Fall-detection research

The bibliography identifies **five distinct studies** and records the reference filenames used for the research review. Those filenames are provenance labels, not bundled project files. The current rule adapts the timed sequence from study [4](#study-4), with separately labeled project extensions. The other methods remain research context; none of the reported performance results has been reproduced or validated in SeniorHealthNode.

### Research context

**Smartphone orientation and voting across multiple rules.** Astriani et al. [1](#study-1) combined five threshold-based methods (AGVeSR, Alim, alpha-angle, GyroReDi, and AGPeak), declaring a fall when at least three agreed. They collected 252 activity records (84 falls and 168 non-falls) in six phone orientations from adults aged 25-35, with simulated falls onto a mattress. Table V reports 76.67% accuracy for acceleration alone, 91.67% for AGVeSR, and 95.82% for the five-method vote. Jumping and sitting/standing transitions remained sources of confusion. The paper describes both a 180/72 training/testing split and leave-one-out validation, so its evaluation protocol should not be assumed to be an independently replicated benchmark. For this project, the useful direction is to evaluate orientation changes and agreement between several motion features rather than assume one mounting orientation will work for everyone. See PDF pp. 4-6.

**Posture plus the preceding motion transition.** Li et al. [2](#study-2) used two sensor nodes, on the chest and thigh, sampled at 120 Hz. Their algorithm identifies a static posture and examines the preceding five seconds of acceleration and angular velocity when the posture is classified as lying. In experiments involving three men in their twenties, they reported 91% sensitivity from 70 fall records and 92% specificity from 72 non-fall records. Falling against a wall into a seated position could be missed; quickly lying down could trigger false alarms. This supports investigating temporal context and posture, but the two-body-location method cannot be reproduced directly by SeniorHealthNode's single MPU6050. See PDF sections 3-4, pp. 2-6.

**Learning from sequences does not guarantee better detection.** Wisesa and Mahardika [3](#study-3) trained a long short-term memory (LSTM) recurrent neural network on waist-sensor data from UMAFall. They retained 617 valid recordings, randomly dividing them into 494 training and 123 validation samples. Their X-axis accelerometer model's prediction table identifies all 38 falls and misclassifies 2 of 85 daily activities as falls. In contrast, their combined accelerometer/gyroscope model misses 31 of 38 falls and falsely flags 10 daily activities (Tables 3-4). The reported peak validation accuracy of 100% for the single-axis model is distinct from those saved-model prediction results. For SeniorHealthNode, an LSTM is an experimental alternative requiring suitable data, evaluation on unseen wearers, and resource measurements; simply adding sensor channels or a neural network is not evidence of improvement. See PDF pp. 5-9 (article pp. 4-8).

**A timed low-acceleration, impact, and rotation sequence.** Huynh et al. [4](#study-4) tested a chest-mounted accelerometer/gyroscope system on 36 adults aged 18-28 and 38-56, collecting 702 laboratory movements and using half the dataset for development and half for assessment. Their rule first detects low acceleration, then requires both an acceleration peak and an angular-velocity peak within a 0.5-second window. The optimized study thresholds were 0.30-0.35 g for low acceleration, 2.4 g for impact, and 240 degrees/s for rotation. Table 3 reports 96.3% sensitivity and 96.2% specificity; the optimized acceleration-only comparison achieved 97.36% and 82.72%, respectively. The main improvement was fewer false alarms, with a small sensitivity reduction. These are chest-mounted, simulated-fall results, not thresholds established for this hardware or its intended users. See PDF pp. 3-7.

**A smartphone prototype with limited testing.** Rakhman et al. [5](#study-5) combined acceleration, rotation, posture, and orientation checks using a phone at the left chest. One male participant performed 120 simulated falls and 210 daily-activity trials. Table III records 112 detected falls out of 120 (93.33%); backward falls were detected in 26 of 30 trials. Daily activities including running, lying on a bed, and descending stairs caused false alarms. The paper's claimed 98% daily-activity accuracy is not consistent with Table IV: its alarm column lists seven false alarms, its running row totals 31 despite listing 30 trials, and its downstairs percentage does not match its counts. That aggregate claim should therefore be treated cautiously. The conclusion describes the work as limited to detection and leaves SMS/GPS extensions for future work, despite broader alert language in the abstract. See PDF pp. 4-5 (proceedings pp. 102-103).

### Implications for SeniorHealthNode

The following separates the implemented rule from further research and validation:

- **Implemented: sequences as well as peaks.** The detector uses a bounded low-acceleration / impact / rotation window [4](#study-4) and an optional post-event quiet check. Study [2](#study-2) motivates exploring temporal context and posture, but quiet magnitudes do not classify lying and its two-node posture method is not implemented.
- **Define how the node is worn.** Chest, thigh, waist, and phone-orientation results are not interchangeable [1](#study-1)-[5](#study-5). A nearby, unworn sensor measures its own movement and cannot directly infer the person's body motion. Placement, fastening, units, sensor ranges, and sampling timing need to be specified before adapting published thresholds.
- **Compare methods on the same held-out recordings.** Evaluate a simple temporal rule before adding voting or an LSTM, and keep test wearers separate from threshold tuning or model training. Study [3](#study-3) demonstrates why performance must be measured rather than inferred from model complexity.
- **Measure missed falls and false alerts separately.** Sensitivity is the fraction of falls detected; specificity is the fraction of non-fall activities correctly rejected. Overall accuracy alone can hide missed falls. A future evaluation should also report false alerts per wear-day, detection delay, and performance across routine activities and intended mounting positions.
- **Test detection and delivery separately.** Published classifier results do not establish MeshCore delivery reliability. Record whether an event was detected, whether its alert was queued, and whether the intended recipient received it, alongside battery and radio conditions.

The studies use different participants, hardware, placements, datasets, and evaluation procedures, so their percentages are not a ranking or a performance estimate for this project. None validates the current SeniorHealthNode firmware in everyday use by older adults. Voting, LSTM inference, and body-posture classification are not implemented.

### Bibliography

The entries identify the five studies reviewed for this detector. Page locators above refer to the reviewed PDFs; published pagination is included below where available.

<a id="study-1"></a>

1. Astriani, Maria Seraphina; Heryadi, Yaya; Kusuma, Gede Putra; and Abdurachman, Edi. (2019). **Human Fall Detection using Accelerometer and Gyroscope Sensors in Unconstrained Smartphone Positions.** *International Journal of Recent Technology and Engineering*, 8(3), 69-75. [DOI: 10.35940/ijrte.C3877.098319](https://doi.org/10.35940/ijrte.C3877.098319). Reference filenames: `fall_detect_paper1.pdf` and identical `fall_detect_paper7.pdf`.

<a id="study-2"></a>

2. Li, Qiang; Stankovic, John A.; Hanson, Mark; Barth, Adam; Lach, John; and Zhou, Gang. (2009). **Accurate, Fast Fall Detection Using Gyroscopes and Accelerometer-Derived Posture Information.** *Body Sensor Networks (BSN 2009)*, 138-143. [DOI: 10.1109/BSN.2009.46](https://doi.org/10.1109/BSN.2009.46). Reference filenames: `fall_detect_paper2.pdf` and identical `fall_detect_paper3.pdf`.

<a id="study-3"></a>

3. Wisesa, I Wayan Wiprayoga, and Mahardika, Genggam. (2019). **Fall detection algorithm based on accelerometer and gyroscope sensor data using Recurrent Neural Networks.** *IOP Conference Series: Earth and Environmental Science*, 258, 012035. [DOI: 10.1088/1755-1315/258/1/012035](https://doi.org/10.1088/1755-1315/258/1/012035). Reference filename: `fall_detect_paper4.pdf`.

<a id="study-4"></a>

4. Huynh, Quoc T.; Nguyen, Uyen D.; Irazabal, Lucia B.; Ghassemian, Nazanin; and Tran, Binh Q. (2015). **Optimization of an Accelerometer and Gyroscope-Based Fall Detection Algorithm.** *Journal of Sensors*, 2015, Article ID 452078, 8 pages. [DOI: 10.1155/2015/452078](https://doi.org/10.1155/2015/452078). Reference filename: `fall_detect_paper5.pdf`.

<a id="study-5"></a>

5. Rakhman, Arkham Zahri; Nugroho, Lukito Edi; Widyawan; and Kurnianingsih. (2014). **Fall Detection System Using Accelerometer and Gyroscope Based on Smartphone.** *2014 1st International Conference on Information Technology, Computer and Electrical Engineering (ICITACEE)*, 99-104. [IEEE publication record](https://ieeexplore.ieee.org/document/7065722/). Reference filename: `fall_detect_paper6.pdf`.
