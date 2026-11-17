#!/usr/bin/env python3
"""Release version and tag of a commit, computed the way the release pipeline computes them.

usage: release-info.py [--date ISO8601] [--output FILE]      (default date: the committer date of HEAD)

The master pipeline takes the version from the push event's commits[0].timestamp, which is the commit's *committer* date
(verified against the event payload), through LizardByte/setup-release-action:

    version = f"{year}.{int(month)}{day}.{int(hour + minute + second)}"        # month unpadded, day/h/m/s zero-padded text
    tag     = "v" + version

The asset names use a shorter form of the tag (main.yml, "Rename release assets"): vYEAR.DATE with DATE padded to four
digits. The merge-queue candidate build needs both before the commit is on master, so that the binary it builds carries
exactly the version the later push would give it. Prints  release_version, release_tag, asset_tag  as key=value lines.
"""

import argparse
import datetime
import subprocess
import sys


def compute(iso: str):
    stamp = datetime.datetime.fromisoformat(iso.replace("Z", "+00:00")).astimezone(datetime.timezone.utc)
    month = str(stamp.month)
    day = f"{stamp.day:02d}"
    clock = f"{stamp.hour:02d}{stamp.minute:02d}{stamp.second:02d}"
    version = f"{stamp.year}.{int(month)}{day}.{int(clock)}"
    tag = f"v{version}"
    date_part = f"{int(month)}{day}"
    asset_tag = f"v{stamp.year}.{int(date_part):04d}"
    return version, tag, asset_tag


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--date")
    parser.add_argument("--output")
    args = parser.parse_args()
    iso = args.date or subprocess.run(["git", "log", "-1", "--format=%cI"], capture_output=True, text=True, check=True).stdout.strip()
    version, tag, asset_tag = compute(iso)
    lines = [f"release_version={version}", f"release_tag={tag}", f"asset_tag={asset_tag}"]
    print("\n".join(lines))
    if args.output:
        with open(args.output, "a", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
