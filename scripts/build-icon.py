#!/usr/bin/env python3
"""Convert the approved transparent PNG to the embedded Windows icon (Pillow)."""

from pathlib import Path

from PIL import Image


def main() -> None:
    resources = Path(__file__).resolve().parent.parent / "src/windows/resources"
    with Image.open(resources / "nskkzime.png") as source:
        source.convert("RGBA").save(
            resources / "nskkzime.ico",
            format="ICO",
            sizes=[(size, size) for size in (16, 20, 24, 32, 40, 48, 64, 128, 256)],
        )


if __name__ == "__main__":
    main()
