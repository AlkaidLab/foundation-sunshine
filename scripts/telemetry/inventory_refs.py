#!/usr/bin/env python3
"""Keeps the `src/telemetry/events.cpp:LINE` references of docs/privacy-data-inventory.md and .zh.md pointing at the thing they name.

The inventory has rows that build_inventory.py does not produce, so it cannot be regenerated whole. Each row names an event (`app_start: ...`) or an
envelope field (`session_id`); this script looks the name up in the current source and rewrites the line number of every events.cpp reference in that row.
  inventory_refs.py           rewrite the references in place
  inventory_refs.py --check   change nothing; exit 1 and list the references that are off (what CI or a review can run)
A reference whose row has no rule is an error: add a rule instead of leaving a number that drifts."""
import re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src/telemetry/events.cpp"
DOCS = [ROOT / "docs/privacy-data-inventory.md", ROOT / "docs/privacy-data-inventory.zh.md"]
REF = re.compile(r"src/telemetry/events\.cpp:(\d+)")

# envelope fields (first cell of the row) -> the line that sets the field in envelope_fields()
ENVELOPE = {
    "session_id": r'^\s+\{ "session_id",',
    "product, channel, os, arch": r'^\s+\{ "product",',
    "version": r'^\s+\{ "version", safe_version',
    "os_version": r'^\s+\{ "os_version", safe_os_version',
    "locale": r'^\s+\{ "locale", safe_locale',
    "sample_weight": r'^\s+fields\["sample_weight"\] = ',
}
# rows that name the shared field list of the stream events or the allow-list itself
SPECIAL = {
    "stream_start / stream_end: client_kind": r"^\s+stream_dims\(\) \{",
    "stream_start / stream_end: client_os": r"^\s+stream_dims\(\) \{",
    "stream_end:": r'push_back\(\{ kind_e::\w+, "stream_end"',
}
# a row that points at a second thing besides its event: row name -> where that is
SECONDARY = {
    "setting_changed": r"^\s+settings\(\) \{",  # the list of the settings that may be reported and their value lists
    "user_report": r"^\s+scrub_text\(std::string_view input",  # the text scrub of the host
}
ALLOW_LIST_TEXT = r"^\s+specs\(\) \{"  # a sentence about the allow-list outside a table row


def line_of(lines, pattern):
    hits = [i + 1 for i, text in enumerate(lines) if re.search(pattern, text)]
    if len(hits) != 1:
        raise SystemExit(f"{len(hits)} matches (need exactly one) for {pattern!r} in {SRC.name}")
    return hits[0]


def target_pattern(row_label, in_table):
    label = re.sub(r"\s*:\s*", ": ", row_label.strip().replace("：", ":"))
    if not in_table:
        return ALLOW_LIST_TEXT
    for key, pattern in ENVELOPE.items():
        if label == key or label.startswith(key + " ") and key == "product, channel, os, arch":
            return pattern
    for key, pattern in SPECIAL.items():
        if label.startswith(key):
            return pattern
    match = re.match(r"([a-z_]+)\b", label)  # "app_start: ...", "app_start：...", "stream_quality (per stream)"
    if match:
        return rf'push_back\(\{{ kind_e::\w+, "{match.group(1)}"'
    return None


def label_of(text):
    return text.split("|")[1].replace("：", ":") if text.startswith("|") else ""


def main(check):
    lines = SRC.read_text(encoding="utf-8").splitlines()
    off = []
    for doc in DOCS:
        out = []
        for number, text in enumerate(doc.read_text(encoding="utf-8").splitlines(), 1):
            if REF.search(text):
                in_table = text.startswith("|")
                label = text.split("|")[1] if in_table else ""
                pattern = target_pattern(label, in_table)
                if pattern is None:
                    raise SystemExit(f"{doc.name}:{number}: no rule for the row {label.strip()!r}")
                want = line_of(lines, pattern)
                first_old = REF.search(text).group(1)
                second = next((v for k, v in SECONDARY.items() if in_table and re.match(rf"\s*{k}\b", label_of(text))), None)

                def replace(match):
                    # a reference with the number of the first one names the same thing; another number names the second thing of the row
                    if match.group(1) == first_old or second is None:
                        return f"src/telemetry/events.cpp:{want}"
                    return f"src/telemetry/events.cpp:{line_of(lines, second)}"

                fixed = REF.sub(replace, text)
                if fixed != text:
                    off.append(f"{doc.name}:{number}: {REF.findall(text)} -> {want}")
                text = fixed
            out.append(text)
        if not check:
            doc.write_text("\n".join(out) + "\n", encoding="utf-8")
    if off:
        print("\n".join(off))
    return 1 if (check and off) else 0


if __name__ == "__main__":
    sys.exit(main("--check" in sys.argv[1:]))
