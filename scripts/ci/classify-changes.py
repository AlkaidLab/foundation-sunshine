#!/usr/bin/env python3
"""Decide which CI jobs a change needs, from the list of changed files.

usage: classify-changes.py [--all] [--output FILE] < changed-files.txt

Reads one repository-relative path per line on stdin and prints  name=true|false  lines (also appended to --output,
normally $GITHUB_OUTPUT):

  cpp        something the host build or its tests can notice: src, tests, cmake, third-party pins, tools, packaging,
             CMake lists, the pipeline itself (an unknown path counts as cpp)
  webui      Web UI sources, npm manifests, Vite/ESLint config, locale files, scripts the Web UI build uses, and every
             file the Web UI tests read from outside the Web UI directory (they check src/config.cpp, tests/fixtures,
             tools/installer_*, README.md, docs/authority, assets/brand against the pages); implied by cpp
  installer  installer shell, installer packaging, the Panel pin
  workflow   any workflow or CI script changed (static checks)
  docs       nothing but documentation, images and issue templates changed
  docs_touched  at least one documentation file changed (the link check runs for it even next to code changes)
  any_code   cpp or webui or installer

The rules are conservative: a path no rule knows is C++, never documentation, so a missing rule costs a build and
never skips one. --all, or an empty file list (manual runs, schedules), turns every code flag on.
"""

import argparse
import fnmatch
import sys

DOC = [
    "docs/*", "*.md", "README*", "LICENSE", "NOTICE", "gh-pages-template/*", "*.png", "*.jpg", "*.jpeg", "*.gif",
    "*.webp", "*.svg", ".github/ISSUE_TEMPLATE/*", ".github/FUNDING.yml", ".github/RELEASE_TEMPLATE.md",
    ".coderabbit.yaml", ".readthedocs.yaml", "crowdin.yml", ".github/crowdin.yml", "codecov.yml",
]
# Documentation-looking files that are build inputs.
NOT_DOC = ["docs/CMakeLists.txt", "docs/Doxyfile*", "docs/source/conf.py", "src_assets/*", "src/*"]
WEBUI = [
    "src_assets/common/assets/web/*", "package.json", "package-lock.json", "vite*.js", "scripts/vite/*",
    ".node-version", "eslint.config.*", "scripts/validate-i18n.js", "scripts/reverse-sync-i18n.js",
    "scripts/format-i18n.js", "scripts/gen-config-index.mjs", "scripts/add-missing-locale-keys.mjs",
    "README.md", "docs/authority/*", "assets/brand/*",
]
INSTALLER = [
    "tools/installer*", "tools/append_installer_payload.py", "cmake/packaging/*", "scripts/*installer*",
    "src_assets/common/sunshine-control-panel",
]
# The pipeline itself: a change here must be exercised by a full run.
PIPELINE = [".github/workflows/main.yml", ".github/actions/*", "scripts/ci/*"]
WORKFLOW = [".github/workflows/*", ".github/actions/*", ".github/dependabot.yml", "scripts/ci/*"]


def hit(path, patterns):
    return any(fnmatch.fnmatchcase(path, p) for p in patterns)


def categories(path):
    found = set()
    if hit(path, DOC) and not hit(path, NOT_DOC):
        found.add("docs")
    if hit(path, WORKFLOW):
        found.add("workflow")
    if hit(path, PIPELINE):
        found |= {"cpp", "webui"}
    if hit(path, WEBUI):
        found.add("webui")
    if hit(path, INSTALLER):
        found |= {"installer", "cpp"}
    if not found:
        found.add("cpp")
    if "cpp" in found:
        found.add("webui")
    return found


def classify(paths):
    flags = {"cpp": False, "webui": False, "installer": False, "workflow": False, "docs": False, "docs_touched": False}
    only_docs = bool(paths)
    for path in paths:
        found = categories(path)
        for name in found:
            flags[name] = True
        if "docs" in found:
            flags["docs_touched"] = True
        if found != {"docs"}:
            only_docs = False
    flags["docs"] = only_docs
    flags["any_code"] = flags["cpp"] or flags["webui"] or flags["installer"]
    return flags


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true", help="turn every code flag on (manual runs, schedules)")
    parser.add_argument("--output")
    args = parser.parse_args()
    paths = [line.strip() for line in sys.stdin if line.strip()]
    if args.all or not paths:
        flags = {"cpp": True, "webui": True, "installer": True, "workflow": True, "docs": False, "docs_touched": True, "any_code": True}
    else:
        flags = classify(paths)
    lines = [f"{key}={'true' if value else 'false'}" for key, value in flags.items()]
    print("\n".join(lines))
    if args.output:
        with open(args.output, "a", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
