#!/usr/bin/env python3
"""Which XAudio2 methods the game called in a native run (T-053) -> re/xaudio2_usage.csv.

  BO1_IMPORT_STATS=1 SDL_AUDIO_DRIVER=dummy artifacts/boot/bo1_boot --seconds N > run.log 2>&1
  .venv/bin/python tools/xaudio2_usage.py run.log [more.log ...]

Reads the "import calls" table the runtime prints at exit (one line per called trampoline; COM methods
appear as com!<interface>::<method>) and writes interface, slot, method, stack words (from the vtable
tables in src/platform/win32/xaudio2_faudio.cpp) and the call count summed over the given logs. Methods
that were never called are listed with 0, so the table also shows what is still unexercised.
"""
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "platform" / "win32" / "xaudio2_faudio.cpp"


def tables():
    text = SRC.read_text()
    out = {}
    for name, iface in (("kEngineMethods", "IXAudio2"), ("kVoiceMethods", "IXAudio2Voice")):
        body = text[text.index(f"const ComMethod {name}[]"):]
        body = body[:body.index("};")]
        out[iface] = [(m.group(1), int(m.group(2))) for m in re.finditer(r'\{"(\w+)", (\d+), \w+\}', body)]
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    calls = {}
    for path in sys.argv[1:]:
        for line in Path(path).read_bytes().decode("latin1").split("\n"):
            m = re.match(r"\s+(\d+)\s+com!(IXAudio2\w*)::(\w+)", line)
            if m:
                key = (m.group(2), m.group(3))
                calls[key] = calls.get(key, 0) + int(m.group(1))
    t = tables()
    rows = []
    for slot, (method, words) in enumerate(t["IXAudio2"]):
        rows.append(("IXAudio2", slot, method, words, calls.get(("IXAudio2", method), 0)))
    for iface, limit in (("IXAudio2MasteringVoice", 19), ("IXAudio2SubmixVoice", 19), ("IXAudio2SourceVoice", 29)):
        for slot, (method, words) in enumerate(t["IXAudio2Voice"][:limit]):
            rows.append((iface, slot, method, words, calls.get((iface, method), 0)))
    with open(ROOT / "re" / "xaudio2_usage.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["interface", "slot", "method", "words", "calls"])
        w.writerows(rows)
    used = [r for r in rows if r[4]]
    print(f"{len(used)} of {len(rows)} methods called; wrote re/xaudio2_usage.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
