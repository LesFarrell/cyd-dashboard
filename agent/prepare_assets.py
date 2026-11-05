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
    image = ImageOps.fit(image.convert("RGBA"), SIZE, method=Image.Resampling.LANCZOS)
    raw = image.tobytes()
    pixels = bytearray()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i : i + 4]
        color = TRANSPARENT if a < 128 or (r < 24 and g < 40 and b < 80) else ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        pixels += bytes((color & 0xFF, color >> 8))
    compressed = bytearray()
    i = 0
    while i < len(pixels):
        color = pixels[i : i + 2]
        count = 1
        while i + count * 2 < len(pixels) and pixels[i + count * 2 : i + count * 2 + 2] == color and count < 255:
            count += 1
        compressed += bytes((count, color[0], color[1]))
        i += count * 2
    return base64.b64encode(compressed).decode("ascii")


def write_asset(name, image):
    (OUT / f"{name}.b64").write_text(rgb565(image), encoding="ascii")


def crop_grid(path, cols, rows, names):
    image = Image.open(path).convert("RGB")
    cell_w, cell_h = image.width // cols, image.height // rows
    for index, name in enumerate(names):
        x = (index % cols) * cell_w
        y = (index // cols) * cell_h
        write_asset(name, image.crop((x, y, x + cell_w, y + cell_h)))


moon_names = [
    "new-moon", "waxing-crescent", "first-quarter", "waxing-gibbous",
    "full-moon", "waning-gibbous", "last-quarter", "waning-crescent",
]
for index, name in enumerate(moon_names):
    write_asset(f"moon{index}", Image.open(ROOT / "assets" / "astronomy" / "tideplanet" / f"{name}.png"))

write_asset("sun", Image.open(ROOT / "assets" / "astronomy" / "sun.png"))

weather_names = [
    "clear-day", "partly-cloudy-day", "cloudy", "fog",
    "hail", "rain", "snow", "thunder",
]
for index, name in enumerate(weather_names):
    write_asset(f"weather{index}", Image.open(ROOT / "assets" / "weather" / "visualcrossing" / f"{name}.png"))
print(f"Prepared {len(list(OUT.glob('*.b64')))} assets in {OUT}")
