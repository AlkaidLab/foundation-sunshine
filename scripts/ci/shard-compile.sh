#!/usr/bin/env bash
# One runner of a cold compile: build chunks of object files until none is left, so a slow runner simply builds fewer.
#
# usage: shard-compile.sh INDEX COUNT        (in the repository root; ./build is configured)
#
# The chunks come from shard-targets.py, heaviest first. A runner takes the next chunk by creating the git ref
# refs/claims/<run>-<attempt>/<k> (POST /git/refs): creating a ref that exists fails with 422, so exactly one runner wins each
# chunk (GitHub REST, "Create a reference"). The ref needs the contents:write token of a trusted event; with a read-only token
# (a pull request from a fork), or when the API does not answer, the runner falls back to a fixed split of the chunks over the
# runners (snake order over the chunk weights). A 422 only means "taken" when a GET of the ref confirms it exists; any other
# 422 (a refused name, a rejected sha) leaves the chunk to the fixed split. A chunk that nobody builds, because its runner died,
# is compiled by the final job, which only costs time (build_win runs when a shard failed, and merges the caches that exist).
# A runner that finds every chunk taken builds nothing and still succeeds: that is the normal outcome for a runner that starts
# late next to fast peers.
# The token is kept out of the environment of everything this script starts: ninja runs CMake- and source-controlled commands.
set -euo pipefail
index="$1"; count="$2"
chunks="${SHARD_CHUNKS:-32}"
token="${CLAIM_TOKEN:-}"; unset CLAIM_TOKEN
dynamic=false
if [[ -n "$token" && -n "${GITHUB_REPOSITORY:-}" ]]; then dynamic=true; fi

ninja -C build -t commands | python scripts/ci/shard-targets.py "$chunks" scripts/ci/shard-weights.ninja_log build/shard.ninja

claim() {
  local code
  code="$(printf 'header = "Authorization: Bearer %s"\n' "$token" | curl -sS -K - -o /dev/null -w '%{http_code}' --max-time 20 \
    -X POST -H 'Accept: application/vnd.github+json' -H 'X-GitHub-Api-Version: 2022-11-28' \
    "https://api.github.com/repos/$GITHUB_REPOSITORY/git/refs" \
    -d "{\"ref\":\"refs/claims/${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}/$1\",\"sha\":\"$GITHUB_SHA\"}" 2>/dev/null)" || code=000
  echo "$code"
}
# 0 when refs/claims/<run>-<attempt>/$1 exists (HTTP 200), 1 when it does not or the answer is unclear.
claimed_by_someone() {
  local code
  code="$(printf 'header = "Authorization: Bearer %s"\n' "$token" | curl -sS -K - -o /dev/null -w '%{http_code}' --max-time 20 \
    -H 'Accept: application/vnd.github+json' -H 'X-GitHub-Api-Version: 2022-11-28' \
    "https://api.github.com/repos/$GITHUB_REPOSITORY/git/ref/claims/${GITHUB_RUN_ID}-${GITHUB_RUN_ATTEMPT}/$1" 2>/dev/null)" || code=000
  [[ "$code" == 200 ]]
}
# Fixed split: chunk k goes to runner k mod count, the order reversed on every second round (heaviest first, then lightest first).
owns_static() {
  local round=$(( $1 / count )) pos=$(( $1 % count ))
  if (( round % 2 == 1 )); then pos=$(( count - 1 - pos )); fi
  (( pos == index ))
}

mine=0; built_seconds=0; started=$(date +%s)
for (( k = 0; k < chunks; k++ )); do
  if [[ "$dynamic" == true ]]; then
    code="$(claim "$k")"
    case "$code" in
      201) ;;
      422) if claimed_by_someone "$k"; then continue; fi
           echo "claim of chunk $k answered 422 but the ref does not exist: leaving the chunk to the fixed split"
           owns_static "$k" || continue ;;
      *) echo "claim of chunk $k answered $code: falling back to the fixed split"; dynamic=false
         owns_static "$k" || continue ;;
    esac
  else
    owns_static "$k" || continue
  fi
  t0=$(date +%s)
  ninja -C build -f shard.ninja "ci_chunk_$k"
  secs=$(( $(date +%s) - t0 ))
  built_seconds=$(( built_seconds + secs ))
  mine=$(( mine + 1 ))
  echo "chunk $k: $secs s"
done
echo "runner $index built $mine of $chunks chunks in $built_seconds s (wall $(( $(date +%s) - started )) s, claims: $([[ "$dynamic" == true ]] && echo dynamic || echo fixed))" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
# A runner that claimed nothing has no build log; the upload step expects the file, so leave an empty one.
if [[ -f build/.ninja_log ]]; then cp build/.ninja_log ninja_log.shard; else : > ninja_log.shard; fi
ccache --show-stats || true
