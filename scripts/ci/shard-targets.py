#!/usr/bin/env python3
"""Cut the compile outputs of a build into weight-balanced chunks and write the ninja file that builds them.

usage: ninja -C build -t commands | shard-targets.py CHUNKS WEIGHTS OUTPUT.ninja

The input is the command list of the default target: only the object files named by `-o <path>.obj` are split, so targets
that are excluded from `all` (the Granite shader compiler, for one) are never compiled. (`ninja -n` is no use here: the dry run
of a fresh build directory stops at the pending CMake re-run.)

WEIGHTS is a `.ninja_log` of an earlier build (scripts/ci/shard-weights.ninja_log); an output it does not list gets the median
weight. The longest-processing-time rule puts the next-longest output into the lightest chunk, so the chunks weigh nearly the
same, and the chunks are numbered from the heaviest. OUTPUT.ninja includes build.ninja and defines the phony target
`ci_chunk_<k>` for every chunk. Every runner reads the same input, so the chunks are the same on every runner; which runner
builds which chunk is decided by shard-compile.sh. Prints "<chunk count> <output count>".
"""
import hashlib
import re
import sys


def escape(path):
    return path.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def main():
    chunks, weights_path, out_path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    outputs = []
    for line in sys.stdin:
        m = re.search(r"\s-o\s+(\S+\.obj)(?:\s|$)", line)
        if m:
            outputs.append(m.group(1))
    outputs = sorted(set(outputs))
    weights = {}
    try:
        with open(weights_path) as log:
            for line in log:
                parts = line.rstrip("\n").split("\t")
                if len(parts) == 5 and parts[0].isdigit():
                    weights[parts[3]] = int(parts[1]) - int(parts[0])
    except OSError:
        pass
    known = sorted(weights.values())
    default = known[len(known) // 2] if known else 1
    weight = lambda p: weights.get(p, default)
    outputs.sort(key=lambda p: (-weight(p), hashlib.md5(p.encode()).hexdigest()))
    bins = [[] for _ in range(chunks)]
    load = [0] * chunks
    for p in outputs:
        b = min(range(chunks), key=lambda i: (load[i], i))
        bins[b].append(p)
        load[b] += weight(p)
    order = sorted(range(chunks), key=lambda i: (-load[i], i))
    with open(out_path, "w", newline="\n") as f:
        f.write("ninja_required_version = 1.10\ninclude build.ninja\n")
        for k, b in enumerate(order):
            f.write(f"build ci_chunk_{k}: phony " + " ".join(escape(p) for p in bins[b]) + "\n")
    print(chunks, len(outputs))


main()
