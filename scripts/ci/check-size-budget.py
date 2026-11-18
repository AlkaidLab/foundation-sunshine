#!/usr/bin/env python3
"""Fail the build when the Windows installer is larger than the budget in size-budget.json.

The installer is the only gated file. The portable ZIP, sunshine.exe and sunshine-gui.exe are measured for the trend and
written to the job summary. The messages are in Chinese on purpose: the owner reads them in the run log.

Run: python3 scripts/ci/check-size-budget.py --installer FILE [--measure 名称=路径 ...] [--budget scripts/ci/size-budget.json]
"""

import argparse
import json
import os
import pathlib
import sys
import zipfile

DEFAULT_BUDGET = pathlib.Path(__file__).with_name("size-budget.json")


def load_budget(path):
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    limit = data.get("installer_max_bytes")
    # bool is an int in Python: a stray `true` must not pass as a one byte limit
    if not isinstance(limit, int) or isinstance(limit, bool) or limit <= 0:
        raise ValueError(f"{path}: installer_max_bytes 必须是正整数")
    fraction = data.get("warn_fraction", 0.9)
    if isinstance(fraction, bool) or not isinstance(fraction, (int, float)) or not 0 < fraction <= 1:
        raise ValueError(f"{path}: warn_fraction 必须在 0 到 1 之间")
    return limit, float(fraction)


def zip_member_size(archive, member):
    """Uncompressed size of a member, or None when the archive or the member is missing."""
    try:
        with zipfile.ZipFile(archive) as bundle:
            return bundle.getinfo(member).file_size
    except (OSError, KeyError, zipfile.BadZipFile):
        return None


def mb(count):
    return f"{count / 1_000_000:.2f} MB"


def evaluate(installer_bytes, limit, fraction):
    """Return ("fail" | "warn" | "ok", the message)."""
    percent = installer_bytes * 100 / limit
    if installer_bytes > limit:
        over = installer_bytes - limit
        return "fail", (
            f"安装器 {installer_bytes:,} 字节，超过体积上限 {limit:,} 字节（超出 {over:,} 字节，{mb(over)}）。"
            "请先瘦身：看 job summary 里便携包、sunshine.exe、sunshine-gui.exe 各涨了多少。"
            "上限写在 scripts/ci/size-budget.json，调高上限必须经车主批准，不要自行修改。")
    if installer_bytes >= limit * fraction:
        left = limit - installer_bytes
        return "warn", (
            f"安装器 {installer_bytes:,} 字节，已用上限 {limit:,} 字节的 {percent:.1f}%"
            f"（超过 {fraction * 100:.0f}% 警戒线），余量 {left:,} 字节（{mb(left)}）。")
    return "ok", f"安装器 {installer_bytes:,} 字节，占上限 {limit:,} 字节的 {percent:.1f}%。"


def summary_table(installer_bytes, limit, status, extra):
    rows = ["### 体积门禁", "", "| 产物 | 字节 | 约合 |", "| --- | ---: | ---: |",
            f"| 安装器（上限 {limit:,}） | {installer_bytes:,} | {mb(installer_bytes)} |"]
    for name, size in extra:
        rows.append(f"| {name} | {size:,} | {mb(size)} |" if size is not None else f"| {name} | 未生成 | |")
    rows += ["", {"fail": "结果：**失败**，超过上限。", "warn": "结果：通过，但已超过警戒线。", "ok": "结果：通过。"}[status], ""]
    return "\n".join(rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--budget", default=str(DEFAULT_BUDGET))
    parser.add_argument("--installer", required=True)
    parser.add_argument("--measure", action="append", default=[], metavar="名称=路径", help="只记录大小的文件，可重复")
    parser.add_argument("--measure-zip", action="append", default=[], metavar="名称=压缩包::成员", help="只记录压缩包内某个文件解压后的大小，可重复")
    parser.add_argument("--summary", default=os.environ.get("GITHUB_STEP_SUMMARY"), help="追加 markdown 的文件")
    args = parser.parse_args(argv)
    # The Windows runner's console code page (cp1252) cannot print the Chinese messages
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")

    try:
        limit, fraction = load_budget(args.budget)
    except (OSError, ValueError) as error:
        print(f"::error::读不到体积上限配置：{error}")
        return 2
    installer = pathlib.Path(args.installer)
    if not installer.is_file():
        print(f"::error::找不到安装器 {installer}，无法检查体积。")
        return 2

    extra = []
    for item in args.measure:
        name, separator, path = item.partition("=")
        if not separator or not name:
            print(f"::error::--measure 要写成 名称=路径：{item}")
            return 2
        target = pathlib.Path(path)
        extra.append((name, target.stat().st_size if target.is_file() else None))

    for item in args.measure_zip:
        name, separator, spec = item.partition("=")
        archive, member_separator, member = spec.rpartition("::")
        if not separator or not name or not member_separator or not archive or not member:
            print(f"::error::--measure-zip 要写成 名称=压缩包::成员：{item}")
            return 2
        extra.append((name, zip_member_size(archive, member)))

    installer_bytes = installer.stat().st_size
    status, message = evaluate(installer_bytes, limit, fraction)
    table = summary_table(installer_bytes, limit, status, extra)
    print(table)
    print({"fail": "::error::", "warn": "::warning::", "ok": ""}[status] + message)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as handle:
            handle.write(table + "\n")
    return 1 if status == "fail" else 0


if __name__ == "__main__":
    sys.exit(main())
