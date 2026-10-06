# Security Policy

## Project scope

This repository contains the combined SeniorHealthNode BLE companion firmware,
`heltec_v4_companion_radio_ble`. Include the exact source revision when reporting
an issue; an upstream MeshCore version label alone does not identify this fork's
health-monitoring and channel-setting code.

## Fall-channel data

The compiled fall destination is public `#falldetect`. Its hashtag-derived key
is reproducible from the channel name and is not a private recipient secret.
A private destination requires a shared key configured on sending and receiving
nodes. Messages can contain the node name, assistance text and GPS coordinates
or the configured fallback location.

Saved channel records contain the shared key without at-rest encryption. Firmware
reads/replies omit the key and USB input has no firmware echo, but a client can
retain typed commands in its history. Existing incidents keep their original
channel after a setting change. Corrupt existing records disable fall sending
until a valid setting is saved; absent records use the compiled default. Local
detection and SOS remain available when the channel is invalid.

A matching repeater echo is not authenticated recipient or human acknowledgement.
See [channel setup](docs/cli_commands.md#fall-message-channel-usb-and-ble) and
[delivery evidence](docs/message_delivery_changes.md) for exact behavior.

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

Use GitHub's private vulnerability reporting instead:
1. Go to the **Security** tab of this repository
2. Click **Report a vulnerability**
3. Fill in the details and submit

### What to include

A useful report tells us:
- Which component or file is affected
- What an attacker can do (impact) and under what conditions
- A minimal reproduction case or proof-of-concept if you have one
- Whether you believe it is remotely exploitable

You do not need a working exploit to report. An incomplete report is better
than no report.

## What to expect

This is a volunteer-maintained open-source project. We will do our best to
respond in a reasonable timeframe, but cannot commit to specific deadlines.

We ask that you give us a fair opportunity to investigate and address the
issue before any public disclosure. If you have not heard back after
**90 days**, feel free to follow up or proceed with disclosure at your
discretion.

## Scope

In scope:
- Remote code execution, memory corruption, or denial-of-service via crafted
  radio packets
- Authentication or encryption bypasses
- Vulnerabilities in the packet routing or path handling logic

Out of scope:
- Physical access attacks (e.g., JTAG, UART extraction of keys)
- Regulatory compliance (duty cycle, frequency restrictions)
- Jamming or other physical-layer radio interference
- Issues in third-party libraries (RadioLib, Crypto, etc.) — report those
  upstream
- "Best practice" suggestions without a demonstrated attack path
