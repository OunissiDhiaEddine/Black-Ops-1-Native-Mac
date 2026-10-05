#!/usr/bin/env python3
"""T-035: compare replayed frames with the baseline's golden frames.

  .venv/bin/python tools/compare_frames.py GOLDEN_DIR REPLAY_DIR [--diff OUT_DIR] [--threshold 0.90]

For every frame_NNNNNN.png present in both directories prints SSIM (luma, 8x8 windows), mean
absolute error and the share of pixels differing by more than 16/255; optionally writes a diff
image (golden | replay | 4x amplified difference). Exit code 1 if any frame is below the threshold.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def box(a, k):
    """Mean over k x k windows (valid region), via an integral image."""
    c = np.cumsum(np.cumsum(np.pad(a, ((1, 0), (1, 0))), axis=0), axis=1)
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def ssim(x, y, k=8):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mx, my = box(x, k), box(y, k)
    vx, vy = box(x * x, k) - mx * mx, box(y * y, k) - my * my
    cov = box(x * y, k) - mx * my
    s = ((2 * mx * my + c1) * (2 * cov + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2))
    return float(s.mean()), s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("golden")
    ap.add_argument("replay")
    ap.add_argument("--diff")
    ap.add_argument("--threshold", type=float, default=0.90)
    ap.add_argument("--json")
    args = ap.parse_args()
    g, r = Path(args.golden), Path(args.replay)
    names = sorted(p.name for p in g.glob("frame_*.png") if (r / p.name).exists())
    if not names:
        print("no common frames")
        return 2
    if args.diff:
        Path(args.diff).mkdir(parents=True, exist_ok=True)
    results, worst = [], 1.0
    for n in names:
        a = np.asarray(Image.open(g / n).convert("RGB"), dtype=np.float64)
        b = np.asarray(Image.open(r / n).convert("RGB"), dtype=np.float64)
        if a.shape != b.shape:
            print(f"{n}: size mismatch {a.shape} vs {b.shape}")
            worst = 0.0
            continue
        s, smap = ssim(luma(a), luma(b))
        d = np.abs(a - b)
        res = {"frame": n, "ssim": round(s, 4), "mae": round(float(d.mean()), 2),
               "pixels_off_by_16": round(float((d.max(axis=2) > 16).mean()), 4)}
        results.append(res)
        worst = min(worst, s)
        print(f"{n}: SSIM {res['ssim']:.4f}   MAE {res['mae']:.2f}/255   pixels differing >16: {100 * res['pixels_off_by_16']:.1f}%")
        if args.diff:
            amp = np.clip(d * 4, 0, 255)
            Image.fromarray(np.concatenate([a, b, amp], axis=1).astype(np.uint8)).save(Path(args.diff) / n)
    if args.json:
        Path(args.json).write_text(json.dumps({"threshold": args.threshold, "frames": results}, indent=1) + "\n")
    print(f"worst SSIM {worst:.4f} (threshold {args.threshold})")
    return 0 if worst >= args.threshold else 1


if __name__ == "__main__":
    sys.exit(main())
