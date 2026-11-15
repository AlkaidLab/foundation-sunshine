#!/usr/bin/env bash
# Initialise only the submodules a CI job needs.
#
# usage: init-submodules.sh <profile>
#   windows          everything the Windows host build links or includes
#   windows-package  windows, plus the Panel (the installer setup UI is built from it)
#   none             nothing
#
# `actions/checkout` with `submodules: recursive` fetches every submodule serially at full depth, including the
# Linux/macOS/Android-only ones and the prebuilt FFmpeg of every platform. Here each submodule is fetched shallow
# (the pinned commit only) as a blobless partial clone (git-clone --filter, git-fetch --depth), several at a time
# (git-submodule update --jobs). build-deps and AMF are fetched sparse: the working tree is restricted before the
# first checkout, so git never downloads the blobs it does not need.
#
# Every top-level submodule is registered first, one command, because `git submodule init` writes the superproject's
# .git/config and concurrent writers race on its lock. Only then do the fetches run side by side; each background job is
# waited for on its own, so a failed fetch fails the script instead of being lost.
set -euo pipefail

profile="${1:?profile required (windows|windows-package|none)}"
jobs="${SUBMODULE_JOBS:-8}"
git config --global core.longpaths true 2>/dev/null || true

# Granite lists Android, Linux-only and vendored-SDL submodules (and PyroWave itself) the Windows build never reads.
granite_skipped="oboe fsr2 fossilize sdl3 pyroenc pyrowave"

# The submodules are registered by the time any of these runs, so --init (which --filter requires) finds the configuration
# complete and writes nothing to the superproject config.
update() { git -c submodule.fetchJobs="$jobs" submodule update --init --depth 1 --filter=blob:none --jobs "$jobs" "$@"; }

# sparse_submodule <path> <cone dir>...  - shallow, blobless, sparse checkout of the commit the superproject pins.
sparse_submodule() {
  local path="$1"; shift
  local sha url
  sha="$(git rev-parse ":$path")"
  url="$(git config "submodule.$path.url")"
  mkdir -p "$path"
  git -C "$path" init -q
  # Repeatable: a retry after a failed fetch finds `origin` already there.
  if git -C "$path" remote get-url origin >/dev/null 2>&1; then
    git -C "$path" remote set-url origin "$url"
  else
    git -C "$path" remote add origin "$url"
  fi
  git -C "$path" sparse-checkout set --cone "$@"
  git -C "$path" fetch -q --depth 1 --filter=blob:none origin "$sha"
  git -C "$path" -c advice.detachedHead=false checkout -q --detach FETCH_HEAD
  git submodule absorbgitdirs -- "$path"
}

# PyroWave -> Granite. Only the Granite submodules that are not in granite_skipped are fetched, named by path: removing a
# submodule's URL from the configuration would not exclude it, `update --init` registers it again.
pyrowave_and_granite() {
  local granite=third-party/pyrowave/third_party/Granite
  update -- third-party/pyrowave
  git -C third-party/pyrowave submodule update --init --depth 1 --filter=blob:none -- third_party/Granite
  local -a wanted=()
  local path skip
  while read -r _ path; do
    [[ " $granite_skipped " == *" ${path##*/} "* ]] || wanted+=("$path")
  done < <(git -C "$granite" config -f .gitmodules --get-regexp '^submodule\..*\.path$')
  git -C "$granite" submodule update --init --depth 1 --filter=blob:none --jobs "$jobs" -- "${wanted[@]}"
  echo "[submodules] granite fetched: ${wanted[*]##*/}"
  for skip in $granite_skipped; do
    if [[ -e "$granite/third_party/$skip/.git" ]]; then
      echo "[submodules] granite/$skip must not be fetched, but it is" >&2
      return 1
    fi
  done
}

case "$profile" in
  none) exit 0 ;;
  windows|windows-package)
    t0=$SECONDS
    stamp() { echo "[submodules] $1: $((SECONDS - t0)) s"; }
    plain=(third-party/googletest third-party/nanors third-party/nvapi third-party/ViGEmClient third-party/tray
      third-party/nvenc-headers/1100 third-party/nvenc-headers/1200 third-party/nvenc-headers/1300 third-party/nvenc-headers/1301
      third-party/moonlight-audio-haptics)
    [[ "$profile" == windows-package ]] && plain+=(src_assets/common/sunshine-control-panel)

    git submodule init -- "${plain[@]}" third-party/pyrowave third-party/moonlight-common-c third-party/build-deps third-party/AMF
    stamp "registered"

    pids=(); names=()
    background() { local name="$1"; shift; "$@" & pids+=($!); names+=("$name"); }
    background build-deps sparse_submodule third-party/build-deps dist/Windows-AMD64
    background AMF sparse_submodule third-party/AMF amf/public/include
    background plain update -- "${plain[@]}"
    # The Granite chain is the slowest, so it gets its own job and starts at once.
    background pyrowave pyrowave_and_granite
    # moonlight-common-c carries enet and nanors/simde as nested submodules the build compiles.
    background moonlight-common-c git -c submodule.fetchJobs="$jobs" submodule update --init --recursive --depth 1 --filter=blob:none --jobs "$jobs" -- third-party/moonlight-common-c

    status=0
    for i in "${!pids[@]}"; do
      if wait "${pids[$i]}"; then
        stamp "${names[$i]}"
      else
        echo "[submodules] ${names[$i]} failed" >&2
        status=1
      fi
    done
    [[ $status -eq 0 ]] || exit 1
    stamp "all"
    ;;
  *) echo "unknown profile: $profile" >&2; exit 2 ;;
esac
