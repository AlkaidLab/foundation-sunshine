#!/bin/bash
# pacman for the MSYS2 toolchain of the Windows jobs, made to survive a mirror that stalls.
#
# usage (inside an MSYS2 shell):  msys2-pacman.sh prepare
#                                 msys2-pacman.sh upgrade
#                                 msys2-pacman.sh install [package ...]     (plus the pinned set of msys2-pins.txt)
#                                 msys2-pacman.sh prune
#                                 msys2-pacman.sh cache-key
#
# Why: setup-msys2's own 'update: true' runs 'pacman -Syuu' with no retry. Master run 43736645013 lost four of eight compile
# shards to "failed retrieving file 'ucrt64.db' from mirror.msys2.org : Operation too slow" (pacman's own limit: less than 1 byte/s
# for 10 s), and the one remaining job then compiled half the tree alone. A synchronised database is a few MB, so a retry is cheap.
#
#   * Every pacman call that needs the network is retried with a growing pause. Each attempt starts from a different server of the
#     official mirror list (repo.msys2.org first, then mirror.msys2.org, then the Tier 1 mirrors): pacman already falls through the
#     list for one file, but a stalled first server cost a whole attempt, and a repeated attempt against the same order repeats it.
#   * Only the [msys] and [ucrt64] repositories are synchronised (the others are never installed from): six databases and their
#     signatures become two, which is six fewer connections to stall.
#   * The toolchain no longer follows the newest repository packages. The compiler, the CRT and the linked libraries are pinned to
#     exact package files in msys2-pins.txt (full name, version, file name, sha256): MSYS2 documents installing a specific version
#     from a downloaded package archive with 'pacman -U' (source msys2-package-management-20261116). install() downloads each
#     pinned file into pacman's cache -- digest verified, every server of the official list tried -- and installs the set in one
#     'pacman -U' transaction, so mutual dependencies (gcc and gcc-libs, the CRT and its headers) resolve against the pinned
#     versions. prepare() also writes the pinned names into IgnorePkg (source pacman-conf-5-20261116): a -Syu of the base system
#     cannot move them, an explicit pacman -U still can. TOOLCHAIN_UNPINNED=1 switches back to the newest packages: the weekly
#     toolchain watch (.github/workflows/msys2-toolchain-watch.yml) dispatches such a build to test a new major first.
#   * The package files live in pacman's cache directory, which the action restores and saves with actions/cache, so a run
#     downloads only what changed since the last one. The database is always synchronised fresh (scripts/ci/msys2-pins.py reads
#     the same repository for the weekly watch). The cache key carries the toolchain mode, so a probe build with the newest
#     packages never replaces the pinned toolchain's cache entry.
#
# Paths can be moved for the unit test (scripts/ci/test_msys2_pacman.py): MSYS2_ROOT is the root of the MSYS2 tree (default /).
set -euo pipefail

ROOT="${MSYS2_ROOT:-}"
CONF="$ROOT/etc/pacman.conf"
MIRRORS_DIR="$ROOT/etc/pacman.d"
CACHE_DIR="${PACMAN_CACHE_DIR:-$ROOT/var/cache/pacman/pkg}"
LOCK="${PACMAN_LOCK:-$ROOT/var/lib/pacman/db.lck}"
PINS_FILE="${PINS_FILE:-$(cd "$(dirname "$0")" && pwd)/msys2-pins.txt}"
# Clang and LLD (and the LLVM libraries their versions are locked to) are pinned in a second file that only a Clang build reads,
# so the GCC builds install nothing of it. TOOLCHAIN is the compiler of the job (gcc, the default, or clang).
CLANG_PINS_FILE="${CLANG_PINS_FILE:-$(cd "$(dirname "$0")" && pwd)/msys2-pins-clang.txt}"
TOOLCHAIN="${TOOLCHAIN:-gcc}"
UNPINNED="${TOOLCHAIN_UNPINNED:-0}"
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

# The pinned entries of msys2-pins.txt: full package name, version, file name, sha256. Comments and blank lines are skipped.
pins() {
  [[ -r "$PINS_FILE" ]] || return 0
  awk 'NF >= 4 && $1 !~ /^#/ { print }' "$PINS_FILE"
  # A Clang build pins the Clang packages as well (require_pins fails first when that file is missing).
  if [[ "$TOOLCHAIN" == clang && -r "$CLANG_PINS_FILE" ]]; then
    awk 'NF >= 4 && $1 !~ /^#/ { print }' "$CLANG_PINS_FILE"
  fi
}

pin_field() { # <field 1-4> <full package name>
  pins | awk -v name="$2" -v field="$1" '$1 == name { print $field; exit }' || true
}

is_pinned() { [[ -n "$(pin_field 1 "$1")" ]]; }

# A pinned toolchain without the pins file would silently float with the repository again -- exactly what the pins are for.
require_pins() {
  if [[ ! -r "$PINS_FILE" ]]; then
    echo "::error::the toolchain is pinned, but $PINS_FILE does not exist; TOOLCHAIN_UNPINNED=1 installs the newest packages"
    return 1
  fi
  if [[ "$TOOLCHAIN" == clang && ! -r "$CLANG_PINS_FILE" ]]; then
    echo "::error::the Clang toolchain is pinned, but $CLANG_PINS_FILE does not exist; TOOLCHAIN_UNPINNED=1 installs the newest packages"
    return 1
  fi
}

prepare() {
  [[ "$UNPINNED" == 1 ]] || require_pins
  # Space for the packages is checked against the drive of the cache dir, which is not what the runner reports; setup-msys2 does the same.
  sed -i 's/^CheckSpace/#CheckSpace/' "$CONF"
  # Comment every line of the repositories that are not used: [msys] (base system) and [ucrt64] (the toolchain) are all that is installed.
  awk '
    /^\[/ { keep = ($0 == "[options]" || $0 == "[msys]" || $0 == "[ucrt64]") }
    { if (!keep && $0 != "" && $0 !~ /^#/) print "#" $0; else print }
  ' "$CONF" > "$CONF.new"
  mv -f "$CONF.new" "$CONF"
  write_mirrors 1
  # IgnorePkg keeps the base upgrade below (and any later -Syu) from moving a pinned package; the explicit pacman -U of
  # install() is not affected. The line is replaced, never appended, so a second prepare -- after pacman.conf was replaced
  # by its .pacnew -- stays idempotent.
  local ignored
  ignored="$(pins | awk '{ printf "%s%s", sep, $1; sep = " " }')"
  if [[ "$UNPINNED" != 1 && -n "$ignored" ]]; then
    sed -i '/^IgnorePkg[[:space:]]*=/d' "$CONF"
    sed -i "/^\[options\]/a IgnorePkg = $ignored" "$CONF"
  fi
  mkdir -p "$CACHE_DIR"
}

upgrade() {
  # -Syuu as setup-msys2 does it: the second u allows the downgrade of a package the repository has rolled back.
  retry "pacman -Syuu" pacman --noconfirm -Syuu --overwrite '*'
}

# A pinned file must match the sha256 recorded in msys2-pins.txt: a mirror cannot be trusted to serve the file it names, and
# pacman -U accepts an unsigned local file under the default LocalFileSigLevel = Optional.
verify_pinned() { # <path> <sha256>
  local actual
  actual="$(sha256sum "$1" | awk '{ print $1 }')"
  [[ "$actual" == "$2" ]] || { say "$1: sha256 $actual is not the pinned $2"; return 1; }
}

# Download one pinned package file into pacman's cache unless it is there; every server of the official list is tried.
fetch_pinned() { # <file name> <sha256>
  local file="$1" digest="$2" server base remaining
  # One deadline for the whole call: `timeout` cannot wrap a shell function, and curl restarts its own limit on every retry.
  local deadline=$((SECONDS + ATTEMPT_TIMEOUT))
  if [[ -s "$CACHE_DIR/$file" ]]; then
    if verify_pinned "$CACHE_DIR/$file" "$digest"; then say "$file is in the package cache"; return 0; fi
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
    if curl -fsSL --connect-timeout 15 --max-time "$((remaining < 300 ? remaining : 300))" -o "$CACHE_DIR/$file.part" "$base/$file" && verify_pinned "$CACHE_DIR/$file.part" "$digest"; then
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
  if [[ "$UNPINNED" == 1 ]]; then
    retry "pacman -S" pacman --noconfirm --needed -S "$@"
    return 0
  fi
  require_pins
  local -a plain=() files=()
  local name version file digest
  for name in "$@"; do
    is_pinned "$name" || plain+=("$name")
  done
  # The floating packages first: curl, the downloader below, is one of them. A pinned name is never passed to -S, so pacman
  # cannot resolve it to the repository's newer version while the pinned file is on its way.
  if [[ "${#plain[@]}" -gt 0 ]]; then
    retry "pacman -S" pacman --noconfirm --needed -S "${plain[@]}"
  fi
  # The whole pinned set in one transaction: mutual dependencies resolve against the pinned versions, not the
  # repository's, and a pin is installed even when the caller did not name it.
  while read -r name version file digest; do
    retry "download of $file" fetch_pinned "$file" "$digest"
    files+=("$CACHE_DIR/$file")
  done < <(pins)
  if [[ "${#files[@]}" -gt 0 ]]; then
    pacman --noconfirm -U "${files[@]}"
  fi
  return 0
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

# One hash of what the saved cache would hold, prefixed with the toolchain mode, so a run that downloaded nothing new does not
# save the same files again and the probe builds with the newest packages never replace the pinned toolchain's cache entry.
cache_key() {
  local mode=pinned sum
  [[ "$UNPINNED" == 1 ]] && mode=newest
  sum="$(cd "$CACHE_DIR" && ls -1 | LC_ALL=C sort | sha256sum | cut -c1-16)"
  echo "$mode-$sum"
}

case "${1:-}" in
  prepare) prepare ;;
  upgrade) upgrade ;;
  install) shift; install "$@" ;;
  prune) prune ;;
  cache-key) cache_key ;;
  *) echo "usage: $0 prepare|upgrade|install [package ...]|prune|cache-key" >&2; exit 2 ;;
esac
