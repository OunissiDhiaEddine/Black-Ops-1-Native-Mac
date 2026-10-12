#!/usr/bin/env python3
"""Reader for the recorded D3D9 call traces (docs/TRACE_FORMAT.md).

  .venv/bin/python tools/trace_reader.py TRACE.bin            summary (calls, bytes per method)
  .venv/bin/python tools/trace_reader.py TRACE.bin --dump N   print the first N records
  .venv/bin/python tools/trace_reader.py TRACE.bin --census OUT.json   full census for T-015

Interface and method names come from src/rhi/d3d9_methods.h (single source of truth).
"""
import argparse
import json
import mmap
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
METHODS_H = ROOT / "src" / "rhi" / "d3d9_methods.h"
BLOB_TAGS = {1: "OUT", 2: "DATA", 3: "STRUCT", 4: "RECT_A", 5: "RECT_B", 6: "BYTECODE", 7: "ELEMENTS",
             8: "PP", 9: "LOCK", 10: "DATA_B"}
LOCK_FIELDS = ("format width height depth pitch slice_pitch lock_flags has_rect left top right bottom "
               "front back offset size data_ptr dumped").split()


def load_interfaces():
    """Expand the X-macros of d3d9_methods.h -> [(com_name, [method names])] indexed by iface id."""
    text = METHODS_H.read_text().replace("\\\n", " ")
    macros = dict(re.findall(r"#define (\w+_METHODS)\(X\)\s+(.*)", text))

    def expand(name):
        out = []
        for m in re.finditer(r"(\w+_METHODS)\(X\)|X\((\d+),\s*(\w+)\)", macros[name]):
            if m.group(1):
                out += expand(m.group(1))
            else:
                out.append((int(m.group(2)), m.group(3)))
        return out

    ifaces = []
    for m in re.finditer(r"F\((\d+),\s*(\w+),\s*(\w+),\s*(\w+),\s*\w+\)", text):
        methods = [n for _, n in sorted(expand(m.group(4)))]
        ifaces.append((int(m.group(1)), m.group(3), methods))
    return [(com, meth) for _, com, meth in sorted(ifaces)]


class Record:
    __slots__ = ("iface", "flags", "depth", "slot", "tid", "result", "words", "blobs", "offset")

    @property
    def has_result(self):
        return bool(self.flags & 1)


def read_trace(path):
    """Yield Record objects. Blob payloads are memoryview slices into the mapped file."""
    f = open(path, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    view = memoryview(mm)
    magic, version, header_size, ptr, rec_hdr, n_if = struct.unpack_from("<8sIIIII", mm, 0)
    if magic != b"BO1D3D9T" or version not in (1, 2) or rec_hdr != 16:
        raise SystemExit(f"not a v1 trace: {magic!r} v{version}")
    off, end = header_size, len(mm)
    while off + 16 <= end:
        iface, flags, depth, nblobs, slot, nwords, tid, result = struct.unpack_from("<BBBBHHII", mm, off)
        r = Record()
        r.offset = off
        off += 16
        if off + 4 * nwords > end:
            break
        if iface >= n_if or nwords > 32 or nblobs > 6:
            print(f"warning: corrupt record at offset {off - 16}; stopping", file=sys.stderr)
            break
        r.iface, r.flags, r.depth, r.slot, r.tid, r.result = iface, flags, depth, slot, tid, result
        r.words = struct.unpack_from(f"<{nwords}I", mm, off)
        off += 4 * nwords
        r.blobs = []
        ok = True
        for _ in range(nblobs):
            if off + 8 > end:
                ok = False
                break
            tag, length = struct.unpack_from("<II", mm, off)
            off += 8
            if off + length > end:
                ok = False
                break
            r.blobs.append((tag, view[off: off + length]))
            off += (length + 3) & ~3
        if not ok:
            break                      # truncated tail (process was killed mid-write)
        yield r


def lock_desc(blob):
    return dict(zip(LOCK_FIELDS, struct.unpack_from("<18I", blob, 0)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--dump", type=int, default=0)
    ap.add_argument("--census", help="write a JSON census here")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    ifaces = load_interfaces()

    def name(r):
        com, meth = ifaces[r.iface]
        return f"{com}::{meth[r.slot]}"

    calls, payload = Counter(), Counter()
    blob_bytes = Counter()
    per_frame = Counter()
    lock_sizes = defaultdict(Counter)
    frames = nrec = 0
    nested = 0
    for r in read_trace(args.trace):
        nrec += 1
        n = name(r)
        calls[n] += 1
        if r.depth:
            nested += 1
        size = sum(len(b) for _, b in r.blobs)
        payload[n] += size
        for tag, b in r.blobs:
            blob_bytes[BLOB_TAGS.get(tag, str(tag))] += len(b)
            if tag == 9 and n.endswith(("::Unlock", "::UnlockRect", "::UnlockBox")):
                d = lock_desc(b)
                lock_sizes[n][(d["format"], d["width"], d["height"], d["size"], d["lock_flags"])] += 1
        if n == "IDirect3DSwapChain9::Present" or n == "IDirect3DDevice9::Present":
            frames += 1
        per_frame[frames] += 1
        if nrec <= args.dump:
            res = f" -> 0x{r.result:08x}" if r.has_result else ""
            blobs = " ".join(f"{BLOB_TAGS.get(t, t)}[{len(b)}]" for t, b in r.blobs)
            print(f"{nrec:7d} f{frames:<5d} d{r.depth} {n}({', '.join(f'0x{w:x}' for w in r.words[1:])}){res} {blobs}")
    total = sum(payload.values())
    print(f"records {nrec}  frames {frames}  nested(depth>0) {nested}  payload {total / 1048576:.1f} MiB")
    print("blob bytes by tag:", {k: f"{v / 1048576:.1f} MiB" for k, v in blob_bytes.most_common()})
    print(f"\n{'method':48s} {'calls':>9s} {'payload MiB':>12s} {'avg bytes':>10s}")
    for n, c in sorted(calls.items(), key=lambda kv: -payload[kv[0]])[: args.top]:
        print(f"{n:48s} {c:9d} {payload[n] / 1048576:12.1f} {payload[n] // c:10d}")
    for n, sizes in lock_sizes.items():
        print(f"\n{n}: top lock shapes (format, width, height, bytes, flags) x count")
        for shape, c in sizes.most_common(8):
            print(f"   {shape} x{c}  = {shape[3] * c / 1048576:.1f} MiB")
    if args.census:
        Path(args.census).write_text(json.dumps({
            "records": nrec, "frames": frames, "calls": dict(calls.most_common()),
            "payload_bytes": dict(payload.most_common()),
            "blob_bytes": dict(blob_bytes.most_common()),
        }, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
