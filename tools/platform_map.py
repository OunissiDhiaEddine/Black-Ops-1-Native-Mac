#!/usr/bin/env python3
"""T-020: platform map — every import of BlackOps.exe -> {shim | stub | not needed} + host plan.

Inputs
  re/imports.csv                              static import table (tools/pe_inventory.py)
  artifacts/baseline/import_census-*.csv      runtime call counts from the proxy (optional, any number)
  the exe's .text                             static call-site counts per import slot
Outputs
  re/platform_map.csv     one row per import: dll,function,subsystem,disposition,static_sites,
                          runtime_calls,runs_seen,host_plan
  stdout                  markdown summary for docs/RE_NOTES.md § Platform map

Dispositions
  shim        real host implementation required (src/platform)
  stub        must exist, but a constant/empty answer is enough
  not needed  no call site reaches it in the game's code (static) — provide a trap that logs
The classification rules below are the reviewed, hand-written part; everything else is measured.

Run:  .venv/bin/python tools/platform_map.py
"""
import csv
import glob
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "reference" / "Call of Duty Black Ops" / "BlackOps.exe"

# (regex on "dll!function", subsystem, disposition, host plan). First match wins.
RULES = [
    # --- graphics ---------------------------------------------------------------------------
    (r"d3d9\.dll!Direct3DCreate9", "gfx", "shim", "src/rhi + src/metal: guest-visible IDirect3D9/Device9 COM objects (docs/HOST_ABI.md §4)"),
    (r"d3d9\.dll!D3DPERF_", "gfx", "stub", "return 0 (optionally forward to MTLCommandBuffer debug groups)"),
    (r"d3dx9_43\.dll!D3DXCompileShader", "gfx", "stub", "never called at runtime (census); return E_FAIL and log"),
    (r"d3dx9_43\.dll!D3DXGetShader(ConstantTable|InputSemantics|OutputSemantics)", "gfx", "shim", "small bytecode parsers (CTAB / dcl tokens) — needed if the census shows calls"),
    (r"d3dx9_43\.dll!D3DXCreateBuffer", "gfx", "shim", "guest-visible ID3DXBuffer over guest heap memory"),
    (r"d3dx9_43\.dll!D3DXSaveSurfaceToFileA", "gfx", "shim", "screenshots: read back the Metal texture, encode with ImageIO"),
    (r"gdi32\.dll!SetDeviceGammaRamp", "gfx", "shim", "apply as a present-time gamma LUT in the Metal backend (do not touch the system gamma)"),
    (r"gdi32\.dll!", "gdi", "stub", "fake DC handle / fixed device caps; fonts and brushes unused by the renderer"),
    # --- video / audio / input middleware -----------------------------------------------------
    (r"binkw32\.dll!", "video", "shim", "Bink API re-implemented over FFmpeg's bink decoder (ADR-008)"),
    (r"dsound\.dll!", "audio", "stub", "DirectSoundCreate8 / CaptureCreate -> DSERR_NODRIVER: voice chat off, Bink audio goes through the XAudio2 shim"),
    (r"winmm\.dll!time(GetTime|BeginPeriod|EndPeriod)", "time", "shim", "mach_absolute_time based millisecond clock; periods are no-ops"),
    (r"winmm\.dll!(mixer|waveIn)", "audio", "stub", "no mixer / capture devices: MMSYSERR_NODRIVER, 0 devices"),
    (r"xinput1_3\.dll!", "input", "shim", "XInputGetState/SetState/GetCapabilities over SDL3 gamepad (GameController.framework underneath)"),
    (r"steam_api\.dll!", "steam", "shim", "bridge to the real Steam client through the Steamworks macOS library — no emulation of ownership/login (ADR-010)"),
    # --- COM ----------------------------------------------------------------------------------
    (r"ole32\.dll!CoCreateInstance", "com", "shim", "CLSID dispatch: XAudio2 2.7 -> FAudio-backed guest COM object (ADR-009); anything else REGDB_E_CLASSNOTREG"),
    (r"ole32\.dll!Co(Initialize|Uninitialize)", "com", "stub", "S_OK"),
    (r"ole32\.dll!CoTaskMem", "mem", "shim", "guest heap"),
    (r"ole32\.dll!(StringFromGUID2|CLSIDFromString)", "com", "shim", "pure string <-> GUID conversion"),
    # --- network ------------------------------------------------------------------------------
    (r"(wsock32|ws2_32)\.dll!", "net", "shim", "thin Winsock -> BSD sockets layer (error-code translation); policy switch to run fully offline"),
    # --- window / input -----------------------------------------------------------------------
    (r"user32\.dll!(SetWindowsHookExA|CallNextHookEx)", "window", "stub", "hook handle that never fires (used to swallow system keys)"),
    (r"user32\.dll!(OpenClipboard|CloseClipboard|GetClipboardData)", "window", "shim", "NSPasteboard text via SDL clipboard"),
    (r"user32\.dll!MessageBox", "window", "shim", "native alert (NSAlert / SDL_ShowMessageBox); auto-answer mode for tests"),
    (r"user32\.dll!(ChangeDisplaySettingsA|EnumDisplayMonitors|GetMonitorInfoA|MonitorFromPoint|MonitorFromWindow|GetSystemMetrics|GetDC|ReleaseDC|GetDesktopWindow)", "display", "shim", "display enumeration / desktop size from SDL; mode switches become window resizes"),
    (r"user32\.dll!(GetAsyncKeyState|MapVirtualKeyA|GetCursorPos|SetCursorPos|ShowCursor|ClientToScreen|ScreenToClient)", "input", "shim", "keyboard state table + cursor in window coordinates (SDL events -> Win32 semantics)"),
    (r"user32\.dll!(LoadCursorA|LoadIconA|LoadImageA)", "window", "stub", "opaque non-null handles"),
    (r"user32\.dll!", "window", "shim", "one emulated HWND + Win32 message queue fed by SDL events; the game's WndProc is called back as guest code"),
    # --- registry / crypto / security -----------------------------------------------------------
    (r"advapi32\.dll!Reg", "registry", "shim", "tiny read-only registry (install path, language) from a config file"),
    (r"advapi32\.dll!Crypt", "crypto", "shim", "CryptGenRandom -> arc4random_buf; contexts are dummy handles"),
    (r"advapi32\.dll!", "security", "stub", "token/privilege/descriptor calls succeed and do nothing"),
    (r"shell32\.dll!SHGetFolderPathA", "fs", "shim", "CSIDL -> ~/Library/Application Support/<app>/… mapped as a guest path"),
    (r"shell32\.dll!ShellExecuteA", "os", "stub", "log (opens URLs / help files)"),
    (r"psapi\.dll!", "os", "stub", "plausible fixed process memory numbers"),
    # --- kernel32 -----------------------------------------------------------------------------
    (r"kernel32\.dll!(CreateFile[AW]|ReadFile|ReadFileEx|WriteFile|SetFilePointer|SetEndOfFile|FlushFileBuffers|GetFileSize|GetFileTime|GetFileType|GetFileAttributesA|SetFileAttributesA|GetFileInformationByHandle|DeleteFileA|MoveFileA|CreateDirectoryA|FindFirstFileA|FindNextFileA|FindClose|GetFullPathName[AW]|GetCurrentDirectory[AW]|GetDriveTypeA|GetStdHandle|SetStdHandle|SetHandleCount)$", "fs", "shim", "POSIX file layer: case-insensitive, backslash paths, install root + writable overlay; ReadFileEx = async read + completion callback queued for SleepEx"),
    (r"kernel32\.dll!(CreateFileMappingA|OpenFileMappingA|MapViewOfFile|UnmapViewOfFile)", "fs", "shim", "mmap into the guest arena (named mappings = process-local table)"),
    (r"kernel32\.dll!(CloseHandle|DuplicateHandle)", "handles", "shim", "one handle table for files, threads, events, mutexes, semaphores, mappings"),
    (r"kernel32\.dll!(CreateThread|ExitThread|GetCurrentThread|GetCurrentThreadId|GetExitCodeThread|ResumeThread|SuspendThread|TerminateThread|SetThreadPriority|GetThreadPriority|SwitchToThread|Sleep|SleepEx)$", "thread", "shim", "pthread per guest thread with its own CPU state, guest stack and TEB; SleepEx drains queued completion callbacks"),
    (r"kernel32\.dll!(SetThreadExecutionState|SetPriorityClass|SetProcessAffinityMask)", "thread", "stub", "succeed, no effect"),
    (r"kernel32\.dll!(GetProcessAffinityMask|GetSystemInfo|GetProcessTimes|GetVersionExA|GetComputerNameA|GlobalMemoryStatus)", "sysinfo", "shim", "stable, sane answers (core count, 'Windows 7', memory capped below 4 GiB) — the game's auto-config depends on them"),
    (r"kernel32\.dll!(InitializeCriticalSection|InitializeCriticalSectionAndSpinCount|DeleteCriticalSection|EnterCriticalSection|LeaveCriticalSection|TryEnterCriticalSection)", "sync", "shim", "recursive mutex stored in the guest CRITICAL_SECTION's 24 bytes (index into a host table)"),
    (r"kernel32\.dll!(CreateEvent[AW]|OpenEventA|SetEvent|ResetEvent|PulseEvent|CreateMutexA|ReleaseMutex|CreateSemaphoreA|ReleaseSemaphore|WaitForSingleObject)", "sync", "shim", "event/mutex/semaphore objects on pthread cond vars; WaitForSingleObject also waits on thread handles"),
    (r"kernel32\.dll!Interlocked", "sync", "shim", "C11 atomics on guest memory (the lifter may inline these)"),
    (r"kernel32\.dll!Tls", "thread", "shim", "per-thread slot array in the emulated TEB"),
    (r"kernel32\.dll!(VirtualAlloc|VirtualFree|VirtualQuery|HeapCreate|HeapAlloc|HeapFree|HeapReAlloc|HeapSize|GetProcessHeap|GlobalLock|GlobalUnlock|GlobalSize)", "mem", "shim", "allocator over the 4 GiB guest arena (reserve/commit semantics; must satisfy one 300 MiB commit)"),
    (r"kernel32\.dll!(QueryPerformanceCounter|QueryPerformanceFrequency|GetTickCount|GetSystemTime|GetSystemTimeAsFileTime|GetTimeZoneInformation|FileTimeToLocalFileTime|FileTimeToSystemTime)", "time", "shim", "monotonic + wall clock conversions"),
    (r"kernel32\.dll!(RaiseException|RtlUnwind|SetUnhandledExceptionFilter|UnhandledExceptionFilter)", "seh", "shim", "guest SEH chain walk / unwind (docs/HOST_ABI.md §7) — the hard one"),
    (r"kernel32\.dll!(OutputDebugStringA|IsDebuggerPresent|DebugBreak|SetErrorMode|FormatMessageA|GetLastError|SetLastError)", "debug", "shim", "last-error in the TEB; OutputDebugString -> host log (this is the engine console); IsDebuggerPresent = 0"),
    (r"kernel32\.dll!(GetModuleHandle[AW]|GetModuleHandleExA|GetModuleFileName[AW]|GetCommandLine[AW]|GetEnvironmentStrings|GetEnvironmentStringsW|FreeEnvironmentStrings[AW]|SetEnvironmentVariable[AW]|GetStartupInfo[AW]|ExitProcess|TerminateProcess|GetCurrentProcess|GetCurrentProcessId|GetSystemDirectoryW|GetWindowsDirectoryW)", "process", "shim", "process identity: fake module handle 0x400000, synthetic command line and environment"),
    (r"kernel32\.dll!(LoadLibraryA|FreeLibrary|GetProcAddress)", "process", "shim", "name -> host shim table for the DLLs we provide; unknown DLL/function fails cleanly and is logged"),
    (r"kernel32\.dll!(OpenProcess|CreateToolhelp32Snapshot|Module32First|Module32Next)", "process", "stub", "empty snapshot / self handle only"),
    (r"kernel32\.dll!(FindResourceW|LoadResource|LockResource|SizeofResource)", "process", "shim", "serve the exe's .rsrc section from the mapped image"),
    (r"kernel32\.dll!(CompareString[AW]|GetACP|GetOEMCP|GetCPInfo|IsValidCodePage|GetLocaleInfoA|GetStringType[AW]|LCMapString[AW]|MultiByteToWideChar|WideCharToMultiByte|lstrcmpiW|lstrlenW|MulDiv)", "nls", "shim", "code page 1252 / en-US only; used by the statically linked CRT"),
    (r"kernel32\.dll!(GetConsoleCP|GetConsoleMode|GetConsoleOutputCP|GetConsoleWindow|WriteConsole[AW])", "console", "stub", "no console attached"),
]


def classify(dll: str, fn: str):
    key = f"{dll.lower()}!{fn}"
    for pat, subsystem, disp, plan in RULES:
        if re.search(pat, key):
            return subsystem, disp, plan
    return "UNCLASSIFIED", "?", ""


def static_sites():
    """Count code references to each IAT slot, following `jmp [slot]` thunks to their callers."""
    raw = EXE.read_bytes()
    pe = pefile.PE(data=raw)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = text.get_data()[: text.Misc_VirtualSize]
    va0 = base + text.VirtualAddress
    slots = {}
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        for i in e.imports:
            # pefile resolves well-known ordinals to names; keep the "by ordinal" fact for matching
            by_ord = bool(getattr(i, "import_by_ordinal", False)) or not i.name
            slots[i.address] = (e.dll.decode().lower(), None if by_ord else i.name.decode(), i.ordinal if by_ord else None)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    refs, thunks, direct = Counter(), {}, Counter()
    hexre = re.compile(r"0x[0-9a-f]+")
    lo, hi = min(slots), max(slots)
    off, size = 0, len(code)
    while off < size:
        last = off
        for addr, sz, mn, ops in md.disasm_lite(code[off:], va0 + off):
            last = addr - va0 + sz
            if "0x" not in ops:
                continue
            if mn in ("call", "jmp") and ops.startswith("0x"):
                direct[int(ops, 16)] += 1
                continue
            for h in hexre.findall(ops):
                v = int(h, 16)
                if lo <= v <= hi and v in slots:
                    if mn == "jmp" and ops == f"dword ptr [{h}]":
                        thunks[addr] = v
                    else:
                        refs[v] += 1
        off = last + 1 if last == off or last < size else last
    for tva, slot in thunks.items():
        refs[slot] += direct.get(tva, 0)
    return {slots[k]: refs.get(k, 0) for k in slots}, slots


def main() -> int:
    sites, slots = static_sites()
    names = {}
    with (ROOT / "re" / "imports.csv").open() as f:
        for r in csv.DictReader(f):
            if r["file"] == "BlackOps.exe":
                names[int(r["iat_va"], 16)] = (r["dll"].lower(), r["function"])
    # runtime counts: union over all census files
    runtime, runs = Counter(), Counter()
    files = sorted(glob.glob(str(ROOT / "artifacts" / "baseline" / "import_census-*.csv")))
    for path in files:
        with open(path, errors="replace") as f:
            for r in csv.DictReader(f):
                name = r["function"] or None
                key = (r["dll"].lower(), name, None if name else int(r["ordinal"]))
                c = int(r["calls"])
                runtime[key] += c
                if c:
                    runs[key] += 1
    rows = []
    for va, (dll, name, ordn) in sorted(slots.items()):
        shown = names.get(va, (dll, name or f"#{ordn}"))[1]
        subsystem, disp, plan = classify(dll, shown)
        st = sites[(dll, name, ordn)]
        rkey = (dll, name, None if name else ordn)
        rt = runtime.get(rkey, 0) if files else None
        if st == 0 and (rt in (0, None)):
            disp_final = "not needed"
        else:
            disp_final = disp
        rows.append({"dll": dll, "function": shown, "subsystem": subsystem, "disposition": disp_final,
                     "planned_if_reached": disp, "static_sites": st,
                     "runtime_calls": "" if rt is None else rt,
                     "runs_seen": "" if rt is None else runs.get(rkey, 0), "host_plan": plan})
    out = ROOT / "re" / "platform_map.csv"
    with out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    unclassified = [r for r in rows if r["subsystem"] == "UNCLASSIFIED"]
    disp = Counter(r["disposition"] for r in rows)
    print(f"Imports: {len(rows)}  ->  " + ", ".join(f"{k}: {v}" for k, v in disp.most_common())
          + f"   (runtime census files: {len(files)})")
    if unclassified:
        print("UNCLASSIFIED:", [f"{r['dll']}!{r['function']}" for r in unclassified])
    sub = defaultdict(list)
    for r in rows:
        sub[r["subsystem"]].append(r)
    print("\n| Subsystem | Imports | shim / stub / not needed | Called at runtime | Runtime calls | Host plan |")
    print("|---|---|---|---|---|---|")
    for s, rs in sorted(sub.items(), key=lambda kv: -sum(int(r["runtime_calls"] or 0) for r in kv[1])):
        d = Counter(r["disposition"] for r in rs)
        called = sum(1 for r in rs if int(r["runtime_calls"] or 0) > 0)
        total = sum(int(r["runtime_calls"] or 0) for r in rs)
        plans = Counter(r["host_plan"] for r in rs if r["disposition"] != "not needed").most_common(1)
        print(f"| {s} | {len(rs)} | {d.get('shim', 0)} / {d.get('stub', 0)} / {d.get('not needed', 0)} | {called} | "
              f"{total:,} | {plans[0][0] if plans else '—'} |")
    if files:
        hot = sorted(rows, key=lambda r: -int(r["runtime_calls"] or 0))[:25]
        print("\nHottest imports (calls over all census runs): " + ", ".join(
            f"{r['function']} {int(r['runtime_calls']):,}" for r in hot))
        never = [r for r in rows if int(r["runtime_calls"] or 0) == 0 and r["static_sites"] > 0]
        print(f"\nReferenced by code but never called in the census runs ({len(never)}): " + ", ".join(
            f"{r['function']}" for r in never))
        dead = [r for r in rows if r["disposition"] == "not needed"]
        print(f"\nNo call site and never called ({len(dead)}): " + ", ".join(r["function"] for r in dead))
    print(f"\n(csv: {out.relative_to(ROOT)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
