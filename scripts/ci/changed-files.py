#!/usr/bin/env python3
"""List the files a pull request or a push changed, one repository-relative path per line (renamed files under both names).

usage: changed-files.py --event EVENT --repo OWNER/NAME [--pr NUMBER] [--before SHA] [--after SHA]

An empty output means "unknown, treat everything as changed" (classify-changes.py reads it that way). That is what every
case that cannot be listed exactly produces: manual and scheduled runs, a push that creates a branch (before is all
zeros), a before commit the API does not know (force push), a failed API call, and a change of 3000 or more files (the
limit of the compare endpoint and far beyond what a path rule needs to look at).

Pull requests use  GET /repos/{repo}/pulls/{n}/files ; pushes use  GET /repos/{repo}/compare/{before}...{after}
(GitHub REST API, "List pull request files" and "Compare two commits"). Both are paginated and need no checkout.
"""

import argparse
import re
import subprocess
import sys

LIMIT = 3000
NAMES = ".filename, (.previous_filename // empty)"


def endpoint(event, repo, pr, before, after):
    """(path, jq filter) of the API call that lists the changed files, or None when the list is unknown."""
    if event == "pull_request" and pr:
        return f"repos/{repo}/pulls/{pr}/files", f".[] | {NAMES}"
    if event == "push" and before and after and not re.fullmatch(r"0+", before):
        return f"repos/{repo}/compare/{before}...{after}", f".files[] | {NAMES}"
    return None


def changed_files(event, repo, pr=None, before=None, after=None, run=subprocess.run):
    call = endpoint(event, repo, pr, before, after)
    if call is None:
        return []
    path, jq = call
    result = run(["gh", "api", "--paginate", path, "--jq", jq], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"could not list the changed files ({path}): {result.stderr.strip()[:200]}", file=sys.stderr)
        return []
    files = [line for line in result.stdout.splitlines() if line.strip()]
    if len(files) >= LIMIT:
        return []
    return files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--event", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--pr")
    parser.add_argument("--before")
    parser.add_argument("--after")
    args = parser.parse_args()
    files = changed_files(args.event, args.repo, args.pr, args.before, args.after)
    sys.stdout.write("".join(f"{name}\n" for name in files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
