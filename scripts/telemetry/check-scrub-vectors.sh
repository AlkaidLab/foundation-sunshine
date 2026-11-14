#!/usr/bin/env bash
# The three scrub implementations (C++, JS, TS) run the same vectors. This checks that the three copies of the vector file are identical.
set -euo pipefail
here="$(cd "$(dirname "$0")/../.." && pwd)"
host="$here/tests/data/telemetry_scrub_vectors.json"
m3e="${ALK_M3E:-/scratch/tp-m3e}/test/scrub-vectors.json"
worker="${ALK_WORKER:-$HOME/Projects/alkaidlab-telemetry-worker}/test/scrub-vectors.json"
status=0
for copy in "$m3e" "$worker"; do
  if ! cmp -s "$host" "$copy"; then echo "differs from the host copy: $copy" >&2; status=1; fi
done
[ "$status" = 0 ] && echo "scrub vectors: the three copies are identical ($(sha256sum "$host" | cut -c1-16))"
exit $status
