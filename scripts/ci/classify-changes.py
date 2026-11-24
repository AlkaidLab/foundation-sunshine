#!/usr/bin/env python3
"""Decide which CI jobs a change needs, from the list of changed files.

usage: classify-changes.py [--all] [--event EVENT] [--output FILE] < changed-files.txt

Reads one repository-relative path per line on stdin and prints  name=true|false  lines (also appended to --output,
normally $GITHUB_OUTPUT):

  cpp        something the host build or its tests can notice: src, tests, cmake, third-party pins, tools, packaging,
             CMake lists, the pipeline itself (an unknown path counts as cpp)
  webui      Web UI sources, npm manifests, Vite/ESLint config, locale files, scripts the Web UI build uses; implied by cpp
             (the Web UI tests read src/config.cpp, tests/fixtures, tools/installer_* and more against the pages)
  webui_inputs a file only three Web UI tests read (README.md, docs/authority, assets/): those tests alone are enough, the
             four full Web UI shards are not needed for it; the workflow runs them when webui is false
  installer  installer shell, installer packaging, the Panel pin
  workflow   any workflow or CI script changed (static checks)
  docs       nothing but documentation, images and issue templates changed
  docs_touched  at least one documentation file changed (the link check runs for it even next to code changes)
  any_code   cpp or webui or installer
  build      the Windows build, package and release path runs: a pull request needs it for cpp only (its other checks are
             routed by the flags above); a push to master or a manual run needs it for any code, the Web UI sources
             included, because that is what gets packaged and published. webui_inputs alone (README.md and the like) never
             builds.
  package    Web UI build, installer UI and Inno Setup: always outside pull requests, and for installer changes in them

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
]
# Read by defaultAppsArt, diagnosticsPage and mascotSweep (the last one walks every file under assets/ and README.md).
WEBUI_DOCS = ["README.md", "docs/authority/*", "assets/*"]
INSTALLER = [
    "tools/installer*", "tools/append_installer_payload.py", "cmake/packaging/*", "scripts/*installer*",
    "scripts/build-setup-ui.ps1", "scripts/fetch-setup-ui.ps1", "scripts/fetch-paired-gui.ps1", "scripts/generate-checksums.ps1",
    "src_assets/common/sunshine-control-panel",
    "scripts/ci/test-installer-silent.ps1", ".github/actions/test-installer-silent/*",
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
    # After the fallback above: a file under assets/ that is not an image is still unknown, hence cpp.
    if hit(path, WEBUI_DOCS):
        found.add("webui_inputs")
    return found


def classify(paths, event="pull_request"):
    flags = {"cpp": False, "webui": False, "webui_inputs": False, "installer": False, "workflow": False, "docs": False,
             "docs_touched": False}
    only_docs = bool(paths)
    for path in paths:
        found = categories(path)
        for name in found:
            flags[name] = True
        if "docs" in found:
            flags["docs_touched"] = True
        if not found <= {"docs", "webui_inputs"}:
            only_docs = False
    flags["docs"] = only_docs
    flags["any_code"] = flags["cpp"] or flags["webui"] or flags["installer"]
    flags.update(routes(flags, event))
    return flags


def routes(flags, event):
    pull_request = event == "pull_request"
    return {
        "build": flags["cpp"] or (not pull_request and flags["any_code"]),
        "package": not pull_request or flags["installer"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true", help="turn every code flag on (manual runs, schedules)")
    parser.add_argument("--event", default="pull_request")
    parser.add_argument("--output")
    args = parser.parse_args()
    paths = [line.strip() for line in sys.stdin if line.strip()]
    if args.all or not paths:
        flags = {"cpp": True, "webui": True, "webui_inputs": True, "installer": True, "workflow": True, "docs": False,
                 "docs_touched": True, "any_code": True}
        flags.update(routes(flags, "manual"))
    else:
        flags = classify(paths, args.event)
    lines = [f"{key}={'true' if value else 'false'}" for key, value in flags.items()]
    print("\n".join(lines))
    if args.output:
        with open(args.output, "a", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
