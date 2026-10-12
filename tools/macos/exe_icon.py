#!/usr/bin/env python3
"""The application icon of a Windows executable as a macOS .icns (P8, scripts/make_app.sh).

  .venv/bin/python tools/macos/exe_icon.py <BlackOps.exe> <out.icns>

Reads the first icon group of the executable's resources, rebuilds the .ico file it stands for, takes its
largest image and writes the sizes an .icns needs (through `iconutil`). The icon comes from the user's own copy
of the game at build time; it is not stored in the repository.
"""
import io
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import pefile
from PIL import Image

RT_ICON, RT_GROUP_ICON = 3, 14


def resources(pe, kind):
    out = {}
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id != kind:
            continue
        for e in t.directory.entries:
            d = e.directory.entries[0].data.struct
            out[e.id if e.id is not None else str(e.name)] = pe.get_data(d.OffsetToData, d.Size)
    return out


def main():
    exe, out = sys.argv[1], Path(sys.argv[2])
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    groups, icons = resources(pe, RT_GROUP_ICON), resources(pe, RT_ICON)
    if not groups:
        sys.exit("no icon in " + exe)
    grp = groups[sorted(groups, key=str)[0]]
    _, _, count = struct.unpack_from("<HHH", grp, 0)
    entries, blob, offset = b"", b"", 6 + 16 * count
    for i in range(count):
        w, h, colors, res, planes, bpp, size, rid = struct.unpack_from("<BBBBHHIH", grp, 6 + 14 * i)
        data = icons[rid]
        entries += struct.pack("<BBBBHHII", w, h, colors, res, planes, bpp, len(data), offset + len(blob))
        blob += data
    ico = Image.open(io.BytesIO(struct.pack("<HHH", 0, 1, count) + entries + blob))
    best = max(ico.ico.sizes())
    img = ico.ico.getimage(best).convert("RGBA")
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "icon.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                img.resize((px, px), Image.LANCZOS).save(iconset / name)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(out)], check=True)
    print(f"icon: {count} images in the executable, largest {best[0]}x{best[1]} -> {out}")


if __name__ == "__main__":
    main()
