#!/usr/bin/env python3
"""Turn the proxy's adapter probe log into the answers the game got from Direct3D 9 (T-045).

  .venv/bin/python tools/adapter_probe.py artifacts/game/adapter_probe.txt   # -> re/d3d9_adapter_probe.json

The log comes from a baseline run (scripts/run_baseline.sh --trace sets BO1_ADAPTER_PROBE): one line
per IDirect3D9 query made before / around device creation. The host D3D9 device (null device in P4,
Metal in P5) replays these answers so that the game takes the same decisions as in the baseline.
Raw structures are kept as hex (D3DCAPS9 = 304 bytes, D3DADAPTER_IDENTIFIER9 = 1100 bytes).
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    src = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "artifacts" / "game" / "adapter_probe.txt")
    out = {"source": str(src.name), "adapter_count": None, "identifier_hex": None, "caps_hex": None, "display_mode": None,
           "modes": {}, "mode_count": {}, "checks": []}
    seen = set()
    for line in src.read_text().splitlines():
        parts = dict(p.split("=", 1) for p in line.split()[1:])
        name = line.split()[0]
        hr = int(parts["hr"], 16)
        args = [int(x, 16) for x in parts["args"].split(",")] if parts.get("args") else []
        blob = bytes.fromhex(parts.get("out", ""))
        if name == "GetAdapterCount":
            out["adapter_count"] = hr
        elif name == "GetAdapterIdentifier" and blob:
            out["identifier_hex"] = blob.hex()
            out["identifier_description"] = blob[512:1024].split(b"\0")[0].decode("latin-1")
        elif name == "GetDeviceCaps" and blob:
            out["caps_hex"] = blob.hex()
        elif name == "GetAdapterDisplayMode" and blob:
            w, h, hz, fmt = struct.unpack("<4I", blob)
            out["display_mode"] = {"width": w, "height": h, "refresh": hz, "format": fmt}
        elif name == "GetAdapterModeCount":
            out["mode_count"][str(args[1])] = hr
        elif name == "EnumAdapterModes" and blob:
            w, h, hz, fmt = struct.unpack("<4I", blob)
            out["modes"].setdefault(str(args[1]), {})[str(args[2])] = [w, h, hz, fmt]
        elif name.startswith("Check"):
            key = (name, tuple(args[:-1] if name == "CheckDeviceMultiSampleType" else args))
            if key in seen:
                continue
            seen.add(key)
            rec = {"method": name, "args": list(key[1]), "hr": f"0x{hr:08x}"}
            if name == "CheckDeviceMultiSampleType" and blob:
                rec["quality_levels"] = struct.unpack("<I", blob)[0]
            out["checks"].append(rec)
    (ROOT / "re" / "d3d9_adapter_probe.json").write_text(json.dumps(out, indent=1) + "\n")
    print(f"adapter \"{out.get('identifier_description')}\": {sum(len(v) for v in out['modes'].values())} modes, "
          f"{len(out['checks'])} distinct capability checks, caps {'present' if out['caps_hex'] else 'MISSING'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
