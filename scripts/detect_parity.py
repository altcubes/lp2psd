#!/usr/bin/env python3
"""Compare the C++ detector output against the Python reference
(scripts/parity_detect.py): rotated quads + per-pixel stroke mask.

Usage:
  python scripts/parity_detect.py --out parity_out   # generate reference
  python scripts/detect_parity.py <image> <cpp_dbg_dir> [--ref parity_out]

The C++ debug dir must contain <stem>_quads.json and <stem>_mask.png
(produced by lp2psd --debug-ocr).
"""

import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image


def quad_iou(a, b):
    """IoU of two convex quads via rasterization on a local grid."""
    xs = [p[0] for p in a] + [p[0] for p in b]
    ys = [p[1] for p in a] + [p[1] for p in b]
    x0, x1 = int(math.floor(min(xs))), int(math.ceil(max(xs))) + 1
    y0, y1 = int(math.floor(min(ys))), int(math.ceil(max(ys))) + 1
    if x1 <= x0 or y1 <= y0:
        return 0.0

    def fill(poly):
        m = np.zeros((y1 - y0, x1 - x0), dtype=bool)
        for yy in range(y1 - y0):
            y = y0 + yy + 0.5
            xs_hit = []
            for i in range(4):
                xa, ya = poly[i]
                xb, yb = poly[(i + 1) % 4]
                if (ya <= y < yb) or (yb <= y < ya):
                    xs_hit.append(xa + (y - ya) * (xb - xa) / (yb - ya))
            xs_hit.sort()
            for i in range(0, len(xs_hit) - 1, 2):
                xl = max(0, int(math.ceil(xs_hit[i] - x0)))
                xr = min(m.shape[1], int(math.floor(xs_hit[i + 1] - x0)) + 1)
                m[yy, xl:xr] = True
        return m

    ma, mb = fill(a), fill(b)
    inter = (ma & mb).sum()
    union = (ma | mb).sum()
    return inter / union if union else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("cpp_dir")
    ap.add_argument("--ref", default=os.path.join("parity_out"))
    args = ap.parse_args()

    stem = os.path.splitext(os.path.basename(args.image))[0]
    cpp_quads = json.load(open(
        os.path.join(args.cpp_dir, f"{stem}_quads.json"), encoding="utf-8"))["boxes"]
    ref_quads = json.load(open(
        os.path.join(args.ref, f"{stem}_lines.json"), encoding="utf-8"))

    print(f"cpp={len(cpp_quads)} boxes, ref={len(ref_quads)} boxes")
    # bipartite matching by quad IoU
    used = [False] * len(ref_quads)
    matches = []
    for c in cpp_quads:
        best, best_i = 0.0, -1
        for i, r in enumerate(ref_quads):
            if used[i]:
                continue
            iou = quad_iou(
                [(c["quad"][k * 2], c["quad"][k * 2 + 1]) for k in range(4)],
                r["quad"])
            if iou > best:
                best, best_i = iou, i
        if best_i >= 0:
            used[best_i] = True
            matches.append(best)
    print(f"matched={len(matches)}/{len(cpp_quads)} "
          f"mean_iou={np.mean(matches) if matches else 0:.3f} "
          f"min_iou={min(matches) if matches else 0:.3f}")

    cpp_mask = np.asarray(Image.open(
        os.path.join(args.cpp_dir, f"{stem}_mask.png")).convert("L")) > 127
    ref_mask = np.asarray(Image.open(
        os.path.join(args.ref, f"{stem}_mask_gray.png")).convert("L")) > 127
    inter = (cpp_mask & ref_mask).sum()
    union = (cpp_mask | ref_mask).sum()
    print(f"mask: cpp={cpp_mask.sum()} ref={ref_mask.sum()} "
          f"iou={inter / union:.4f}" if union else "mask: empty")

    bad = [iou for iou in matches if iou < 0.7]
    if bad or (matches and len(matches) < len(cpp_quads)):
        print("WARNING: some quads differ from the reference")
        sys.exit(1)


if __name__ == "__main__":
    main()
