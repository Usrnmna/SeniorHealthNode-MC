# Assembly and initial setup

```text
GY-521 -- I2C (GPIO4/6) --> Heltec V4.3 OLED <-- BLE --> phone client
Passive buzzer <-- GPIO47 --       |
                                  +-- LoRa mesh --> receiving companion --> recipient
```

The buzzer is optional. The connection summary omits power and ground; the
[pin reference](pin_schematic.md) provides complete signal and supply assignments.
The sensor must be firmly attached to the wearer. A receiving companion requires
matching radio settings and the selected channel key.

## Setup procedure

Run build commands from the `SeniorHealthNode-MC` repository root. Install
PlatformIO Core using the [Windows build setup](build-validation.md#build-and-test-on-windows).

1. Use a **Heltec V4.3 OLED** board and connect a GY-521:
   `VCC -> 3V3`, `GND -> GND`, `SDA -> GPIO4`, `SCL -> GPIO6`, and
   `AD0 -> GND` for address `0x68`. Follow the [power and pin reference](pin_schematic.md).
2. If sound is required, connect the DIYables passive module I/O to GPIO47,
   VCC to 3V3 and GND to GND, following the
   [wiring guide](gy521_sensor.md#passive-buzzer-wiring-and-shared-functions).
   Its driver is onboard. The [buzzer-disabled configuration](gy521_sensor.md#operation-without-a-buzzer)
   suppresses GPIO/PWM setup when desired; physical absence also permits monitoring.
3. Set compiled assistance text and fallback location in
   [HealthNodeConfig.h](../include/HealthNodeConfig.h) if the defaults are unsuitable.
   Source-setting changes require rebuilding and flashing. Build and upload the
   combined firmware:

   ```powershell
   pio run -e heltec_v4_companion_radio_ble
   pio run -e heltec_v4_companion_radio_ble -t upload --upload-port COM_PORT
   ```

4. Pair a phone with this node over BLE using the default PIN `12345`
   (`012345` in a six-digit pairing dialog). A saved custom PIN takes precedence.
   Configure its name and radio parameters through a compatible MeshCore client
   to match the intended network.
5. Select the fall-message destination with the
   [USB/BLE channel commands](cli_commands.md#fall-message-channel-usb-and-ble).
   Add the selected public hashtag channel (initially `#falldetect`) or matching
   private channel and shared key to every receiving companion. Match radio
   settings on senders and receivers. Fall messages do not require contact registration.
6. Use a serial monitor at **115200 baud** and enter `motion` to inspect the latest
   sensor reading. Check alarm responses and actual alert delivery with the intended
   recipient at the intended location before relying on the setup. The
   [sensor validation procedure](gy521_sensor.md#build-and-run) and
   [USB debugging guide](usb_serial_debugging.md) describe focused checks.

Replace `COM_PORT` with the board's port. The [GY-521 sensor guide](gy521_sensor.md) has the full wiring table, recipient setup, commands, and validation steps. Use `health` to distinguish queueing, local TX, repeater evidence, and exhaustion. Confirm that the intended companion actually receives the channel messages; a heard repeat is not recipient acknowledgment.

## Related procedures

- [Daily alarm operation](operation.md): response gestures, automatic warnings and battery reminders.
- [Stationary gyroscope calibration](gyro_calibration.md): optional maintenance procedure and temporary monitoring pause.
- [USB serial debugging](usb_serial_debugging.md): Windows/Linux ports, commands and troubleshooting.
- [Build outputs and flashing](build-validation.md#uploading-the-combined-firmware): application and merged-image distinctions.
