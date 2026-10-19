#!/usr/bin/env python3
"""Convert PNG images to pre-dithered RGB565 raw bytes for embedded use.

Each input PNG is fit-and-resized to the target box (default 240x240)
while preserving aspect ratio, composited on white, dithered with
Floyd-Steinberg, then packed to RGB565 little-endian. The output .bin
is a flat array of uint16_t pixels, exactly target_w * target_h * 2
bytes — ready to be blitted to the panel without any runtime
conversion (no decode, no resample, no dither at boot).

Usage:
    prepare-assets.py SRC.png DST.bin [--width 240] [--height 240]
"""
import argparse
import os
import sys


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("src", help="Source PNG path")
    p.add_argument("dst", help="Destination .bin path (RGB565 LE)")
    p.add_argument("--width", type=int, default=240)
    p.add_argument("--height", type=int, default=240)
    args = p.parse_args()

    try:
        from PIL import Image
    except ImportError:
        sys.stderr.write(
            "ERROR: Pillow is not installed. Run: pip install Pillow\n"
        )
        return 1

    # Open and flatten onto a white background so transparent PNGs get
    # sensible colour instead of black halos.
    img = Image.open(args.src).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    img = Image.alpha_composite(bg, img).convert("RGB")

    # Fit-and-resize: only shrinks. Preserves aspect ratio. A 1024x1024
    # PNG becomes 240x240; a 64x64 icon becomes 64x64 on a 240x240 canvas.
    img.thumbnail((args.width, args.height), Image.LANCZOS)
    canvas = Image.new("RGB", (args.width, args.height), (255, 255, 255))
    canvas.paste(img, ((args.width - img.width) // 2,
                       (args.height - img.height) // 2))
    img = canvas

    w, h = img.size
    pixels = img.load()
    out = bytearray(w * h * 2)

    def add(px, py, er, eg, eb, scale):
        if 0 <= px < w and 0 <= py < h:
            nr, ng, nb = pixels[px, py]
            pixels[px, py] = (
                max(0, min(255, nr + er * scale // 16)),
                max(0, min(255, ng + eg * scale // 16)),
                max(0, min(255, nb + eb * scale // 16)),
            )

    # Floyd-Steinberg dither straight into RGB565.
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[x, y]
            qr = (r >> 3) << 3
            qg = (g >> 2) << 2
            qb = (b >> 3) << 3
            er, eg, eb = r - qr, g - qg, b - qb
            v = (qr >> 3) << 11 | (qg >> 2) << 5 | (qb >> 3)
            o = (y * w + x) * 2
            out[o] = v & 0xFF
            out[o + 1] = (v >> 8) & 0xFF
            pixels[x, y] = (qr, qg, qb)
            if x + 1 < w:
                add(x + 1, y, er, eg, eb, 7)
            if y + 1 < h:
                if x > 0:
                    add(x - 1, y + 1, er, eg, eb, 3)
                add(x, y + 1, er, eg, eb, 5)
                if x + 1 < w:
                    add(x + 1, y + 1, er, eg, eb, 1)

    with open(args.dst, "wb") as f:
        f.write(out)
    print(f"prepared {os.path.basename(args.src)} -> "
          f"{os.path.basename(args.dst)} ({w}x{h}, {len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
