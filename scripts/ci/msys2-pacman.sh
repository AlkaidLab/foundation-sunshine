#!/bin/bash
# pacman for the MSYS2 toolchain of the Windows jobs, made to survive a mirror that stalls.
#
# usage (inside an MSYS2 shell):  msys2-pacman.sh prepare
#                                 msys2-pacman.sh upgrade
#                                 msys2-pacman.sh install [package ...]     (OPUS_PACKAGE: a pinned package file)
#                                 msys2-pacman.sh prune
#                                 msys2-pacman.sh cache-key
#
# Why: setup-msys2's own `update: true` runs `pacman -Syuu` with no retry. Master run 43736645013 lost four of eight compile
# shards to "failed retrieving file 'ucrt64.db' from mirror.msys2.org : Operation too slow" (pacman's own limit: less than 1 byte/s for
# 10 s), and the one remaining job then compiled half the tree alone. A synchronised database is a few MB, so a retry is cheap.
#
#   * Every pacman call that needs the network is retried with a growing pause. Each attempt starts from a different server of the
#     official mirror list (repo.msys2.org first, then mirror.msys2.org, then the Tier 1 mirrors): pacman already falls through the
#     list for one file, but a stalled first server cost a whole attempt, and a repeated attempt against the same order repeats it.
#   * Only the [msys] and [ucrt64] repositories are synchronised (the others are never installed from): six databases and their
#     signatures become two, which is six fewer connections to stall.
#   * The package files live in pacman's cache directory, which the action restores and saves with actions/cache, so a run
#     downloads only what changed since the last one. The database is always synchronised fresh (the toolchain follows the newest
#     packages, and scripts/ci/config-fingerprint.py reads the same repository).
#
# Paths can be moved for the unit test (scripts/ci/test_msys2_pacman.py): MSYS2_ROOT is the root of the MSYS2 tree (default /).
set -euo pipefail

ROOT="${MSYS2_ROOT:-}"
CONF="$ROOT/etc/pacman.conf"
MIRRORS_DIR="$ROOT/etc/pacman.d"
CACHE_DIR="${PACMAN_CACHE_DIR:-$ROOT/var/cache/pacman/pkg}"
LOCK="${PACMAN_LOCK:-$ROOT/var/lib/pacman/db.lck}"
ATTEMPTS="${PACMAN_ATTEMPTS:-5}"
PAUSE="${PACMAN_RETRY_PAUSE:-10}"
ATTEMPT_TIMEOUT="${PACMAN_ATTEMPT_TIMEOUT:-900}"
# How many mirrors from the top of the list an attempt may start from. Past these, the order is the official one.
ROTATION="${PACMAN_MIRROR_ROTATION:-5}"

say() { echo "msys2-pacman: $*" >&2; }

# Put the Server lines of one official mirror list in the order of attempt N: repo.msys2.org leads, the rest keep the official
# order, and attempt N moves the N-th of the first ROTATION servers to the front.
mirror_order() { # <file> <attempt>
  local file="$1" attempt="$2" ordered start
  mapfile -t ordered < <(grep -E '^[[:space:]]*Server[[:space:]]*=' "$file" | awk '/repo\.msys2\.org/ { print; next } { rest[++n] = $0 } END { for (i = 1; i <= n; i++) print rest[i] }')
  ((${#ordered[@]} > 0)) || return 0
  start=$(((attempt - 1) % (ROTATION < ${#ordered[@]} ? ROTATION : ${#ordered[@]})))
  printf '%s\n' "${ordered[$start]}"
  local i
  for i in "${!ordered[@]}"; do
    ((i == start)) || printf '%s\n' "${ordered[$i]}"
  done
}

# The shipped lists are kept next to the live ones (once), so attempt N always orders the official servers, not the last order.
write_mirrors() { # <attempt>
  local attempt="$1" name
  for name in mirrorlist.msys mirrorlist.mingw; do
    [[ -f "$MIRRORS_DIR/$name" ]] || continue
    [[ -f "$MIRRORS_DIR/$name.official" ]] || cp "$MIRRORS_DIR/$name" "$MIRRORS_DIR/$name.official"
    { echo "# attempt $attempt: official servers, repo.msys2.org first"; mirror_order "$MIRRORS_DIR/$name.official" "$attempt"; } > "$MIRRORS_DIR/$name"
  done
  say "attempt $attempt starts at: $(grep -h -m1 '^Server' "$MIRRORS_DIR/mirrorlist.mingw" 2> /dev/null | sed 's/^Server *= *//')"
}

# <description> <command ...>: the command is run up to ATTEMPTS times, each time against another first mirror.
retry() {
  local what="$1" attempt=1 pause="$PAUSE" status=0
  shift
  while true; do
    write_mirrors "$attempt"
    status=0
    # `timeout` runs programs, not shell functions.
    if [[ "$(type -t "$1")" == file && -n "$(command -v timeout)" ]]; then timeout "$ATTEMPT_TIMEOUT" "$@" || status=$?; else "$@" || status=$?; fi
    if ((status == 0)); then
      ((attempt == 1)) || say "$what succeeded on attempt $attempt"
      return 0
    fi
    # An attempt that was killed or timed out leaves pacman's lock behind; nothing else runs pacman here.
    rm -f "$LOCK"
    if ((attempt >= ATTEMPTS)); then
      echo "::error::$what failed after $attempt attempts (exit $status)"
      return "$status"
    fi
    say "$what failed (exit $status), attempt $attempt of $ATTEMPTS; retrying in ${pause}s"
    sleep "$pause"
    pause=$((pause * 2 < 60 ? pause * 2 : 60))
    attempt=$((attempt + 1))
  done
}

prepare() {
  # Space for the packages is checked against the drive of the cache dir, which is not what the runner reports; setup-msys2 does the same.
  sed -i 's/^CheckSpace/#CheckSpace/' "$CONF"
  # Comment every line of the repositories that are not used: [msys] (base system) and [ucrt64] (the toolchain) are all that is installed.
  awk '
    /^\[/ { keep = ($0 == "[options]" || $0 == "[msys]" || $0 == "[ucrt64]") }
    { if (!keep && $0 != "" && $0 !~ /^#/) print "#" $0; else print }
  ' "$CONF" > "$CONF.new"
  mv -f "$CONF.new" "$CONF"
  write_mirrors 1
  mkdir -p "$CACHE_DIR"
}

upgrade() {
  # -Syuu as setup-msys2 does it: the second u allows the downgrade of a package the repository has rolled back.
  retry "pacman -Syuu" pacman --noconfirm -Syuu --overwrite '*'
}

# The pinned package file must match OPUS_SHA256 when it is set: a mirror cannot be trusted to serve the file it names, and pacman -U accepts an
# unsigned local file under the default LocalFileSigLevel = Optional.
verify_pinned() { # <path>
  [[ -z "${OPUS_SHA256:-}" ]] && return 0
  local actual
  actual="$(sha256sum "$1" | awk '{ print $1 }')"
  [[ "$actual" == "$OPUS_SHA256" ]] || { say "$1: sha256 $actual is not the pinned $OPUS_SHA256"; return 1; }
}

# Download the pinned package file (OPUS_PACKAGE) into pacman's cache unless it is there; every server of the official list is tried.
fetch_pinned() { # <file name>
  local file="$1" server base remaining
  # One deadline for the whole call: `timeout` cannot wrap a shell function, and curl restarts its own limit on every retry.
  local deadline=$((SECONDS + ATTEMPT_TIMEOUT))
  if [[ -s "$CACHE_DIR/$file" ]]; then
    if verify_pinned "$CACHE_DIR/$file"; then say "$file is in the package cache"; return 0; fi
    say "$file in the package cache does not match its pinned digest; downloading it again"
    rm -f "$CACHE_DIR/$file"
  fi
  while read -r server; do
    remaining=$((deadline - SECONDS))
    if ((remaining <= 0)); then say "$file: the time of this attempt is used up"; break; fi
    base="${server#*=}"
    base="${base# }"
    base="${base//\$repo/ucrt64}"
    base="${base%/}"
    # `|| true` keeps a refused or stalled server from ending the loop under -e.
    if curl -fsSL --connect-timeout 15 --max-time "$((remaining < 300 ? remaining : 300))" -o "$CACHE_DIR/$file.part" "$base/$file" && verify_pinned "$CACHE_DIR/$file.part"; then
      mv -f "$CACHE_DIR/$file.part" "$CACHE_DIR/$file"
      say "$file from $base"
      return 0
    fi
    say "$file not available from $base"
  done < <(grep -E '^Server' "$MIRRORS_DIR/mirrorlist.mingw")
  rm -f "$CACHE_DIR/$file.part"
  return 1
}

install() {
  retry "pacman -S" pacman --noconfirm --needed -S "$@"
  if [[ -n "${OPUS_PACKAGE:-}" ]]; then
    # Pinned: the newest opus is not known to work. The file is kept in the package cache, so it survives the mirror dropping it.
    retry "download of $OPUS_PACKAGE" fetch_pinned "$OPUS_PACKAGE"
    verify_pinned "$CACHE_DIR/$OPUS_PACKAGE" || { echo "::error::$OPUS_PACKAGE does not match its pinned sha256"; return 1; }
    pacman --noconfirm -U "$CACHE_DIR/$OPUS_PACKAGE"
  fi
}

# Keep exactly the package files of the installed versions (and their signatures): the saved cache must not grow with every update.
prune() {
  local keep file base stem
  # A version with an epoch (1:2.3-1) appears in a file name with the colon written differently, depending on who built the package.
  keep="$(pacman -Q | awk '{ v = $1 "-" $2; print v; if (v ~ /:/) { a = v; gsub(":", "~", a); print a; a = v; gsub(":", ".", a); print a; a = v; gsub(":", "%3a", a); print a } }')"
  shopt -s nullglob
  for file in "$CACHE_DIR"/*; do
    base="${file##*/}"
    stem="${base%.sig}"
    stem="${stem%.pkg.tar.*}"
    stem="${stem%-*}" # the architecture: any, x86_64
    if ! grep -qxF -- "$stem" <<< "$keep"; then
      rm -f -- "$file"
    fi
  done
  shopt -u nullglob
}

# One hash of what the saved cache would hold, so a run that downloaded nothing new does not save the same files again.
cache_key() {
  local sum
  sum="$(cd "$CACHE_DIR" && ls -1 | LC_ALL=C sort | sha256sum | cut -c1-16)"
  echo "$sum"
}

case "${1:-}" in
  prepare) prepare ;;
  upgrade) upgrade ;;
  install) shift; install "$@" ;;
  prune) prune ;;
  cache-key) cache_key ;;
  *) echo "usage: $0 prepare|upgrade|install [package ...]|prune|cache-key" >&2; exit 2 ;;
esac
