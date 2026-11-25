#!/usr/bin/env python3
"""Regenerate every ModelForge icon from branding/model-forge-logo.jpg.

The generated assets are shared by all platforms (they live in the Web UI
bundle) and by the Windows executable (favicon.ico is embedded as an RC
resource). Run from any directory:

    python scripts/generate-icons.py

Requirements: Pillow  (python -m pip install Pillow)
"""

from pathlib import Path
import io
import struct

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover - environment issue
    raise SystemExit("Pillow is required: python -m pip install Pillow") from exc

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "branding" / "model-forge-logo.jpg"
PUBLIC = ROOT / "frontend" / "public"
DOC_IMAGES = ROOT / "docs" / "images"

PNG_TARGETS = (
    (16, "favicon-16x16.png"),
    (32, "favicon-32x32.png"),
    (192, "android-chrome-192x192.png"),
    (512, "android-chrome-512x512.png"),
)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)


def write_ico(image: Image.Image, destination: Path) -> None:
    """Write a multi-resolution ICO with PNG-compressed entries (Vista+)."""
    entries = []
    for size in ICO_SIZES:
        buffer = io.BytesIO()
        image.resize((size, size), Image.LANCZOS).save(buffer, "PNG", optimize=True)
        entries.append((size, buffer.getvalue()))

    with destination.open("wb") as handle:
        handle.write(struct.pack("<HHH", 0, 1, len(entries)))
        offset = 6 + 16 * len(entries)
        for size, data in entries:
            dimension = 0 if size == 256 else size
            handle.write(struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(data), offset))
            offset += len(data)
        for _, data in entries:
            handle.write(data)


def main() -> None:
    if not SOURCE.is_file():
        raise SystemExit(f"missing source image: {SOURCE}")

    image = Image.open(SOURCE).convert("RGBA")
    PUBLIC.mkdir(parents=True, exist_ok=True)
    DOC_IMAGES.mkdir(parents=True, exist_ok=True)

    for size, name in PNG_TARGETS:
        image.resize((size, size), Image.LANCZOS).save(PUBLIC / name, "PNG", optimize=True)
    write_ico(image, PUBLIC / "favicon.ico")
    image.resize((512, 512), Image.LANCZOS).convert("RGB").save(
        DOC_IMAGES / "logo.png", "PNG", optimize=True
    )

    for name in ("favicon.ico", *(item[1] for item in PNG_TARGETS)):
        print(f"{name:32s} {(PUBLIC / name).stat().st_size:>8d} bytes")
    print(f"{'docs/images/logo.png':32s} {(DOC_IMAGES / 'logo.png').stat().st_size:>8d} bytes")


if __name__ == "__main__":
    main()
