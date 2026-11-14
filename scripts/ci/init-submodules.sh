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
set -euo pipefail

profile="${1:?profile required (windows|windows-package|none)}"
jobs="${SUBMODULE_JOBS:-8}"
git config --global core.longpaths true 2>/dev/null || true

update() { git -c submodule.fetchJobs="$jobs" submodule update --init --depth 1 --filter=blob:none --jobs "$jobs" "$@"; }

# sparse_submodule <path> <cone dir>...  - shallow, blobless, sparse checkout of the commit the superproject pins.
sparse_submodule() {
  local path="$1"; shift
  local sha url
  sha="$(git rev-parse ":$path")"
  git submodule init -- "$path"
  url="$(git config "submodule.$path.url")"
  mkdir -p "$path"
  git -C "$path" init -q
  git -C "$path" remote add origin "$url"
  git -C "$path" sparse-checkout set --cone "$@"
  git -C "$path" fetch -q --depth 1 --filter=blob:none origin "$sha"
  git -C "$path" -c advice.detachedHead=false checkout -q --detach FETCH_HEAD
  git submodule absorbgitdirs -- "$path"
}

case "$profile" in
  none) exit 0 ;;
  windows|windows-package)
    t0=$SECONDS
    stamp() { echo "[submodules] $1: $((SECONDS - t0)) s"; }
    sparse_submodule third-party/build-deps dist/Windows-AMD64 &
    sparse_submodule third-party/AMF amf/public/include &
    # PyroWave is fetched in the same pass as the plain submodules; its nested Granite tree is the slowest chain, so that
    # chain starts as soon as PyroWave is there and runs while moonlight-common-c (recursive) is fetched.
    update -- \
      third-party/googletest third-party/nanors third-party/nvapi third-party/ViGEmClient third-party/tray \
      third-party/nvenc-headers/1100 third-party/nvenc-headers/1200 \
      third-party/nvenc-headers/1300 third-party/nvenc-headers/1301 \
      third-party/moonlight-audio-haptics third-party/pyrowave
    if [[ "$profile" == windows-package ]]; then update -- src_assets/common/sunshine-control-panel; fi
    stamp "plain submodules and pyrowave"
    # PyroWave -> Granite. Granite lists Android, Linux-only and vendored-SDL submodules the Windows build never reads.
    (
      granite=third-party/pyrowave/third_party/Granite
      git -C third-party/pyrowave submodule update --init --depth 1 --filter=blob:none -- third_party/Granite
      git -C "$granite" submodule init
      for skip in oboe fsr2 fossilize sdl3 pyroenc pyrowave; do
        git -C "$granite" config --unset "submodule.third_party/$skip.url" || true
      done
      git -C "$granite" submodule update --init --depth 1 --filter=blob:none --jobs "$jobs"
    ) &
    # moonlight-common-c carries enet and nanors/simde as nested submodules the build compiles.
    update --recursive -- third-party/moonlight-common-c
    stamp "moonlight-common-c"
    wait
    stamp "all"
    ;;
  *) echo "unknown profile: $profile" >&2; exit 2 ;;
esac
