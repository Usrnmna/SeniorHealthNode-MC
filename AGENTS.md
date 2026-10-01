## Non-negotiable firmware architecture

SeniorHealthNode is ONE firmware image based on the MeshCore BLE companion.
BLE companion functionality and fall detection run together on the same node.

The firmware target is heltec_v4_companion_radio_ble.
Do not create or restore a separate sensor firmware or move health features
out of the companion without my explicit approval.

Memories or documentation describing two firmware images are superseded
and must not be treated as requirements.
