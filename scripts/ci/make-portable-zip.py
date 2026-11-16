#!/usr/bin/env python3
"""Create the Windows portable archive with parallel per-file compression.

CPack's ZIP generator applies one compression policy to every entry.  This
packager stores image and installer payloads directly and compresses the
remaining files in parallel, which keeps the archive broadly compatible with
Windows ZIP readers while avoiding work that cannot reduce its size.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
from pathlib import Path
import time
import zlib
import zipfile

# ZIP compatibility and compression-level semantics follow the CMake 4.4
# Archive Generator documentation (source record: cmake-cpack-archive-20261116).
# CPack has one policy for all entries, so the workflow uses this small writer
# to apply the documented ZIP format with per-entry storage decisions.

_STORE_SUFFIXES = frozenset(
    {
        ".7z",
        ".avi",
        ".bz2",
        ".cab",
        ".gif",
        ".gz",
        ".ico",
        ".jpeg",
        ".jpg",
        ".m4a",
        ".mkv",
        ".mov",
        ".mp3",
        ".mp4",
        ".msi",
        ".png",
        ".rar",
        ".webm",
        ".webp",
        ".xz",
        ".zip",
        ".zst",
    }
)
_STORE_INSTALLER_DIRS = ("scripts/gamepad/", "scripts/usbip/")


def _should_store(relative_name: str) -> bool:
    normalized = relative_name.replace(os.sep, "/").lower()
    suffix = Path(normalized).suffix
    if suffix in _STORE_SUFFIXES:
        return True
    return suffix == ".exe" and any(f"/{directory}" in f"/{normalized}" for directory in _STORE_INSTALLER_DIRS)


def _compress_one_with_crc(item: tuple[Path, str], level: int) -> tuple[str, bytes, int, int, int]:
    path, name = item
    data = path.read_bytes()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if _should_store(name):
        return name, data, zipfile.ZIP_STORED, len(data), crc
    compressor = zlib.compressobj(level, zlib.DEFLATED, -15)
    compressed = compressor.compress(data) + compressor.flush()
    if len(compressed) >= len(data):
        return name, data, zipfile.ZIP_STORED, len(data), crc
    return name, compressed, zipfile.ZIP_DEFLATED, len(data), crc


def _write_entry(archive: zipfile.ZipFile, result: tuple[str, bytes, int, int, int]) -> None:
    name, payload, method, original_size, crc = result
    info = zipfile.ZipInfo(name)
    info.create_system = 0
    info.compress_type = method
    info.file_size = original_size
    info.compress_size = len(payload)
    info.CRC = crc
    # ZipFile has no public API for accepting a precompressed member.  These
    # private methods are stable in the Python versions used by the CI image;
    # the format is checked by the package step with cmake -E tar tf.
    archive._writecheck(info)
    info.header_offset = archive.fp.tell()
    archive.fp.write(info.FileHeader(False))
    archive.fp.write(payload)
    archive.filelist.append(info)
    archive.NameToInfo[info.filename] = info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prefix", default="Sunshine")
    parser.add_argument("--level", type=int, default=9, choices=range(1, 10))
    parser.add_argument("--workers", type=int, default=0, help="0 uses the available CPU count")
    args = parser.parse_args()
    source = args.source.resolve()
    if not source.is_dir():
        parser.error(f"source directory does not exist: {source}")
    files = [
        (path, f"{args.prefix}/{path.relative_to(source).as_posix()}")
        for path in sorted(source.rglob("*"))
        if path.is_file()
    ]
    workers = args.workers or min(32, (os.cpu_count() or 1))
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        results = list(executor.map(lambda item: _compress_one_with_crc(item, args.level), files))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
        for result in results:
            _write_entry(archive, result)
        # _write_entry writes the local records directly so worker threads can
        # prepare compressed payloads.  Tell ZipFile where its central
        # directory starts before close() writes the directory records.
        archive.start_dir = archive.fp.tell()
    stored = sum(method == zipfile.ZIP_STORED for _, _, method, _, _ in results)
    source_bytes = sum(size for _, _, _, size, _ in results)
    elapsed = time.perf_counter() - started
    print(
        f"portable zip: files={len(results)} stored={stored} source_bytes={source_bytes} "
        f"archive_bytes={args.output.stat().st_size} seconds={elapsed:.3f} workers={workers} level={args.level}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
