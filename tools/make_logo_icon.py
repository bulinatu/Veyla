"""Generates the Veyla application icon.

The icon is the same mark the shell draws in its title bar: a rounded square
with the violet-to-indigo gradient and a white bold V. Keeping one generator
means the taskbar icon and the in-app logo can never drift apart.

Run from the repository root:

    python tools/make_logo_icon.py

Outputs:
    assets/app.png       1024 px preview
    assets/app.ico       multi-size Windows icon (16..256)
    ui/assets/app.ico    the copy embedded in the GPUI shell
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
UI_ASSETS = ROOT / "ui" / "assets"
FONT = ROOT / "ui" / "assets" / "fonts" / "Inter-Bold.ttf"

SIZE = 1024
SS = 4  # supersampling
S = SIZE * SS

# Same values as widgets::logo_mark: linear-gradient(140deg, #8B5CF6, #4F46E5).
TOP = (0x8B, 0x5C, 0xF6)
BOTTOM = (0x4F, 0x46, 0xE5)
ANGLE = 140.0


def gradient(size: int) -> Image.Image:
    # CSS-style angle: 0 = to top, 90 = to right, 180 = to bottom.
    import math

    rad = math.radians(ANGLE)
    dx, dy = math.sin(rad), -math.cos(rad)
    # Project the corners so the gradient covers the whole square.
    corners = [(0, 0), (size, 0), (0, size), (size, size)]
    projections = [x * dx + y * dy for x, y in corners]
    lo, hi = min(projections), max(projections)
    image = Image.new("RGB", (size, size))
    pixels = image.load()
    span = max(1.0, hi - lo)
    for y in range(size):
        for x in range(size):
            t = ((x * dx + y * dy) - lo) / span
            t = min(1.0, max(0.0, t))
            pixels[x, y] = (
                round(TOP[0] + (BOTTOM[0] - TOP[0]) * t),
                round(TOP[1] + (BOTTOM[1] - TOP[1]) * t),
                round(TOP[2] + (BOTTOM[2] - TOP[2]) * t),
            )
    return image


def draw_mark(scale: int) -> Image.Image:
    size = SIZE * scale
    image = gradient(SIZE).resize((size, size), Image.Resampling.LANCZOS).convert("RGBA")

    # Rounded square: radius is 28% of the tile, matching logo_mark().
    mask = Image.new("L", (size, size), 0)
    radius = round(size * 0.28)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    image.putalpha(mask)

    # White bold V, 46% of the tile, centred on its ink box.
    font = ImageFont.truetype(str(FONT), round(size * 0.46))
    draw = ImageDraw.Draw(image)
    box = font.getbbox("V")
    width = box[2] - box[0]
    height = box[3] - box[1]
    x = (size - width) / 2 - box[0]
    y = (size - height) / 2 - box[1]
    # A V carries its mass in the two upper strokes, so a mathematically
    # centred ink box still reads a touch high. Nudge it down by 0.6% of the
    # tile to land on the optical centre.
    y += size * 0.006
    draw.text((x, y), "V", font=font, fill=(255, 255, 255, 255), anchor="la")
    return image


def main() -> None:
    if not FONT.exists():
        raise SystemExit(f"font not found: {FONT}")
    master = draw_mark(SS).resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    ASSETS.mkdir(parents=True, exist_ok=True)
    master.save(ASSETS / "app.png")

    sizes = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48),
             (64, 64), (96, 96), (128, 128), (256, 256)]
    master.save(ASSETS / "app.ico", sizes=sizes)

    UI_ASSETS.mkdir(parents=True, exist_ok=True)
    (UI_ASSETS / "app.ico").write_bytes((ASSETS / "app.ico").read_bytes())
    print(f"wrote {ASSETS / 'app.ico'} and {UI_ASSETS / 'app.ico'}")


if __name__ == "__main__":
    main()
