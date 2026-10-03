"""Fetch pinned, sparse GoogCC validation dependencies without editing algorithms."""

import argparse
import json
import pathlib
import subprocess


def git(*arguments, cwd=None, capture=False):
    return subprocess.run(
        ["git", *arguments], cwd=cwd, check=True, text=True,
        stdout=subprocess.PIPE if capture else None,
    ).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    destination = arguments.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    versions = json.loads(pathlib.Path(__file__).with_name("versions.json").read_text(encoding="utf-8"))
    for name, dependency in versions.items():
        checkout = destination / dependency["directory"]
        if checkout.exists():
            if not (checkout / ".git").is_dir():
                raise RuntimeError(f"Refusing to replace a non-repository directory: {checkout}")
            if git("status", "--porcelain", cwd=checkout, capture=True).strip():
                raise RuntimeError(f"Refusing to change a dirty dependency checkout: {checkout}")
        else:
            git("clone", "--depth", "1", "--filter=blob:none", "--no-checkout",
                dependency["url"], str(checkout))
        revision = dependency["revision"]
        if git("rev-parse", "HEAD", cwd=checkout, capture=True).strip() != revision:
            git("fetch", "--depth", "1", dependency["url"], revision, cwd=checkout)
        paths = dependency.get("sparse_paths")
        if paths:
            git("sparse-checkout", "init", "--no-cone", cwd=checkout)
            git("sparse-checkout", "set", "--no-cone", *paths, cwd=checkout)
        git("checkout", "--detach", revision, cwd=checkout)
        print(f"{name}: {revision}", flush=True)


if __name__ == "__main__":
    main()
