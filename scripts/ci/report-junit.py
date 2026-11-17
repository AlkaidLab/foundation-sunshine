#!/usr/bin/env python3
"""Turn JUnit XML (ctest --output-junit, GoogleTest, node --test-reporter=junit) into a job summary and annotations,
and fail the step when the results cannot be trusted.

usage: report-junit.py [--min-tests N] [--allowed-skips FILE] [--title TEXT] <junit.xml>...

Exit status 1 when
  * a test failed or errored,
  * no test ran, or fewer than --min-tests ran (a registered suite that silently disappeared),
  * a test was skipped that is not listed in --allowed-skips (one test name per line, '#' comments).

Annotations use the workflow command syntax (docs.github.com, "Workflow commands for GitHub Actions"), so they work in
fork pull requests: they need no token. The runner shows at most 10 error annotations per step; the full list is
always written to the job summary ($GITHUB_STEP_SUMMARY).
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET


def escape_data(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def escape_prop(text: str) -> str:
    return escape_data(text).replace(":", "%3A").replace(",", "%2C")


def load(paths):
    cases = []
    for path in paths:
        try:
            root = ET.parse(path).getroot()
        except (OSError, ET.ParseError) as error:
            print(f"::error title=Unreadable test report::{escape_data(path)}: {escape_data(str(error))}")
            cases.append({"name": f"<unreadable {path}>", "suite": "", "status": "fail", "time": 0.0,
                          "message": str(error), "file": None, "line": None})
            continue
        for case in root.iter("testcase"):
            status = "pass"
            message = ""
            if case.find("failure") is not None or case.find("error") is not None:
                status = "fail"
                node = case.find("failure") if case.find("failure") is not None else case.find("error")
                message = (node.get("message") or node.text or "").strip()
            elif case.find("skipped") is not None or case.get("status") in ("notrun", "disabled"):
                # ctest --output-junit writes a disabled test as status="disabled" without a <skipped> child
                status = "skip"
            cases.append({
                "name": case.get("name", "?"),
                "suite": case.get("classname", ""),
                "status": status,
                "time": float(case.get("time") or 0),
                "message": message,
                "file": case.get("file"),
                "line": case.get("line"),
            })
    return cases


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--min-tests", type=int, default=1)
    parser.add_argument("--allowed-skips")
    parser.add_argument("--title", default="Tests")
    parser.add_argument("reports", nargs="+")
    args = parser.parse_args()

    cases = load(args.reports)
    allowed = set()
    if args.allowed_skips:
        with open(args.allowed_skips, encoding="utf-8") as handle:
            allowed = {line.strip() for line in handle if line.strip() and not line.startswith("#")}

    failed = [c for c in cases if c["status"] == "fail"]
    skipped = [c for c in cases if c["status"] == "skip"]
    unexpected_skips = [c for c in skipped if c["name"] not in allowed and f'{c["suite"]}.{c["name"]}' not in allowed]
    problems = []
    if failed:
        problems.append(f"{len(failed)} failed")
    if len(cases) < max(args.min_tests, 1):
        problems.append(f"only {len(cases)} tests ran, expected at least {max(args.min_tests, 1)}")
    if unexpected_skips:
        problems.append(f"{len(unexpected_skips)} unexpected skips")

    for case in failed[:10]:
        where = ""
        if case["file"]:
            where = f"file={escape_prop(case['file'])}," + (f"line={case['line']}," if case["line"] else "")
        print(f"::error {where}title={escape_prop(args.title + ': ' + case['suite'] + '.' + case['name'])}::{escape_data(case['message'][:600] or 'failed')}")
    for case in unexpected_skips[:10]:
        print(f"::error title={escape_prop(args.title + ': unexpected skip')}::{escape_data(case['suite'] + '.' + case['name'])} was skipped and is not in the allowed list")

    lines = [f"### {args.title}", "",
             f"| total | passed | failed | skipped |", "|---|---|---|---|",
             f"| {len(cases)} | {len(cases) - len(failed) - len(skipped)} | {len(failed)} | {len(skipped)} |", ""]
    if problems:
        lines += [f"**Result: failed** ({'; '.join(problems)})", ""]
    if failed:
        lines += ["#### Failed", ""] + [f"- `{c['suite']}.{c['name']}`: {c['message'][:300]!r}" for c in failed] + [""]
    if skipped:
        lines += ["#### Skipped", ""] + [f"- `{c['suite']}.{c['name']}`" + ("" if c in unexpected_skips else " (allowed)") for c in skipped] + [""]
    slow = sorted(cases, key=lambda c: -c["time"])[:5]
    if slow and slow[0]["time"] > 0:
        lines += ["#### Slowest", ""] + [f"- `{c['suite']}.{c['name']}` {c['time']:.1f} s" for c in slow] + [""]
    text = "\n".join(lines)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(text + "\n")
    print(text)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
