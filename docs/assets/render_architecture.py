#!/usr/bin/env python3
"""Render bar-tech architecture PNG (cogitor.dev-inspired dark theme)."""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "bar-tech-architecture.png"
FACE = ROOT / "barmen-idle-thumb.jpg"
SRC_FACE = Path(r"D:\Projects\youtube\Бармен-idle.png")

W, H = 1920, 1080
BG = (10, 10, 10)
PANEL = (17, 17, 17)
PANEL2 = (22, 22, 22)
CHIP = (26, 26, 26)
BORDER = (42, 42, 42)
TEXT = (245, 245, 245)
MUTED = (138, 138, 138)
AMBER = (232, 164, 90)
CYAN = (94, 234, 212)
CYAN_DIM = (45, 120, 112)
TEAL_BG = (15, 31, 29)


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        r"C:\Windows\Fonts\segoeui.ttf",
        r"C:\Windows\Fonts\seguisb.ttf" if bold else r"C:\Windows\Fonts\segoeui.ttf",
        r"C:\Windows\Fonts\arial.ttf",
        r"C:\Windows\Fonts\calibri.ttf",
    ]
    if bold:
        candidates = [
            r"C:\Windows\Fonts\seguisb.ttf",
            r"C:\Windows\Fonts\segoeuib.ttf",
            r"C:\Windows\Fonts\arialbd.ttf",
            *candidates,
        ]
    for path in candidates:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def round_rect(draw: ImageDraw.ImageDraw, box, fill, outline=None, width=1, radius=12):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def text_center(draw, xy, text, fnt, fill):
    x, y = xy
    bbox = draw.textbbox((0, 0), text, font=fnt)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    draw.text((x - tw / 2, y - th / 2), text, font=fnt, fill=fill)


def draw_grid(img: Image.Image, step: int = 48):
    overlay = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)
    for x in range(0, W, step):
        d.line([(x, 0), (x, H)], fill=(28, 28, 28, 180), width=1)
    for y in range(0, H, step):
        d.line([(0, y), (W, y)], fill=(28, 28, 28, 180), width=1)
    img.alpha_composite(overlay)


def ensure_face() -> Image.Image:
    if FACE.exists():
        return Image.open(FACE).convert("RGB")
    src = Image.open(SRC_FACE).convert("RGB")
    w, h = src.size
    side = min(w, h)
    left = (w - side) // 2
    top = max(0, (h - side) // 2 - side // 10)
    face = src.crop((left, top, left + side, top + side)).resize((368, 368), Image.LANCZOS)
    face.save(FACE, "JPEG", quality=90)
    return face


def main() -> None:
    img = Image.new("RGBA", (W, H), (*BG, 255))
    draw_grid(img)
    draw = ImageDraw.Draw(img)

    f_col = font(16, bold=True)
    f_h = font(22, bold=True)
    f_body = font(18)
    f_small = font(15)
    f_tiny = font(13)

    # No page title — article heading covers that.
    # Column geometry (shifted up for tighter crop)
    cols = [
        ("DEVICE", AMBER, 56, 48, 560),
        ("LAN", MUTED, 656, 48, 600),
        ("CLOUD", CYAN, 1296, 48, 560),
    ]
    col_h = 900
    for label, color, x, y, cw in cols:
        round_rect(draw, (x, y, x + cw, y + col_h), PANEL, BORDER, 1, 14)
        round_rect(draw, (x, y, x + cw, y + 52), PANEL2, BORDER, 1, 14)
        draw.rectangle((x + 1, y + 26, x + cw - 1, y + 52), fill=PANEL2)
        draw.text((x + 24, y + 16), label, font=f_col, fill=color)

    # ── Device ──────────────────────────────────────────────
    dx = 56
    draw.text((dx + 24, 118), "bar-face · ESP32-S3", font=f_h, fill=TEXT)

    face = ensure_face().resize((300, 300), Image.LANCZOS)
    fx, fy = dx + 130, 170
    round_rect(draw, (fx - 8, fy - 8, fx + 300 + 8, fy + 300 + 8), (21, 21, 21), AMBER, 2, 12)
    mask = Image.new("L", (300, 300), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 299, 299), radius=8, fill=255)
    img.paste(face, (fx, fy), mask)

    chips = ["Wake word", "Opus UDP", "Display", "Pour"]
    positions = [(dx + 36, 520), (dx + 300, 520), (dx + 36, 600), (dx + 300, 600)]
    for text, (cx, cy) in zip(chips, positions):
        round_rect(draw, (cx, cy, cx + 240, cy + 52), CHIP, BORDER, 1, 8)
        text_center(draw, (cx + 120, cy + 26), text, f_body, TEXT)

    # ── LAN ─────────────────────────────────────────────────
    lx = 656
    round_rect(draw, (lx + 28, 128, lx + 572, 186), CHIP, BORDER, 1, 8)
    text_center(draw, (lx + 300, 157), "iot-platform · MQTT", f_body, TEXT)

    # bar-core hub (sidecars sit below, outside this box)
    hx1, hy1, hx2, hy2 = lx + 28, 210, lx + 572, 560
    round_rect(draw, (hx1, hy1, hx2, hy2), (14, 28, 26), CYAN, 2, 12)
    glow = Image.new("RGBA", (120, 120), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for r, a in [(55, 40), (35, 70), (16, 160)]:
        gd.ellipse((60 - r, 60 - r, 60 + r, 60 + r), fill=(*CYAN, a))
    img.alpha_composite(glow, (lx + 240, 238))
    text_center(draw, (lx + 300, 268), "bar-core", f_h, TEXT)

    modules = [
        ("Session", 0, 0),
        ("STT / TTS", 1, 0),
        ("Dialog", 2, 0),
        ("RAG", 0, 1),
        ("LLM client", 1, 1),
        ("Pour tool", 2, 1),
    ]
    mw, mh, gap = 160, 48, 16
    ox, oy = lx + 48, 340
    for name, col, row in modules:
        x = ox + col * (mw + gap)
        y = oy + row * (mh + gap)
        outline = CYAN if name == "RAG" else BORDER
        round_rect(draw, (x, y, x + mw, y + mh), (18, 18, 18), outline, 1, 8)
        text_center(draw, (x + mw / 2, y + mh / 2), name, f_small, TEXT)

    text_center(
        draw,
        (lx + 300, 500),
        "voice session · knowledge · dialog tools",
        f_tiny,
        MUTED,
    )

    draw.text((lx + 28, 580), "local voice", font=f_tiny, fill=MUTED)
    for i, name in enumerate(["whisper.cpp", "Piper TTS"]):
        x = lx + 28 + i * 280
        round_rect(draw, (x, 608, x + 256, 668), CHIP, BORDER, 1, 8)
        text_center(draw, (x + 128, 638), name, f_body, TEXT)

    # ── Cloud ───────────────────────────────────────────────
    cx = 1296
    draw.text((cx + 24, 118), "OpenAI-compat", font=f_h, fill=TEXT)
    for i, name in enumerate(["Chat Completions", "Whisper API", "TTS API", "Embeddings"]):
        y = 180 + i * 100
        round_rect(draw, (cx + 36, y, cx + 524, y + 76), TEAL_BG, CYAN_DIM, 1, 8)
        text_center(draw, (cx + 280, y + 38), name, f_body, TEXT)
    draw.text((cx + 36, 610), "dashed links = optional cloud voice", font=f_tiny, fill=MUTED)

    # ── Connectors ──────────────────────────────────────────
    draw.line([(616, 240), (656, 157)], fill=CYAN, width=2)
    draw.text((600, 180), "MQTT", font=f_tiny, fill=CYAN)
    draw.line([(616, 420), (656, 340)], fill=CYAN, width=4)
    draw.text((590, 390), "Opus UDP", font=f_tiny, fill=CYAN)
    for y1, y2 in [(390, 218), (440, 418)]:
        x0, x1 = 1256, 1296
        segs = 12
        for s in range(segs):
            if s % 2:
                continue
            t0, t1 = s / segs, (s + 1) / segs
            draw.line(
                [
                    (x0 + (x1 - x0) * t0, y1 + (y2 - y1) * t0),
                    (x0 + (x1 - x0) * t1, y1 + (y2 - y1) * t1),
                ],
                fill=CYAN,
                width=2,
            )

    # Legend
    ly = 980
    items = [
        (AMBER, "Device"),
        ((107, 107, 107), "LAN"),
        (CYAN, "Cloud"),
    ]
    x = 64
    for color, label in items:
        draw.rectangle((x, ly, x + 16, ly + 16), fill=color)
        draw.text((x + 24, ly - 2), label, font=f_small, fill=MUTED)
        x += 120
    draw.line([(x, ly + 8), (x + 40, ly + 8)], fill=CYAN, width=3)
    draw.text((x + 48, ly - 2), "control / audio", font=f_small, fill=MUTED)
    x += 200
    for s in range(0, 40, 8):
        draw.line([(x + s, ly + 8), (x + s + 4, ly + 8)], fill=CYAN, width=2)
    draw.text((x + 48, ly - 2), "optional cloud", font=f_small, fill=MUTED)

    rgb = img.convert("RGB")
    rgb.save(OUT, "PNG", optimize=True)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
