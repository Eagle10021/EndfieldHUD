"""Copies an upstream test program without the blocks the Linux port does not implement yet.

usage: filter_upstream_test.py SOURCE OUTPUT FIRST LAST [FIRST LAST ...]
Each pair names the start of the first and of the last line of one omitted block.
"""
import sys

source, output, *markers = sys.argv[1:]
lines = open(source, encoding="utf-8").read().split("\n")
for first, last in zip(markers[::2], markers[1::2]):
    start = next(i for i, line in enumerate(lines) if line.strip().startswith(first))
    end = next(i for i in range(start, len(lines)) if last in lines[i])
    print(f"omitted lines {start + 1}-{end + 1}: {first}")
    del lines[start:end + 1]
open(output, "w", encoding="utf-8").write("\n".join(lines))
