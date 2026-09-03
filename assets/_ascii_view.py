#!/usr/bin/env python3
"""Render a PNG as ASCII art + report the opaque content bounding box.
Usage: python3 _ascii_view.py <file.png> [width]"""
import sys
import numpy as np
from PIL import Image

chars = " .:-=+*#%@"

def main():
    path = sys.argv[1]
    W = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    img = Image.open(path).convert("RGBA")
    fullW, fullH = img.size
    a = np.asarray(img).astype(np.float32)
    lum = (0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]) / 255.0
    alpha = a[:, :, 3] / 255.0
    # content bbox (alpha > 8)
    mask255 = (alpha * 255) > 8
    ys, xs = np.where(mask255)
    if len(xs):
        print(f"content bbox: x[{xs.min()}..{xs.max()}] y[{ys.min()}..{ys.max()}] "
              f"on {fullW}x{fullH}, alpha>8 coverage {mask255.mean()*100:.1f}%")
    else:
        print(f"NO OPAQUE CONTENT on {fullW}x{fullH}")
    # ascii (keep aspect; chars ~2x tall)
    aspect = fullH / fullW
    H = max(1, int(W * aspect * 0.5))
    img2 = img.resize((W, H))
    b = np.asarray(img2).astype(np.float32)
    blum = (0.299 * b[:, :, 0] + 0.587 * b[:, :, 1] + 0.114 * b[:, :, 2]) / 255.0
    bmask = b[:, :, 3] / 255.0
    val = np.clip(blum * bmask, 0, 1)
    for row in val:
        line = "".join(chars[min(len(chars) - 1, int(v * len(chars)))] for v in row)
        print(line)

if __name__ == "__main__":
    main()
