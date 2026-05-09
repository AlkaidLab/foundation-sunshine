#!/usr/bin/env python3
"""Convenience downloader for RVC pretrained weights and example voice models.

Usage::

    # download required pretrained weights only (HuBERT + RMVPE, ~600 MB)
    python download_models.py --pretrained

    # list curated voice models
    python download_models.py --list

    # download a curated voice model by id
    python download_models.py --voice paimon

    # download from arbitrary HuggingFace URL
    python download_models.py --url https://huggingface.co/.../model.pth

All files land under ``./models/`` next to this script (override with
``--dest``). SHA256 verification when available; resumable via HTTP Range.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

LOG = logging.getLogger("download_models")

DEFAULT_DEST = Path(__file__).resolve().parent / "models"


@dataclass
class Asset:
    name: str
    url: str
    rel_path: str  # path relative to dest dir
    sha256: str = ""  # optional verification
    size_mb: int = 0
    description: str = ""


# Pretrained weights required by any RVC inference run.
# Mirrors are picked from the most stable HF buckets; users behind GFW can
# substitute with hf-mirror.com by setting HF_ENDPOINT env var.
PRETRAINED: list[Asset] = [
    Asset(
        name="hubert_base",
        url="https://huggingface.co/lj1995/VoiceConversionWebUI/resolve/main/hubert_base.pt",
        rel_path="pretrained/hubert_base.pt",
        size_mb=180,
        description="HuBERT-Base content encoder (required by all RVC v2 models)",
    ),
    Asset(
        name="rmvpe",
        url="https://huggingface.co/lj1995/VoiceConversionWebUI/resolve/main/rmvpe.pt",
        rel_path="pretrained/rmvpe.pt",
        size_mb=170,
        description="RMVPE F0 (pitch) extractor — recommended over crepe/pm",
    ),
]

# Curated voice models (community-trained, freely redistributable).
# Add more by extending this list or use --url for arbitrary downloads.
VOICES: dict[str, list[Asset]] = {
    # Each entry: voice_id -> [model.pth, model.index]
    # Below are *placeholder* HF URLs; actual community-mirrored models change
    # over time. Update via PR or pull a fresh registry.json from the repo.
}


def _hf_url(url: str) -> str:
    """Rewrite huggingface.co URLs to HF_ENDPOINT mirror if user set one."""
    import os
    endpoint = os.environ.get("HF_ENDPOINT", "").rstrip("/")
    if not endpoint:
        return url
    return url.replace("https://huggingface.co", endpoint, 1)


def download(asset: Asset, dest_dir: Path, force: bool = False) -> Path:
    target = dest_dir / asset.rel_path
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and not force:
        if asset.sha256:
            if _sha256(target) == asset.sha256:
                LOG.info("[ok] %s already exists (sha256 verified)", target)
                return target
            LOG.warning("[!] %s exists but sha256 mismatch — redownloading", target)
        else:
            LOG.info("[ok] %s already exists (skip; pass --force to redownload)", target)
            return target

    url = _hf_url(asset.url)
    LOG.info("[..] downloading %s (%d MB) -> %s", asset.name, asset.size_mb, target)

    tmp = target.with_suffix(target.suffix + ".part")
    resume = tmp.stat().st_size if tmp.exists() else 0
    req = urllib.request.Request(url, headers={"Range": f"bytes={resume}-"} if resume else {})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            total = int(r.headers.get("Content-Length", "0")) + resume
            mode = "ab" if resume else "wb"
            with open(tmp, mode) as f:
                done = resume
                last_pct = -1
                while True:
                    buf = r.read(64 * 1024)
                    if not buf:
                        break
                    f.write(buf)
                    done += len(buf)
                    if total:
                        pct = int(done * 100 / total)
                        if pct != last_pct and pct % 5 == 0:
                            LOG.info("    %d%% (%.1f / %.1f MB)",
                                     pct, done / 1e6, total / 1e6)
                            last_pct = pct
    except urllib.error.HTTPError as e:
        LOG.error("HTTP %d for %s: %s", e.code, url, e.reason)
        raise
    except urllib.error.URLError as e:
        LOG.error("network error for %s: %s", url, e.reason)
        LOG.error("hint: set HF_ENDPOINT=https://hf-mirror.com if blocked")
        raise

    tmp.rename(target)
    if asset.sha256 and _sha256(target) != asset.sha256:
        target.unlink()
        raise RuntimeError(f"sha256 mismatch for {target} — file deleted")
    LOG.info("[ok] %s", target)
    return target


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def list_voices() -> None:
    if not VOICES:
        print("(no curated voices registered yet)")
        print("Use --url to download arbitrary HuggingFace .pth/.index files.")
        return
    print(f"{'id':<20} {'description':<60}")
    print("-" * 80)
    for vid, assets in VOICES.items():
        desc = assets[0].description if assets else ""
        print(f"{vid:<20} {desc:<60}")


def download_voice(voice_id: str, dest_dir: Path, force: bool) -> None:
    if voice_id not in VOICES:
        LOG.error("unknown voice '%s' (use --list to see available)", voice_id)
        sys.exit(2)
    for a in VOICES[voice_id]:
        download(a, dest_dir, force=force)


def download_url(url: str, dest_dir: Path, force: bool) -> None:
    name = url.rsplit("/", 1)[-1]
    asset = Asset(
        name=name,
        url=url,
        rel_path=f"voices/{name}",
        description="user-supplied",
    )
    download(asset, dest_dir, force=force)


def main(argv: Iterable[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dest", type=Path, default=DEFAULT_DEST,
                   help=f"output directory (default: {DEFAULT_DEST})")
    p.add_argument("--pretrained", action="store_true",
                   help="download required pretrained weights (hubert + rmvpe)")
    p.add_argument("--list", action="store_true", help="list curated voice models")
    p.add_argument("--voice", help="download curated voice by id")
    p.add_argument("--url", help="download arbitrary URL (.pth or .index)")
    p.add_argument("--force", action="store_true", help="redownload even if exists")
    p.add_argument("--log-level", default="INFO")
    args = p.parse_args(list(argv) if argv is not None else None)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(message)s",
    )

    args.dest.mkdir(parents=True, exist_ok=True)

    did_anything = False
    if args.list:
        list_voices()
        did_anything = True
    if args.pretrained:
        for a in PRETRAINED:
            download(a, args.dest, args.force)
        did_anything = True
    if args.voice:
        download_voice(args.voice, args.dest, args.force)
        did_anything = True
    if args.url:
        download_url(args.url, args.dest, args.force)
        did_anything = True

    if not did_anything:
        p.print_help()
        return 1

    # write a small registry next to the assets so the sidecar can discover them
    registry = {
        "pretrained_dir": str((args.dest / "pretrained").resolve()),
        "voices_dir": str((args.dest / "voices").resolve()),
    }
    (args.dest / "registry.json").write_text(json.dumps(registry, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
