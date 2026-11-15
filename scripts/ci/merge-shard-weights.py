#!/usr/bin/env python3
"""Rebuild scripts/ci/shard-weights.ninja_log from the `.ninja_log` files of an earlier sharded build.

usage: merge-shard-weights.py OUTPUT NINJA_LOG...   (the ninja-log-shard-* artifacts of one run, downloaded)

The shard job splits the compile outputs by weight (scripts/ci/shard-targets.py). The weight of an output is the median of its
durations over the given logs. Outputs missing from the file get the median weight, so a stale file only makes the split less
even, never wrong; refresh it when the shards of a nightly finish more than a few minutes apart.
"""
import collections
import statistics
import sys


def main():
    out, logs = sys.argv[1], sys.argv[2:]
    durations = collections.defaultdict(list)
    for path in logs:
        with open(path) as log:
            for line in log:
                parts = line.rstrip("\n").split("\t")
                if len(parts) == 5 and parts[0].isdigit() and parts[3].endswith(".obj"):
                    durations[parts[3]].append(int(parts[1]) - int(parts[0]))
    with open(out, "w") as f:
        f.write("# ninja log v5\n")
        for output, values in sorted(durations.items()):
            f.write(f"0\t{int(statistics.median(values))}\t0\t{output}\t0\n")
    print(f"{len(durations)} outputs from {len(logs)} logs")


main()
