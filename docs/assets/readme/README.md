# README presentation assets

These assets support the project overview. Detailed behavior remains in the
[operation guide](../../operation.md), [setup guide](../../setup.md) and
[validation guide](../../build-validation.md).

| Asset | Purpose |
| --- | --- |
| `seniorhealthnode-banner.png` | Concept illustration of wearable monitoring and a radio-mesh connection. The illustration does not represent a verified enclosure, PCB layout or physical test. |
| `status-experimental.svg` | Prototype-status badge linked to the README limitations. |
| `meshcore-ble.svg` | MeshCore BLE companion badge linked to the project foundation. |
| `local-monitoring.svg` | Local-monitoring badge linked to the capability overview. |
| `alarm-walkthrough.gif` | Illustrated assistance path before automatic escalation. Step highlighting runs twice, then stops on the complete sequence. Presentation timing is unrelated to firmware timing. |
| `alarm-walkthrough.svg` | Editable, motion-free version of the illustrated sequence. |
| `render_walkthrough.py` | Shared labels and layout for the SVG and GIF. Requires Python, Pillow, and Segoe UI or DejaVu Sans. |

The badges and static walkthrough use local SVG without scripts or external image
services. Regenerate the walkthrough from the repository root with:

```powershell
python docs/assets/readme/render_walkthrough.py
```

The illustrations do not establish fall-detection accuracy, recipient reception,
phone compatibility or physical alarm operation. The alarm table and linked
guides retain the text equivalent and precise gesture rules.
