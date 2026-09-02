#!/usr/bin/env python3
"""Python reference of the yakuyomi-engine DBNet detector on ONNX Runtime.

Implements the full pipeline 1:1 from yakuyomi-engine's Detector.kt +
ImageOps.kt + Geometry.kt (ported from manga-image-translator default_utils):

  preprocess  resize_aspect (long side -> size, pad right/bottom to mult=256,
              black pad), RGB, (x/127.5 - 1), NCHW
  infer       dbnet_detect.onnx -> out0 db[1,2,H,W] (ch0 raw logits),
              out1 mask[1,1,H/2,W/2] (sigmoid)
  lines       sigmoid(ch0) -> binarize(dbBinThreshold) -> 8-conn components
              -> boundary pts -> minAreaRect -> unclip(dbUnclipRatio)
              -> rotated quad in original coords; score = component mean prob
  mask        valid region [0:round(origH*ratio*mw/inW)] -> bilinear resize
              -> threshold segThreshold -> per-pixel stroke mask

Used as the parity oracle for the C++ port (scripts/detect_parity check).
"""

import argparse
import json
import math
import os

import numpy as np
import onnxruntime as ort
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# DetectorConfig defaults from yakuyomi-engine Config.kt
class DetectorConfig:
    min_side = 3.0          # min box side in model-grid space
    seg_threshold = 0.12    # stroke mask binarization
    dbnet_input_size = 1024
    detect_unsharp = False
    db_bin_threshold = 0.5  # sigmoid(db ch0) > this
    db_box_threshold = 0.7  # component-mean prob < this -> drop
    db_unclip_ratio = 2.3


def resize_aspect(img, size, mult=256):
    """Long side -> size, pad right/bottom to mult multiple. Returns canvas."""
    h, w = img.shape[:2]
    ratio = size / max(w, h)
    tw = max(1, round(w * ratio))
    th = max(1, round(h * ratio))
    in_w = tw + (mult - tw % mult) % mult
    in_h = th + (mult - th % mult) % mult
    scaled = np.asarray(Image.fromarray(img).resize((tw, th), Image.BILINEAR))
    canvas = np.zeros((in_h, in_w, 3), dtype=np.uint8)
    canvas[:th, :tw] = scaled
    return canvas, ratio, tw, th


def preprocess(img_bgr, cfg):
    canvas, ratio, tw, th = resize_aspect(img_bgr, cfg.dbnet_input_size)
    rgb = canvas[..., ::-1].astype(np.float32)  # BGR -> RGB
    chw = np.ascontiguousarray(rgb.transpose(2, 0, 1) / 127.5 - 1.0)
    return chw, ratio, tw, th


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def convex_hull(points):
    pts = sorted(points, key=lambda p: (p[0], p[1]))
    if len(pts) < 3:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def min_area_rect(points):
    """Rotating calipers; matches cv2.minAreaRect. Returns
    (cx, cy, ux, uy, w, h)."""
    hull = convex_hull(points)
    if len(hull) < 2:
        return None
    best_area = float("inf")
    best = None
    for i in range(len(hull)):
        a = hull[i]
        b = hull[(i + 1) % len(hull)]
        ex, ey = b[0] - a[0], b[1] - a[1]
        length = math.hypot(ex, ey)
        if length < 1e-6:
            continue
        ex /= length
        ey /= length
        us = []
        vs = []
        for p in hull:
            dx, dy = p[0] - a[0], p[1] - a[1]
            us.append(dx * ex + dy * ey)
            vs.append(-dx * ey + dy * ex)
        min_u, max_u = min(us), max(us)
        min_v, max_v = min(vs), max(vs)
        w, h = max_u - min_u, max_v - min_v
        area = w * h
        if area < best_area:
            best_area = area
            cu = (min_u + max_u) / 2
            cv_ = (min_v + max_v) / 2
            cx = a[0] + cu * ex - cv_ * ey
            cy = a[1] + cu * ey + cv_ * ex
            best = (cx, cy, ex, ey, w, h)
    return best


def rot_rect_corners(rect):
    cx, cy, ux, uy, w, h = rect
    hw, hh = w / 2, h / 2
    return [
        (cx - hw * ux - hh * -uy, cy - hw * uy - hh * ux),
        (cx + hw * ux - hh * -uy, cy + hw * uy - hh * ux),
        (cx + hw * ux + hh * -uy, cy + hw * uy + hh * ux),
        (cx - hw * ux + hh * -uy, cy - hw * uy + hh * ux),
    ]


def unclip(rect, ratio):
    cx, cy, ux, uy, w, h = rect
    peri = 2 * (w + h)
    d = (w * h) * ratio / peri if peri > 1e-6 else 0.0
    return (cx, cy, ux, uy, w + 2 * d, h + 2 * d)


def lines_from_prob_map(prob, grid_w, grid_h, ratio, orig_w, orig_h, cfg):
    thresh = cfg.db_bin_threshold
    prob = prob.ravel()
    visited = np.zeros(prob.size, dtype=bool)
    out = []
    stack = np.empty(prob.size, dtype=np.int64)
    for seed in range(prob.size):
        if visited[seed] or prob[seed] <= thresh:
            continue
        sp = 0
        stack[sp] = seed
        sp += 1
        visited[seed] = True
        boundary = []
        total = 0.0
        cnt = 0
        while sp > 0:
            sp -= 1
            idx = stack[sp]
            x, y = idx % grid_w, idx // grid_w
            total += prob[idx]
            cnt += 1
            is_boundary = False
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < grid_w and 0 <= ny < grid_h:
                        nidx = ny * grid_w + nx
                        if prob[nidx] > thresh:
                            if not visited[nidx]:
                                visited[nidx] = True
                                stack[sp] = nidx
                                sp += 1
                        elif dx == 0 or dy == 0:
                            is_boundary = True
                    elif dx == 0 or dy == 0:
                        is_boundary = True
            if is_boundary:
                boundary.append((float(x), float(y)))
        score = total / cnt if cnt else 0.0
        if score < cfg.db_box_threshold:
            continue
        rect = min_area_rect(boundary)
        if rect is None or min(rect[4], rect[5]) < cfg.min_side:
            continue
        quad = rot_rect_corners(unclip(rect, cfg.db_unclip_ratio))
        quad = [
            (min(max(x / ratio, 0.0), float(orig_w)),
             min(max(y / ratio, 0.0), float(orig_h)))
            for x, y in quad
        ]
        out.append({"quad": quad, "score": float(score)})
    return out


def seg_to_mask(mask, src_w, src_h, ratio, orig_w, orig_h, cfg):
    nw = max(1, min(round(orig_w * ratio), src_w))
    nh = max(1, min(round(orig_h * ratio), src_h))
    valid = mask[:nh, :nw]
    scaled = np.asarray(
        Image.fromarray((valid * 255).astype(np.uint8))
        .resize((orig_w, orig_h), Image.BILINEAR)
        .convert("L"))
    return (scaled > cfg.seg_threshold * 255).astype(np.uint8)


def detect(img_bgr, cfg=None, session=None):
    cfg = cfg or DetectorConfig()
    if session is None:
        session = ort.InferenceSession(
            os.path.join(REPO, "dbnet_detect.onnx"),
            providers=["CPUExecutionProvider"])
    chw, ratio, tw, th = preprocess(img_bgr, cfg)
    db, mask = session.run(None, {"in0": chw[None]})
    db, mask = db[0], mask[0, 0]
    h, w = img_bgr.shape[:2]
    prob = sigmoid(db[0])
    lines = lines_from_prob_map(prob, chw.shape[2], chw.shape[1], ratio,
                                w, h, cfg)
    # mask grid ratio: mask is half-res here, but read actual size for safety
    mask_ratio = ratio * mask.shape[1] / chw.shape[2]
    text_mask = seg_to_mask(mask, mask.shape[1], mask.shape[0], mask_ratio,
                            w, h, cfg)
    return lines, text_mask


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="?", default=os.path.join(
        REPO, "third_party", "yakuyomi-engine", "app-sandbox",
        "src", "main", "assets", "test", "ch34_006.jpg"))
    ap.add_argument("--out", default=os.path.join(REPO, "parity_out"))
    args = ap.parse_args()

    img = np.asarray(Image.open(args.image).convert("RGB"))[..., ::-1].copy()
    lines, text_mask = detect(img)
    os.makedirs(args.out, exist_ok=True)
    stem = os.path.splitext(os.path.basename(args.image))[0]

    overlay = img[..., ::-1].copy()
    mask_rgb = np.zeros_like(overlay)
    mask_rgb[..., 0] = 255
    overlay = np.where(text_mask[:, :, None].astype(bool), mask_rgb, overlay)
    Image.fromarray(overlay).save(os.path.join(args.out, f"{stem}_mask.png"))
    Image.fromarray((text_mask * 255).astype(np.uint8)).save(
        os.path.join(args.out, f"{stem}_mask_gray.png"))

    overlay2 = img[..., ::-1].copy()
    for ln in lines:
        pts = np.array(ln["quad"], dtype=np.float32).round().astype(int)
        for i in range(4):
            x0, y0 = pts[i]
            x1, y1 = pts[(i + 1) % 4]
            cv2_line(overlay2, x0, y0, x1, y1, (255, 0, 0))
    Image.fromarray(overlay2).save(os.path.join(args.out, f"{stem}_lines.png"))

    with open(os.path.join(args.out, f"{stem}_lines.json"), "w") as f:
        json.dump(lines, f, indent=1)
    print(f"{len(lines)} lines, mask coverage "
          f"{text_mask.mean()*100:.2f}% -> {args.out}")


def cv2_line(img, x0, y0, x1, y1, color, w=2):
    # tiny Bresenham (avoids opencv dependency for the debug overlay)
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    while True:
        for oy in range(-(w // 2), w // 2 + 1):
            for ox in range(-(w // 2), w // 2 + 1):
                yy, xx = y0 + oy, x0 + ox
                if 0 <= yy < img.shape[0] and 0 <= xx < img.shape[1]:
                    img[yy, xx] = color
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy


if __name__ == "__main__":
    main()
