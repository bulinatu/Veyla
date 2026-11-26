"""Generates the BetterRemotePlay application icon.

Draws a rounded dark "aurora" tile with a light dual-sense-style controller.
Run from the repository root:

    python tools/make_icon.py

Outputs:
    assets/app.png   1024px preview / docs
    assets/app.ico   multi-size Windows icon (16..256)
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

S = 1024
OUT = Path(__file__).resolve().parent.parent / "assets"


def rounded_mask(size: int, box, radius: int) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


def vertical_gradient(size: int, top: tuple[int, int, int], bottom: tuple[int, int, int]) -> Image.Image:
    column = Image.new("RGB", (1, size))
    pixels = column.load()
    for y in range(size):
        t = y / max(1, size - 1)
        pixels[0, y] = (
            round(top[0] + (bottom[0] - top[0]) * t),
            round(top[1] + (bottom[1] - top[1]) * t),
            round(top[2] + (bottom[2] - top[2]) * t),
        )
    return column.resize((size, size), Image.Resampling.NEAREST)


def aurora_layer(size: int, mask: Image.Image) -> Image.Image:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    blobs = [
        ((150, 120, 700, 620), (34, 211, 238, 120)),    # cyan
        ((520, 40, 1120, 560), (139, 92, 246, 110)),    # violet
        ((300, 600, 900, 1120), (16, 185, 129, 70)),    # emerald
    ]
    for box, color in blobs:
        draw.ellipse(box, fill=color)
    layer = layer.filter(ImageFilter.GaussianBlur(150))
    layer.putalpha(Image.composite(layer.getchannel("A"), Image.new("L", (size, size), 0), mask))
    return layer


def controller_mask(size: int) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(mask)
    # Main bar and the two grips. The bar is deliberately wide and the grips
    # low, so the silhouette still reads as a gamepad at 16 px.
    draw.rounded_rectangle((246, 360, 778, 604), radius=126, fill=255)
    draw.ellipse((240, 500, 474, 768), fill=255)
    draw.ellipse((550, 500, 784, 768), fill=255)
    # Arch between the grips.
    draw.rounded_rectangle((428, 560, 596, 820), radius=88, fill=0)
    return mask


def draw_controller(size: int) -> Image.Image:
    mask = controller_mask(size)
    body = vertical_gradient(size, (255, 255, 255), (188, 203, 219))
    body = body.convert("RGBA")
    body.putalpha(mask)

    # A soft drop shadow under the controller.
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    shadow_mask = mask.filter(ImageFilter.GaussianBlur(26))
    shadow.putalpha(shadow_mask.point(lambda v: int(v * 0.55)))
    shadow = Image.composite(shadow, Image.new("RGBA", (size, size), (0, 0, 0, 0)), mask)
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))

    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    layer.alpha_composite(shadow, (0, 22))
    layer.alpha_composite(body)

    draw = ImageDraw.Draw(layer)
    # Touchpad.
    draw.rounded_rectangle((432, 404, 592, 474), radius=24, fill=(226, 232, 240, 255),
                           outline=(148, 163, 184, 255), width=3)
    # D-pad.
    draw.rounded_rectangle((326, 466, 406, 510), radius=12, fill=(15, 23, 42, 255))
    draw.rounded_rectangle((346, 446, 386, 530), radius=12, fill=(15, 23, 42, 255))
    # Face buttons (PlayStation colours, simplified to dots).
    cx, cy, step, radius = 674, 500, 46, 18
    for dx, dy, color in [
        (0, -step, (34, 211, 238, 255)),
        (step, 0, (244, 63, 94, 255)),
        (0, step, (163, 230, 53, 255)),
        (-step, 0, (167, 139, 250, 255)),
    ]:
        draw.ellipse((cx + dx - radius, cy + dy - radius, cx + dx + radius, cy + dy + radius),
                     fill=color)
    return layer


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    size = S
    tile = vertical_gradient(size, (8, 15, 28), (16, 38, 54)).convert("RGBA")
    mask = rounded_mask(size, (28, 28, size - 28, size - 28), 228)
    tile.putalpha(mask)
    tile.alpha_composite(aurora_layer(size, mask))

    # Inner rim: a light hairline and a darker outer edge keep the tile crisp
    # on both light and dark taskbars.
    rim = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rim)
    rd.rounded_rectangle((28, 28, size - 28, size - 28), radius=228,
                         outline=(255, 255, 255, 46), width=6)
    rd.rounded_rectangle((43, 43, size - 43, size - 43), radius=214,
                         outline=(0, 0, 0, 50), width=3)
    tile.alpha_composite(rim)

    controller = draw_controller(size)
    # Nudge the controller up a touch: the grips carry the visual weight.
    tile.alpha_composite(controller, (0, -40))

    tile.save(OUT / "app.png")
    tile.save(OUT / "app.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                                      (64, 64), (128, 128), (256, 256)])
    print(f"wrote {OUT / 'app.png'} and {OUT / 'app.ico'}")


if __name__ == "__main__":
    main()
