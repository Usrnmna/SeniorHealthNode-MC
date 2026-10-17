"""Render the README's illustrative response sequence as SVG and animated GIF."""

from html import escape
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


OUTPUT_DIRECTORY = Path(__file__).resolve().parent
CANVAS_WIDTH = 1200
CANVAS_HEIGHT = 340
BACKGROUND = "#071C31"
PANEL = "#102C43"
OUTLINE = "#254A60"
FOREGROUND = "#F3F7FA"
MUTED = "#B4C8D6"
ACCENT = "#41D8CF"
STEPS = (
    ("01", "Possible fall", "Motion sequence detected", "SOS + initial alert queued"),
    ("02", "Wearer response", "Three short PRG presses", "SOS stops; assistance queued"),
    ("03", "Channel message", "Assistance text + location", "Matching receiving radios"),
)
FRAME_DURATIONS_MS = (1800, 1800, 1800, 3000)


def font(size, bold=False):
    """Load a readable system font on Windows or common Linux installations."""
    candidates = (
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
        "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf",
    )
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    raise RuntimeError("Segoe UI or DejaVu Sans is required to render the walkthrough.")


def render_frame(active_step):
    """Highlight one step, or all steps, without implying measured event timing."""
    frame = Image.new("RGB", (CANVAS_WIDTH, CANVAS_HEIGHT), BACKGROUND)
    draw = ImageDraw.Draw(frame)
    draw.text((32, 20), "ILLUSTRATED ALARM SEQUENCE", font=font(16, True), fill=ACCENT)
    draw.text((32, 49), "Assistance requested before automatic escalation", font=font(25, True), fill=FOREGROUND)
    for index, (number, title, first_line, second_line) in enumerate(STEPS):
        left = 32 + index * 394
        highlighted = active_step is None or active_step == index
        color = ACCENT if highlighted else OUTLINE
        draw.rounded_rectangle((left, 105, left + 348, 262), radius=18, fill=PANEL, outline=color, width=3)
        draw.text((left + 20, 123), number, font=font(20, True), fill=ACCENT if highlighted else MUTED)
        draw.text((left + 20, 158), title, font=font(27, True), fill=FOREGROUND)
        draw.text((left + 20, 204), first_line, font=font(20), fill=MUTED)
        draw.text((left + 20, 232), second_line, font=font(20), fill=MUTED)
        if index < len(STEPS) - 1:
            draw.line((left + 360, 183, left + 381, 183), fill=ACCENT, width=3)
            draw.polygon(((left + 382, 183), (left + 375, 178), (left + 375, 188)), fill=ACCENT)
    draw.text((32, 290), "Concept sequence only. Radio reception depends on coverage, settings and available recipients.", font=font(19), fill=MUTED)
    return frame


def render_static_svg():
    """Provide an editable, motion-free counterpart with accessible descriptions."""
    elements = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{CANVAS_WIDTH}" height="{CANVAS_HEIGHT}" viewBox="0 0 {CANVAS_WIDTH} {CANVAS_HEIGHT}" role="img" aria-labelledby="title description">',
        '<title id="title">Illustrated alarm sequence</title>',
        '<desc id="description">A possible fall starts SOS and queues an alert. Before escalation, three short PRG presses stop SOS and queue assistance text with location for the selected channel. Actual reception requires a working radio path.</desc>',
        f'<rect width="1200" height="340" rx="20" fill="{BACKGROUND}"/>',
        '<g font-family="Segoe UI, DejaVu Sans, sans-serif">',
        f'<text x="32" y="39" font-size="16" font-weight="700" fill="{ACCENT}">ILLUSTRATED ALARM SEQUENCE</text>',
        f'<text x="32" y="78" font-size="25" font-weight="700" fill="{FOREGROUND}">Assistance requested before automatic escalation</text>',
    ]
    for index, (number, title, first_line, second_line) in enumerate(STEPS):
        left = 32 + index * 394
        elements.append(f'<rect x="{left}" y="105" width="348" height="157" rx="18" fill="{PANEL}" stroke="{ACCENT}" stroke-width="3"/>')
        for y, text, size, color, weight in (
            (148, number, 20, ACCENT, 700),
            (187, title, 27, FOREGROUND, 700),
            (225, first_line, 20, MUTED, 400),
            (253, second_line, 20, MUTED, 400),
        ):
            elements.append(f'<text x="{left + 20}" y="{y}" font-size="{size}" font-weight="{weight}" fill="{color}">{escape(text)}</text>')
        if index < len(STEPS) - 1:
            elements.append(f'<path d="M{left + 360} 183h21m-6 -5 7 5-7 5" fill="none" stroke="{ACCENT}" stroke-width="3"/>')
    elements.extend((
        f'<text x="32" y="311" font-size="19" fill="{MUTED}">Concept sequence only. Radio reception depends on coverage, settings and available recipients.</text>',
        '</g></svg>',
    ))
    return "\n".join(elements) + "\n"


def main():
    """Write the static source and a short sequence with no flashing transitions."""
    (OUTPUT_DIRECTORY / "alarm-walkthrough.svg").write_text(render_static_svg(), encoding="utf-8")
    frames = [render_frame(step) for step in (0, 1, 2, None)]
    frames[0].save(
        OUTPUT_DIRECTORY / "alarm-walkthrough.gif",
        save_all=True,
        append_images=frames[1:],
        duration=FRAME_DURATIONS_MS,
        loop=1,
        optimize=True,
        disposal=2,
    )


if __name__ == "__main__":
    main()
