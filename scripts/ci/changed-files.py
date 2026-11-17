#!/usr/bin/env python3
"""List the files a pull request or a push changed, one repository-relative path per line (renamed files under both names).

usage: changed-files.py --event EVENT --repo OWNER/NAME [--pr NUMBER] [--before SHA] [--after SHA]

An empty output means "unknown, treat everything as changed" (classify-changes.py reads it that way). That is what every
case that cannot be listed exactly produces:

* manual and scheduled runs, a push that creates a branch (before is all zeros), a failed API call;
* a push that is not a fast-forward of `before` (force push, diverged history): the three-dot compare lists what `after`
  added since the merge base, not what the push removed from `before`, so a reverted code change would be missed;
* a list that may be truncated: GitHub documents the compare endpoint as returning at most 300 files, on the first page
  only (--paginate does not add more), and "List pull request files" as returning at most 3000 files. A list that
  reaches its limit is incomplete, so it is not trusted. The file objects are counted before renames are expanded.

Pull requests use  GET /repos/{repo}/pulls/{n}/files ; pushes use  GET /repos/{repo}/compare/{before}...{after}
(GitHub REST API, https://docs.github.com/en/rest/pulls/pulls#list-pull-requests-files and
https://docs.github.com/en/rest/commits/commits#compare-two-commits). Neither needs a checkout.
"""

import argparse
import json
import re
import subprocess
import sys

PULL_REQUEST_LIMIT = 3000
COMPARE_LIMIT = 300
OBJECT = "[.filename, .previous_filename] | tojson"


def endpoint(event, repo, pr, before, after):
    """(API path, jq filter, paginate, file limit) of the call that lists the changed files, or None when unknown."""
    if event == "pull_request" and pr:
        return f"repos/{repo}/pulls/{pr}/files", f".[] | {OBJECT}", True, PULL_REQUEST_LIMIT
    if event == "push" and before and after and not re.fullmatch(r"0+", before):
        # status first: only "ahead" means that `before` is an ancestor of `after`
        return f"repos/{repo}/compare/{before}...{after}", f".status, (.files[] | {OBJECT})", False, COMPARE_LIMIT
    return None


def changed_files(event, repo, pr=None, before=None, after=None, run=subprocess.run):
    call = endpoint(event, repo, pr, before, after)
    if call is None:
        return []
    path, jq, paginate, limit = call
    command = ["gh", "api"] + (["--paginate"] if paginate else []) + [path, "--jq", jq]
    result = run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"could not list the changed files ({path}): {result.stderr.strip()[:200]}", file=sys.stderr)
        return []
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    if event == "push":
        status, lines = (lines[0] if lines else ""), lines[1:]
        if status != "ahead":
            print(f"the push is not a fast-forward of the previous head (compare status {status!r}); treating everything as changed", file=sys.stderr)
            return []
    if len(lines) >= limit:
        print(f"{len(lines)} files reach the limit of the API ({limit}); the list may be incomplete, treating everything as changed", file=sys.stderr)
        return []
    names = []
    for line in lines:
        names += [name for name in json.loads(line) if name]
    return names


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
