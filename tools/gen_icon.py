#!/usr/bin/env python3
"""
gen_icon.py - gera src/icon.ico (icone do .exe, usado por src/icon.rc) a partir de icon.png.

Mesmos 7 tamanhos do icone anterior: 16, 24, 32, 48, 64, 128 e 256 px. Requer Pillow (so para esta ferramenta).

Uso:
  python tools/gen_icon.py [icon.png] [src/icon.ico]
"""
import os
import sys

from PIL import Image

SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(here, "..")
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "icon.png")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "src", "icon.ico")
    img = Image.open(src).convert("RGBA")
    if img.width != img.height:
        print("aviso: a imagem nao e quadrada (%dx%d); sera cortada ao centro" % img.size)
        s = min(img.size)
        img = img.crop(((img.width - s) // 2, (img.height - s) // 2, (img.width + s) // 2, (img.height + s) // 2))
    img.save(dst, format="ICO", sizes=SIZES)
    print("gravado %s (%d bytes) a partir de %s (%dx%d)" % (dst, os.path.getsize(dst), src, img.width, img.height))


if __name__ == "__main__":
    main()
