"""Prepare generated astronomy/weather art as CYD RGB565 cache payloads."""

import base64
from pathlib import Path

from PIL import Image, ImageOps

ROOT = Path(__file__).parent
OUT = ROOT / "assets" / "cache"
OUT.mkdir(parents=True, exist_ok=True)
SIZE = (54, 54)
TRANSPARENT = 0x0001


def rgb565(image):
    image = ImageOps.fit(image.convert("RGB"), SIZE, method=Image.Resampling.LANCZOS)
    raw = image.tobytes()
    out = bytearray()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i : i + 3]
        color = TRANSPARENT if r < 24 and g < 40 and b < 80 else ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((color & 0xFF, color >> 8))
    return base64.b64encode(out).decode("ascii")


def write_asset(name, image):
    (OUT / f"{name}.b64").write_text(rgb565(image), encoding="ascii")


def crop_grid(path, cols, rows, names):
    image = Image.open(path).convert("RGB")
    cell_w, cell_h = image.width // cols, image.height // rows
    for index, name in enumerate(names):
        x = (index % cols) * cell_w
        y = (index // cols) * cell_h
        write_asset(name, image.crop((x, y, x + cell_w, y + cell_h)))


crop_grid(ROOT / "assets" / "astronomy" / "moon_sheet.png", 4, 2, [f"moon{i}" for i in range(8)])
write_asset("sun", Image.open(ROOT / "assets" / "astronomy" / "sun.png"))
crop_grid(
    ROOT / "assets" / "weather" / "sheet.png",
    4,
    2,
    [f"weather{i}" for i in range(8)],
)
print(f"Prepared {len(list(OUT.glob('*.b64')))} assets in {OUT}")
