#!/usr/bin/env python3
"""T-002/T-003: PE inventory + import census for the game binaries in reference/.

Outputs (all derived, safe to commit — no code or data bytes from the binaries):
  re/inventory.json   full per-file facts
  re/imports.csv      file,dll,function,ordinal,category
  re/inventory.md     human-readable summary (pasted into docs/RE_NOTES.md)

Run:  .venv/bin/python tools/pe_inventory.py
"""
import csv
import hashlib
import json
import math
import struct
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "reference" / "Call of Duty Black Ops"
OUT = ROOT / "re"
# Redistributable installers are not part of the game's code.
SKIP_DIRS = {"Redist", "~Common Redist"}
PE_SUFFIXES = {".exe", ".dll", ".cdx"}

# Import DLL -> subsystem category (drives the P2 platform map).
CATEGORIES = {
    "d3d9.dll": "gfx", "d3dx9_": "gfx", "d3d10": "gfx", "d3d11": "gfx", "dxgi": "gfx",
    "ddraw.dll": "gfx", "opengl32.dll": "gfx", "gdi32.dll": "gdi",
    "dsound.dll": "audio", "xaudio2": "audio", "x3daudio": "audio", "winmm.dll": "audio/timer",
    "mss32.dll": "audio", "openal32.dll": "audio", "xapofx": "audio",
    "dinput8.dll": "input", "dinput.dll": "input", "xinput": "input", "hid.dll": "input",
    "ws2_32.dll": "net", "wsock32.dll": "net", "wininet.dll": "net", "iphlpapi.dll": "net",
    "winhttp.dll": "net", "dnsapi.dll": "net",
    "steam_api.dll": "steam", "binkw32.dll": "video",
    "kernel32.dll": "os", "user32.dll": "window", "advapi32.dll": "os/registry",
    "shell32.dll": "os/shell", "ole32.dll": "com", "oleaut32.dll": "com", "version.dll": "os",
    "psapi.dll": "os", "dbghelp.dll": "debug", "imm32.dll": "input/ime", "shlwapi.dll": "os/shell",
    "msvcr": "crt", "msvcp": "crt", "comctl32.dll": "window", "comdlg32.dll": "window",
    "setupapi.dll": "os", "wintrust.dll": "os/crypto", "crypt32.dll": "os/crypto",
    "netapi32.dll": "net", "rpcrt4.dll": "os", "mpr.dll": "net", "powrprof.dll": "os",
}

# Rich-header product ids we care about (MSVC toolchains of the era).
RICH_PRODUCTS = {
    0x0001: "import (linker-generated)",
    0x005D: "VS2003 C", 0x005F: "VS2003 C++", 0x0060: "VS2003 linker?",
    0x006D: "VS2005 C", 0x006E: "VS2005 C++", 0x0078: "VS2005 link", 0x007B: "VS2005 import",
    0x007C: "VS2005 export", 0x007D: "VS2005 MASM", 0x0083: "VS2008 C", 0x0084: "VS2008 C++",
    0x0091: "VS2008 link", 0x0092: "VS2008 export", 0x0093: "VS2008 import", 0x0094: "VS2008 cvtres",
    0x0095: "VS2008 MASM", 0x009A: "VS2008 cvtres?", 0x009E: "VS2010 MASM", 0x00AA: "VS2010 C",
    0x00AB: "VS2010 C++", 0x009D: "VS2010 link", 0x009B: "VS2010 import", 0x009C: "VS2010 export",
}

SUBSYSTEMS = {2: "GUI", 3: "CUI"}
DATA_DIRS = ["EXPORT", "IMPORT", "RESOURCE", "EXCEPTION", "SECURITY", "BASERELOC", "DEBUG",
             "ARCHITECTURE", "GLOBALPTR", "TLS", "LOAD_CONFIG", "BOUND_IMPORT", "IAT",
             "DELAY_IMPORT", "COM_DESCRIPTOR", "RESERVED"]


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = Counter(data)
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts.values())


def categorize(dll: str) -> str:
    d = dll.lower()
    for key, cat in CATEGORIES.items():
        if d == key or d.startswith(key):
            return cat
    return "other"


def section_flags(ch: int) -> str:
    return "".join(f for bit, f in ((0x40000000, "R"), (0x80000000, "W"), (0x20000000, "X"),
                                    (0x20, "c"), (0x40, "d"), (0x80, "u")) if ch & bit)


def rich_header(pe: pefile.PE):
    rh = pe.parse_rich_header()
    if not rh:
        return None
    vals = rh["values"]
    entries = []
    for i in range(0, len(vals), 2):
        comp, count = vals[i], vals[i + 1]
        prod, build = comp >> 16, comp & 0xFFFF
        entries.append({"prod_id": prod, "build": build, "count": count,
                        "name": RICH_PRODUCTS.get(prod, f"prod 0x{prod:04x}")})
    return {"checksum": f"0x{rh['checksum']:08x}", "entries": entries}


def inventory(path: Path) -> dict:
    raw = path.read_bytes()
    pe = pefile.PE(data=raw, fast_load=False)
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    info = {
        "file": str(path.relative_to(GAME)),
        "size": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "machine": f"0x{fh.Machine:04x}",
        "pe32plus": oh.Magic == 0x20B,
        "timestamp": fh.TimeDateStamp,
        "timestamp_utc": datetime.fromtimestamp(fh.TimeDateStamp, timezone.utc).isoformat(),
        "characteristics": f"0x{fh.Characteristics:04x}",
        "large_address_aware": bool(fh.Characteristics & 0x20),
        "relocs_stripped": bool(fh.Characteristics & 0x1),
        "is_dll": bool(fh.Characteristics & 0x2000),
        "linker": f"{oh.MajorLinkerVersion}.{oh.MinorLinkerVersion}",
        "image_base": f"0x{oh.ImageBase:08x}",
        "entry_rva": f"0x{oh.AddressOfEntryPoint:08x}",
        "entry_va": f"0x{oh.ImageBase + oh.AddressOfEntryPoint:08x}",
        "size_of_image": oh.SizeOfImage,
        "size_of_code": oh.SizeOfCode,
        "subsystem": SUBSYSTEMS.get(oh.Subsystem, str(oh.Subsystem)),
        "dll_characteristics": f"0x{oh.DllCharacteristics:04x}",
        "aslr": bool(oh.DllCharacteristics & 0x40),
        "nx_compat": bool(oh.DllCharacteristics & 0x100),
        "no_seh": bool(oh.DllCharacteristics & 0x400),
        "stack_reserve": oh.SizeOfStackReserve,
        "heap_reserve": oh.SizeOfHeapReserve,
        "checksum_header": f"0x{oh.CheckSum:08x}",
        "checksum_computed": f"0x{pe.generate_checksum():08x}",
    }

    entry_section = None
    sections = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode("latin-1")
        data = s.get_data()
        sec = {
            "name": name,
            "va": f"0x{oh.ImageBase + s.VirtualAddress:08x}",
            "vsize": s.Misc_VirtualSize,
            "raw_off": s.PointerToRawData,
            "raw_size": s.SizeOfRawData,
            "flags": section_flags(s.Characteristics),
            "characteristics": f"0x{s.Characteristics:08x}",
            "entropy": round(entropy(data), 3),
        }
        if s.VirtualAddress <= oh.AddressOfEntryPoint < s.VirtualAddress + max(s.Misc_VirtualSize, 1):
            entry_section = name
        sections.append(sec)
    info["sections"] = sections
    info["entry_section"] = entry_section

    info["data_dirs"] = {
        DATA_DIRS[i]: {"rva": f"0x{d.VirtualAddress:08x}", "size": d.Size}
        for i, d in enumerate(oh.DATA_DIRECTORY) if d.Size
    }

    end_of_sections = max((s.PointerToRawData + s.SizeOfRawData for s in pe.sections), default=0)
    sec_dir = oh.DATA_DIRECTORY[4]
    info["overlay"] = {"offset": end_of_sections, "size": len(raw) - end_of_sections}
    # The certificate table is addressed by FILE OFFSET, and normally *is* the overlay.
    info["authenticode"] = {"present": sec_dir.Size > 0, "file_offset": sec_dir.VirtualAddress,
                            "size": sec_dir.Size,
                            "covers_overlay": sec_dir.VirtualAddress == end_of_sections
                            and sec_dir.Size >= len(raw) - end_of_sections - 8}

    imports = []
    for attr, kind in (("DIRECTORY_ENTRY_IMPORT", "static"), ("DIRECTORY_ENTRY_DELAY_IMPORT", "delay")):
        for entry in getattr(pe, attr, []) or []:
            dll = entry.dll.decode("latin-1")
            funcs = [{"name": i.name.decode("latin-1") if i.name else None, "ordinal": i.ordinal,
                      "iat_va": f"0x{i.address:08x}"} for i in entry.imports]
            imports.append({"dll": dll, "kind": kind, "category": categorize(dll), "functions": funcs})
    info["imports"] = imports
    info["import_count"] = sum(len(i["functions"]) for i in imports)

    exports = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            exports.append({"name": e.name.decode("latin-1") if e.name else None, "ordinal": e.ordinal,
                            "rva": f"0x{e.address:08x}",
                            "forwarder": e.forwarder.decode("latin-1") if e.forwarder else None})
    info["exports"] = exports
    info["export_count"] = len(exports)

    tls = None
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        t = pe.DIRECTORY_ENTRY_TLS.struct
        callbacks = []
        if t.AddressOfCallBacks:
            off = pe.get_offset_from_rva(t.AddressOfCallBacks - oh.ImageBase)
            while off + 4 <= len(raw):
                (cb,) = struct.unpack_from("<I", raw, off)
                if not cb:
                    break
                callbacks.append(f"0x{cb:08x}")
                off += 4
        tls = {"start_va": f"0x{t.StartAddressOfRawData:08x}", "end_va": f"0x{t.EndAddressOfRawData:08x}",
               "template_size": t.EndAddressOfRawData - t.StartAddressOfRawData,
               "zero_fill": t.SizeOfZeroFill, "index_va": f"0x{t.AddressOfIndex:08x}",
               "callbacks": callbacks}
    info["tls"] = tls

    debug = []
    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []) or []:
        item = {"type": d.struct.Type, "timestamp": d.struct.TimeDateStamp, "size": d.struct.SizeOfData}
        pdb = getattr(getattr(d, "entry", None), "PdbFileName", None)
        if pdb:
            item["pdb"] = pdb.rstrip(b"\0").decode("latin-1")
        debug.append(item)
    info["debug"] = debug

    lc = getattr(pe, "DIRECTORY_ENTRY_LOAD_CONFIG", None)
    if lc:
        s = lc.struct
        info["load_config"] = {"security_cookie_va": f"0x{getattr(s, 'SecurityCookie', 0):08x}",
                               "se_handler_table_va": f"0x{getattr(s, 'SEHandlerTable', 0):08x}",
                               "se_handler_count": getattr(s, "SEHandlerCount", 0)}

    relocs = 0
    for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []) or []:
        relocs += sum(1 for e in block.entries if e.type != 0)
    info["reloc_count"] = relocs

    version = {}
    for fi in getattr(pe, "FileInfo", []) or []:
        for entry in fi:
            for st in getattr(entry, "StringTable", []) or []:
                for k, v in st.entries.items():
                    version[k.decode("latin-1")] = v.decode("latin-1")
    info["version_info"] = version
    info["rich"] = rich_header(pe)
    return info


def summarize(items) -> str:
    out = []
    out.append("| File | SHA-256 | Size | Arch | Linker / built (UTC) | Image base | Entry VA | Notes |")
    out.append("|---|---|---|---|---|---|---|---|")
    for i in items:
        notes = []
        if i["authenticode"]["present"]:
            notes.append("Authenticode table present")
        if i["tls"]:
            notes.append(f"TLS ({len(i['tls']['callbacks'])} callbacks)")
        notes.append("relocs stripped" if i["relocs_stripped"] else f"{i['reloc_count']} relocs")
        notes.append("LAA" if i["large_address_aware"] else "no LAA")
        if i["overlay"]["size"]:
            notes.append(f"overlay {i['overlay']['size']} B")
        out.append(f"| {i['file']} | `{i['sha256']}` | {i['size']:,} | "
                   f"{'PE32+' if i['pe32plus'] else 'PE32'} {i['machine']} {i['subsystem']} | "
                   f"link {i['linker']} / {i['timestamp_utc'][:10]} | {i['image_base']} | {i['entry_va']} "
                   f"(`{i['entry_section']}`) | {'; '.join(notes)} |")
    for i in items:
        out.append(f"\n**{i['file']} — sections**\n")
        out.append("| Name | VA | VSize | RawSize | Flags | Entropy |")
        out.append("|---|---|---|---|---|---|")
        for s in i["sections"]:
            out.append(f"| `{s['name']}` | {s['va']} | {s['vsize']:,} | {s['raw_size']:,} | {s['flags']} | {s['entropy']} |")
        if i["rich"]:
            agg = defaultdict(int)
            for e in i["rich"]["entries"]:
                agg[(e["name"], e["build"])] += e["count"]
            top = sorted(agg.items(), key=lambda kv: -kv[1])[:8]
            out.append("\nRich header (top by object count): " +
                       ", ".join(f"{n} build {b} ×{c}" for (n, b), c in top))
        else:
            out.append("\nRich header: none")
        imps = ", ".join(f"{d['dll']} ({len(d['functions'])}{', delay' if d['kind'] == 'delay' else ''})"
                         for d in i["imports"])
        out.append(f"\nImports ({i['import_count']}): {imps or 'none'}")
        out.append(f"\nExports: {i['export_count']}")
        if i["version_info"]:
            vi = i["version_info"]
            out.append("\nVersion info: " + "; ".join(f"{k}={vi[k]}" for k in
                       ("CompanyName", "ProductName", "FileDescription", "FileVersion", "OriginalFilename")
                       if k in vi))
    return "\n".join(out) + "\n"


def main() -> int:
    if not GAME.is_dir():
        print(f"missing {GAME}", file=sys.stderr)
        return 1
    files = sorted(p for p in GAME.rglob("*") if p.is_file() and p.suffix.lower() in PE_SUFFIXES
                   and not (set(p.relative_to(GAME).parts) & SKIP_DIRS))
    items = [inventory(p) for p in files]
    OUT.mkdir(exist_ok=True)
    (OUT / "inventory.json").write_text(json.dumps(items, indent=1) + "\n")
    with (OUT / "imports.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", "dll", "kind", "category", "function", "ordinal", "iat_va"])
        for i in items:
            for d in i["imports"]:
                for fn in d["functions"]:
                    w.writerow([i["file"], d["dll"], d["kind"], d["category"], fn["name"] or "",
                                fn["ordinal"] if fn["name"] is None else "", fn["iat_va"]])
    (OUT / "inventory.md").write_text(summarize(items))
    print(summarize(items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
