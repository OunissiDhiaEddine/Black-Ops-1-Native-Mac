#!/usr/bin/env python3
"""Inclusive time below one function in a macOS `sample` report, summed over every place it appears (T-058).

  sample <pid> 10 -file out.txt
  .venv/bin/python tools/sample_tree.py out.txt "MetalRenderer::draw" [depth]

Prints the callees of every frame whose symbol contains the text, merged by name, down to `depth`
levels (default 2), with sample counts (1 sample = 1 ms) and the share of the function's own total.
"""
import re
import sys
from collections import defaultdict

LINE = re.compile(r"^(\s+[+!:| ]*)(\d+) (.+?)(?:  \(in ([^)]+)\).*)?$")


def short(name):
    name = re.sub(r"bo1::rhi::\(anonymous namespace\)::|bo1::\(anonymous namespace\)::", "", name)
    name = re.sub(r"\(.*", "", name) if not name.startswith("-[") else name
    return name[:90]


def main():
    path, needle = sys.argv[1], sys.argv[2]
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    rows = []
    for l in open(path, errors="replace"):
        if l.startswith("Total number in stack") or l.startswith("Sort by top of stack"):
            break
        m = LINE.match(l.rstrip("\n"))
        if m:
            rows.append((len(m.group(1)), int(m.group(2)), short(m.group(3))))
    total = 0
    tree = defaultdict(int)  # path tuple -> samples
    i = 0
    while i < len(rows):
        ind, n, name = rows[i]
        if needle in name:
            total += n
            stack = [(ind, ())]
            j = i + 1
            while j < len(rows) and rows[j][0] > ind:
                d, c, nm = rows[j]
                while stack and stack[-1][0] >= d:
                    stack.pop()
                p = stack[-1][1] + (nm,)
                if len(p) <= depth:
                    tree[p] += c
                stack.append((d, p))
                j += 1
            i = j
        else:
            i += 1
    print(f"{needle}: {total} samples")

    def show(prefix, level):
        kids = sorted(((c, p) for p, c in tree.items() if len(p) == level + 1 and p[:level] == prefix), reverse=True)
        for c, p in kids:
            if c * 200 < total:
                continue
            print(f"{'  ' * (level + 1)}{c:6d} {100.0 * c / max(total, 1):5.1f}%  {p[-1]}")
            show(p, level + 1)

    show((), 0)


if __name__ == "__main__":
    main()
