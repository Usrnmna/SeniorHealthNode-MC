# Contributing to SeniorHealthNode

SeniorHealthNode is one firmware image based on the MeshCore BLE companion:
`heltec_v4_companion_radio_ble`. BLE services and health monitoring run on the
same node. Changes to this architecture, firmware targets, hardware roles or
feature ownership require explicit project-owner approval before implementation.

## Reports and proposed changes

For a bug report, include the source revision, board/sensor wiring, configuration,
reproduction steps and relevant serial output. Distinguish observed hardware
behavior from simulated or source-inferred behavior. Remove private keys from
logs. For vulnerabilities, follow [SECURITY.md](SECURITY.md).

Discuss substantial features before implementation. Keep patches focused, preserve
existing behavior outside the agreed scope, and update the nearest maintained
guide for changed features, settings or commands. Do not add a separate sensor
firmware or move fall detection out of the companion.

## Code and documentation

Follow `.clang-format` and nearby C++ conventions. Use descriptive names, focused
functions and obvious sections. Keep tunable values easy to find; explain
non-obvious inputs, side effects, timing assumptions and failure behavior.
Avoid complexity that makes manual inspection or adjustment harder.

Use impersonal academic language in documentation and code comments. State
requirements, configuration, behavior, assumptions and evidence directly, without
personal narration or discussion history. Preserve original upstream MeshCore
documentation; apply editorial revisions to project-specific material. Document
only work performed and evidence verified. Describe current capabilities rather
than keeping dated change summaries or old passing-test totals in usage guides.

Keep the main README focused on the intended audience, project capabilities,
alarm experience, setup entry points and limitations. Place implementation detail,
research, wiring schedules and verification procedures in linked topic guides.
Use text connection or process diagrams when they clarify relationships more
directly than prose. Preserve relevant settings, source citations and evidence
boundaries when moving material between documents.

## Verification

Follow the [Windows setup](docs/build-validation.md#build-and-test-on-windows), then run
`.\verify.ps1` for firmware changes. It builds the combined image and merged
binary and runs both `native` and `native_delivery`. Add focused tests for changed
observable behavior; [fault testing](docs/virtual-fault-testing.md) lists examples.

For documentation-only edits, check links, examples, commands and settings against
the current source and run `git diff --check`. A firmware rebuild is not evidence
that prose, links or physical hardware are correct.

In a pull request, state the problem, resulting behavior, checks actually run and
any remaining validation limits. Builds and host tests do not establish real RF
delivery, phone/BLE behavior, physical sensor timing or fall-detection accuracy.
