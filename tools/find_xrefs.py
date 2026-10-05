#!/usr/bin/env python3
"""T-009: locate key entry points in BlackOps.exe via import-slot and anchor-string xrefs.

Method: capstone linear sweep of .text; every instruction whose operands contain the VA of
an interesting IAT slot or anchor string is recorded and attributed to a function.
Function attribution uses re/ghidra_export/functions.csv (T-006) when present; otherwise
the start of the 0xCC-padded region containing the instruction (heuristic, marked as such).

Outputs:
  re/xrefs_anchor.csv   kind,name,target_va,insn_va,insn,func_va,func_src
  stdout                per-target summary + WinMain candidate

Run:  .venv/bin/python tools/find_xrefs.py
"""
import bisect
import csv
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"
FUNCS = ROOT / "re" / "ghidra_export" / "functions.csv"
OUT = ROOT / "re" / "xrefs_anchor.csv"

# Whole DLLs whose every import slot is interesting.
DLLS_ALL = {"d3d9.dll", "d3dx9_43.dll", "binkw32.dll", "xinput1_3.dll", "dsound.dll", "steam_api.dll"}
# Individual imports worth anchoring on.
FUNCS_SOME = {
    "CoCreateInstance", "CoInitializeEx", "CreateWindowExA", "RegisterClassExA", "RegisterClassA",
    "PeekMessageA", "GetMessageA", "DispatchMessageA", "SetWindowsHookExA", "ChangeDisplaySettingsA",
    "CreateThread", "VirtualAlloc", "VirtualFree", "CreateFileA", "CreateFileW", "ReadFile", "ReadFileEx",
    "QueryPerformanceCounter", "timeGetTime", "Sleep", "SetUnhandledExceptionFilter", "RaiseException",
    "RtlUnwind", "LoadLibraryA", "GetProcAddress", "TlsAlloc", "GetCommandLineA", "ExitProcess",
    "SetDeviceGammaRamp", "GetAsyncKeyState", "RegOpenKeyExW", "SHGetFolderPathA", "WSAStartup",
    "IsDebuggerPresent", "GetModuleHandleA", "CreateFileMappingA", "MessageBoxA",
}
# Engine log/error strings that name the function or subsystem they live in.
STRINGS = [
    "Getting Direct3D 9 interface...",
    "----- R_Init -----",
    "----- Initializing Renderer ----",
    "----- Client Initialization -----",
    "----- Client Initialization Complete -----",
    "--- Common Initialization Complete ---",
    "----- FS_Startup -----",
    "------ Server Initialization ------",
    "--- Initializing Voice ---",
    "Direct3DDevice9::Present failed: %s (%d)",
    "Incorrect version of Direct3D or D3DX.",
    "Shader model 3.0 not available.",
    "Initializing render targets...",
    "Initializing dynamic buffers...",
    "Initializing static model cache...",
    "DirectX didn't lock a vertex buffer: %s",
    "%s/%s/shadercache%d",
    "%s compilation failed - NULL shader",
    "Couldn't read shader '%s'",
    "Shader compilation failed.",
    "vs_3_0",
    "ps_3_0",
    "Loading fastfile '%s'",
    "Adding fastfile '%s' to queue",
    "IWffu100",
    "IWff0100",
    "zlib decompress %d",
    "%s\\zone\\Common\\%s%s",
    "exec default.cfg",
    "Invalid XAudio2 API call or arguments",
    "ERROR: [play] failed to initialize DirectSound",
    "Hunk_AllocAlign failed on %i bytes (total %i MB, low %i MB, high %i MB)",
    "CL_InitCGame: %5.2f seconds",
    "screenshotJPEG savegame save/%s",
    "STEAM: SteamServer is not VAC Secure!",
    "steam_appid",
    "%s.gsc",
]
HEX = re.compile(r"0x[0-9a-f]+")


def load_functions():
    if not FUNCS.exists():
        return None
    starts, sizes, names = [], [], []
    with FUNCS.open() as f:
        for row in csv.DictReader(f):
            starts.append(int(row["addr"], 16))
            sizes.append(int(row["size"]))
            names.append(row["name"])
    order = sorted(range(len(starts)), key=starts.__getitem__)
    return [starts[i] for i in order], [sizes[i] for i in order], [names[i] for i in order]


def main() -> int:
    raw = EXE.read_bytes()
    pe = pefile.PE(data=raw)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = text.get_data()[: text.Misc_VirtualSize]
    va0 = base + text.VirtualAddress

    targets = {}  # va -> (kind, name)
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode().lower()
        for imp in entry.imports:
            name = imp.name.decode() if imp.name else f"#{imp.ordinal}"
            if dll in DLLS_ALL or name in FUNCS_SOME:
                targets[imp.address] = ("import", f"{dll}!{name}")
    missing = []
    for s in STRINGS:
        needle = s.encode("latin-1")
        hits, pos = [], raw.find(needle)
        while pos >= 0:
            # the literal may carry a prefix ("\n-----..."): anchor on the start of the C string
            start = pos
            while start > 0 and raw[start - 1] >= 0x09 and raw[start - 1] not in (0,) and pos - start < 8:
                start -= 1
            try:
                for off in {start, pos}:
                    hits.append(base + pe.get_rva_from_offset(off))
            except Exception:
                pass
            pos = raw.find(needle, pos + 1)
        if not hits:
            missing.append(s)
        for va in hits:
            targets[va] = ("string", s)

    funcs = load_functions()

    def attribute(va: int):
        if funcs:
            starts, sizes, _ = funcs
            i = bisect.bisect_right(starts, va) - 1
            if i >= 0 and va < starts[i] + max(sizes[i], 1):
                return starts[i], "ghidra"
            if i >= 0:
                return starts[i], "ghidra-nearest"   # outside the body: tail/chunk or unanalysed
        off = va - va0
        # heuristic: walk back to the end of the previous 0xCC padding run
        i = code.rfind(b"\xcc\xcc", 0, off)
        return (va0 + i + 2 if i >= 0 else va0), "heuristic-cc"

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    rows = []
    direct = defaultdict(list)   # direct call/jmp target -> [insn va]
    thunks = {}                  # va of `jmp dword ptr [iat]` stub -> (kind, name)
    off, size = 0, len(code)
    lo, hi = min(targets), max(targets)
    while off < size:
        last = off
        for addr, sz, mn, ops in md.disasm_lite(code[off:], va0 + off):
            last = addr - va0 + sz
            if "0x" not in ops:
                continue
            if mn in ("call", "jmp") and ops.startswith("0x"):
                direct[int(ops, 16)].append((addr, f"{mn} {ops}"))
                continue
            for h in HEX.findall(ops):
                v = int(h, 16)
                if lo <= v <= hi and v in targets:
                    kind, name = targets[v]
                    fva, src = attribute(addr)
                    if mn == "jmp" and ops == f"dword ptr [{h}]" and kind == "import":
                        thunks[addr] = (kind, name)      # import thunk: report its callers instead
                        continue
                    rows.append((kind, name, v, addr, f"{mn} {ops}", fva, src))
        off = last + 1 if last == off or last < size else last

    # Calls that reach an import through its `jmp dword ptr [iat]` thunk.
    for tva, (kind, name) in thunks.items():
        for addr, insn in direct.get(tva, []):
            fva, src = attribute(addr)
            rows.append((kind, name, tva, addr, insn + " (via thunk)", fva, src))
    # Data refs to anchor strings from tables in .rdata/.data are not code and are not listed.
    rows.sort(key=lambda r: (r[0], r[1], r[3]))
    with OUT.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "name", "target_va", "insn_va", "insn", "func_va", "func_src"])
        for kind, name, v, addr, insn, fva, src in rows:
            w.writerow([kind, name, f"0x{v:08x}", f"0x{addr:08x}", insn, f"0x{fva:08x}", src])

    by = defaultdict(list)
    for r in rows:
        by[(r[0], r[1])].append(r)
    print(f"targets: {len(targets)}  xrefs: {len(rows)}  attribution: {'ghidra' if funcs else 'heuristic'}")
    for (kind, name), rs in sorted(by.items()):
        fset = sorted({r[5] for r in rs})
        shown = ", ".join(f"0x{x:08x}" for x in fset[:6]) + (" …" if len(fset) > 6 else "")
        print(f"{kind:6s} {name[:58]:58s} refs={len(rs):3d} funcs={len(fset):3d}  {shown}")
    unref = sorted(n for va, (k, n) in targets.items() if (k, n) not in by)
    if unref:
        print("no code xref (referenced from data tables or indirectly):", "; ".join(unref))
    if missing:
        print("anchor strings not found in file:", missing)

    # WinMain candidate: the CRT startup pushes hInstance (= image base) right before the call.
    ep = base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    print(f"entry point 0x{ep:08x}")
    needle = b"\x68" + struct.pack("<I", base)  # push 0x00400000
    pos = code.find(needle)
    while pos >= 0:
        window = code[pos: pos + 16]
        for addr, sz, mn, ops in md.disasm_lite(window, va0 + pos):
            if mn == "call" and ops.startswith("0x"):
                fva, src = attribute(va0 + pos)
                print(f"WinMain candidate: call {ops} at 0x{addr:08x} (after push 0x{base:08x}), "
                      f"caller func 0x{fva:08x} [{src}]")
                break
        pos = code.find(needle, pos + 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
