#!/usr/bin/env python3
"""Turn the Metal renderer's per-pass debug dump (T-056) into PNG images.

  BO1_DEBUG_FRAME=5200 BO1_DEBUG_DIR=artifacts/debug/khe_sanh_5200 \\
      build/replay artifacts/traces/khe_sanh.bin --backend metal --frames 5199-5200
  .venv/bin/python tools/pass_dump.py artifacts/debug/khe_sanh_5200

Each pass_NNNN_c.raw (colour target) / pass_NNNN_d.raw (depth) becomes a PNG next to it. Float and
depth data are shown normalised; the value range is printed and stored in ranges.json.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# MTLPixelFormat values used by the renderer
BGRA8, R32F, RG16, RGBA16, RGBA16F, B5G6R5, RG8, R8, A8, D32F = 80, 55, 60, 110, 115, 40, 30, 10, 1, 252


def load(path):
    data = path.read_bytes()
    nl = data.index(b"\n")
    _, w, h, bpp, fmt, d3dfmt = data[:nl].split()
    w, h, bpp, fmt = int(w), int(h), int(bpp), int(fmt)
    return w, h, fmt, int(d3dfmt), np.frombuffer(data, dtype=np.uint8, offset=nl + 1).reshape(h, w, bpp)


def to_image(w, h, fmt, px):
    """-> (PIL image, info dict)"""
    info = {}
    if fmt == BGRA8:
        info["alpha_min_max"] = [int(px[..., 3].min()), int(px[..., 3].max())]
        return Image.fromarray(px[..., [2, 1, 0]].copy(), "RGB"), info
    if fmt in (R8, A8):
        return Image.fromarray(px[..., 0].copy(), "L"), info
    if fmt in (R32F, D32F):
        v = px.copy().view(np.float32)[..., 0]
    elif fmt == RGBA16F:
        v = px.copy().view(np.float16).astype(np.float32)[..., :3]
    elif fmt == RGBA16:
        v = px.copy().view(np.uint16).astype(np.float32)[..., :3] / 65535.0
    elif fmt == RG16:
        v = px.copy().view(np.uint16).astype(np.float32) / 65535.0
        v = np.dstack([v, np.zeros((h, w), np.float32)])
    elif fmt == RG8:
        v = np.dstack([px.astype(np.float32) / 255.0, np.zeros((h, w), np.float32)])
    elif fmt == B5G6R5:
        u = px.copy().view(np.uint16)[..., 0]
        v = np.dstack([(u >> 11) / 31.0, ((u >> 5) & 63) / 63.0, (u & 31) / 31.0]).astype(np.float32)
    else:
        return None, {"error": f"format {fmt}"}
    finite = v[np.isfinite(v)]
    lo, hi = (float(finite.min()), float(finite.max())) if finite.size else (0.0, 0.0)
    info["min_max"] = [lo, hi]
    info["percentiles_1_50_99"] = [float(x) for x in np.percentile(finite, [1, 50, 99])] if finite.size else []
    if fmt in (R32F, D32F) or hi > 1.0 or lo < 0.0:
        v = (v - lo) / (hi - lo) if hi > lo else v * 0
    out = (np.clip(np.nan_to_num(v), 0, 1) * 255 + 0.5).astype(np.uint8)
    return Image.fromarray(out, "L" if out.ndim == 2 else "RGB"), info


def main() -> int:
    d = Path(sys.argv[1])
    ranges = {}
    for raw in sorted(d.glob("pass_*.raw")):
        w, h, fmt, d3dfmt, px = load(raw)
        img, info = to_image(w, h, fmt, px)
        info.update(size=[w, h], mtl_format=fmt, d3d_format=d3dfmt)
        ranges[raw.stem] = info
        if img:
            img.save(raw.with_suffix(".png"))
    (d / "ranges.json").write_text(json.dumps(ranges, indent=1))
    print(f"{len(ranges)} targets converted in {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
