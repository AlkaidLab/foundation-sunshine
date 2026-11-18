#!/usr/bin/env python3
"""Syntax-check every bash/sh `run:` block in the workflow and composite-action files with `bash -n`.

usage: check-workflow-shell.py [FILE.yml ...]     (no files: .github/workflows/*.yml and .github/actions/*/action.yml)

Why: actionlint hands scripts to ShellCheck only for the shells it knows by name; the `msys2 {0}` steps (the whole Windows
build) are skipped, and a stray `fi` there is a runtime failure that only shows up when the step runs. A `${{ expr }}` is
replaced by a placeholder first (it is evaluated before the shell sees the script). pwsh/cmd/python steps are not checked.

Exit status 1 with one ::error annotation per script that bash cannot parse.
"""

import glob
import re
import subprocess
import sys

import yaml

BASH_SHELLS = {None, "bash", "sh", "msys2 {0}"}


def default_shell(node):
    return (((node or {}).get("defaults") or {}).get("run") or {}).get("shell")


def steps_of(document):
    if not isinstance(document, dict):
        return
    # A step's own shell wins over the job default, which wins over the workflow default (GitHub Docs, "Setting default values for jobs").
    workflow_shell = default_shell(document)
    for job in (document.get("jobs") or {}).values():
        for step in (job or {}).get("steps") or []:
            yield step, default_shell(job) or workflow_shell
    runs = document.get("runs") or {}
    for step in runs.get("steps") or []:
        yield step, None


def main() -> int:
    files = sys.argv[1:] or sorted(glob.glob(".github/workflows/*.yml") + glob.glob(".github/actions/*/action.yml"))
    failures = 0
    checked = 0
    for path in files:
        with open(path, encoding="utf-8") as handle:
            document = yaml.safe_load(handle)
        for step, default_shell in steps_of(document):
            script = step.get("run")
            if not script:
                continue
            shell = step.get("shell", default_shell)
            if shell not in BASH_SHELLS:
                continue
            checked += 1
            text = re.sub(r"\$\{\{.*?\}\}", "EXPR", script, flags=re.S)
            result = subprocess.run(["bash", "-n"], input=text, capture_output=True, text=True)
            if result.returncode != 0:
                failures += 1
                name = step.get("name", "(unnamed step)")
                message = result.stderr.strip().splitlines()[0] if result.stderr.strip() else "syntax error"
                print(f"::error file={path},title=Shell syntax error in step '{name}'::{message}")
    print(f"checked {checked} shell scripts in {len(files)} files, {failures} with syntax errors")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
