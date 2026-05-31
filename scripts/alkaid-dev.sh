#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
HOST_HELPER="${SCRIPT_DIR}/sunshine-remote-macos.sh"

HOST="${SUNSHINE_HOST:-}"
QT_SRC="${QT_SRC:-}"
QT_BUILD_DIR="${QT_BUILD_DIR:-}"
QT_CONFIG="${QT_CONFIG:-release}"
QT_EXTRA_CONFIG="${QT_EXTRA_CONFIG:-disable-prebuilts}"
QT_QMAKE="${QT_QMAKE:-}"
QT_MAKE="${QT_MAKE:-}"
QT_INSTALL_APP="${QT_INSTALL_APP:-}"
QT_ARGS=()
HOST_ARGS=()
PROFILE_NAME="${ALKAID_PROFILE:-}"
PROFILE_DIR="${ALKAID_PROFILE_DIR:-${HOME}/.alkaidlab/devkit/profiles}"

LANG_CHOICE="${ALKAID_LANG:-zh}"
detect_lang() {
  case "${LANG_CHOICE}" in
    zh|zh_CN|zh-Hans|cn) echo zh ;;
    en|en_US|en-GB) echo en ;;
    auto|"")
      case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in zh*|ZH*) echo zh ;; *) echo en ;; esac ;;
    *) echo en ;;
  esac
}
tr() {
  local key="$1"
  case "$(detect_lang):$key" in
    zh:err_prefix) echo "错误" ;;
    zh:qt_missing) echo "找不到 Moonlight-Qt 源码目录；请传 --qt-src <路径>" ;;
    zh:qmake_missing) echo "找不到 qmake；请安装 Qt 或传 --qmake <路径>" ;;
    zh:make_missing) echo "找不到构建工具；请传 --make <make|jom|nmake>" ;;
    zh:helper_missing) echo "host helper 不存在或不可执行" ;;
    zh:qt_not_built) echo "Qt 客户端还没构建。请先运行 qt-build。" ;;
    zh:opening_qt) echo "正在打开 Qt 客户端" ;;
    zh:removing) echo "正在删除" ;;
    *)
      case "$key" in
        err_prefix) echo "error" ;;
        qt_missing) echo "cannot find Moonlight-Qt checkout; pass --qt-src <path>" ;;
        qmake_missing) echo "qmake not found; install Qt or pass --qmake <path>" ;;
        make_missing) echo "build tool not found; pass --make <make|jom|nmake>" ;;
        helper_missing) echo "host helper missing or not executable" ;;
        qt_not_built) echo "Qt app not built. Run qt-build first." ;;
        opening_qt) echo "Opening Qt client" ;;
        removing) echo "Removing" ;;
        *) echo "$key" ;;
      esac ;;
  esac
}

usage() {
  if [[ "$(detect_lang)" == "zh" ]]; then
    cat <<'USAGE'
AlkaidLab 通用开发 / 部署助手

用法:
  scripts/alkaid-dev.sh [全局选项] <命令> [命令选项]

Host/Sunshine 命令，通过 SSH 操作 Windows Sunshine 主机:
  host-status              查看 Sunshine 服务、进程、hash、监听端口状态。
  host-logs                查看 Sunshine 日志尾部。
  host-events              查看最近 Sunshine 崩溃 / WER 事件。
  host-start-user          在 Windows 交互用户会话中启动 Sunshine。
  host-stop                停止 SunshineService 和 Sunshine.exe。
  host-deploy-user         上传 --exe 并部署 Sunshine.exe，然后以用户态启动。
  host-deploy-assets       上传 --assets 并安装 Sunshine 运行资源。

Moonlight-Qt 命令，本机执行，支持 macOS/Linux/Windows MSYS2 或 Git Bash:
  qt-status                查看 Qt 源码、构建目录、应用和工具状态。
  qt-build                 通过 qmake + make/jom/nmake 配置并构建 Moonlight Qt。
  qt-open                  打开/运行已构建的 Qt 客户端。
  qt-package               在支持的平台构建分发包。
  qt-clean                 删除配置的 Qt 构建目录。
  profile-set              保存当前参数为 profile。
  profile-list             列出已有 profile。
  profile-show             显示指定 profile。
  profile-remove           删除指定 profile。

全局选项:
  --lang <zh|en|auto>      语言。默认 zh，也可用 ALKAID_LANG。
  --profile <name>         读取/保存 profile；默认目录 ~/.alkaidlab/devkit/profiles。
  --host <ssh-host>        host-* 命令的 SSH 目标。也可用 SUNSHINE_HOST。
  --qt-src <path>          Moonlight-Qt checkout。也可用 QT_SRC。会自动探测当前 repo。
  --qt-build-dir <path>    构建目录。默认 <qt-src>/build-devkit-<platform>。
  --qt-config <debug|release>
                           默认 release。
  --qt-extra-config <cfg>  额外 qmake CONFIG。默认 disable-prebuilts。传 '' 可禁用。
  --qmake <path/name>      qmake 命令。Unix 默认 qmake6/qmake，Windows 默认 qmake。
  --make <path/name>       构建工具。Unix 默认 make，Windows 默认 jom/nmake/mingw32-make/make。
  --install-app <path>     macOS 下 qt-build 可将 Moonlight.app 复制到这里。
  --exe <path>             host-deploy-user 使用的 Sunshine.exe。
  --assets <path>          host-deploy-assets 使用的 Sunshine assets 文件夹。
  --tail <n>               host 日志/事件行数。
  --ssh-option <option>    额外 ssh/scp 选项，可重复。
  -h, --help               显示帮助。

示例:
  ./scripts/alkaid-dev.sh --lang zh --qt-src ~/black-qt qt-build
  ./scripts/alkaid-dev.sh --qt-src ~/black-qt qt-open
  ./scripts/alkaid-dev.sh --host user@192.168.1.50 --exe ./build/sunshine.exe host-deploy-user
  ./scripts/alkaid-dev.sh --host win-host host-status
  ./scripts/alkaid-dev.sh --profile office --host win-host --qt-src ~/black-qt profile-set
  ./scripts/alkaid-dev.sh --profile office host-deploy-user

说明:
  这个 public helper 不包含实验室专用主机昵称。通过 --host / --qt-src 或环境变量配置。
  Windows 协作者可从 MSYS2、Git Bash，或任何带 ssh/scp 的 Bash 环境运行。
USAGE
    return
  fi
  cat <<'USAGE'
AlkaidLab generic dev/deploy helper

Usage:
  scripts/alkaid-dev.sh [global options] <command> [command options]

Host/Sunshine commands, over SSH to a Windows Sunshine host:
  host-status              Show Sunshine service/process/hash/listener status.
  host-logs                Tail Sunshine log.
  host-events              Show recent Sunshine crash/WER events.
  host-start-user          Start Sunshine in the interactive Windows user session.
  host-stop                Stop SunshineService and Sunshine.exe.
  host-deploy-user         Upload --exe and deploy Sunshine.exe, then start user-mode.
  host-deploy-assets       Upload --assets and install Sunshine runtime assets.

Moonlight-Qt commands, local machine, works on macOS/Linux/Windows MSYS2/Git Bash:
  qt-status                Show Qt source/build/app status and detected tools.
  qt-build                 Configure/build Moonlight Qt via qmake + make/jom/nmake.
  qt-open                  Open/run the built Qt client.
  qt-package               Build distributable package when supported.
  qt-clean                 Remove the configured Qt build dir.
  profile-set              Save current options as a profile.
  profile-list             List profiles.
  profile-show             Show a profile.
  profile-remove           Remove a profile.

Global options:
  --host <ssh-host>        SSH target for host-* commands. Or SUNSHINE_HOST.
  --qt-src <path>          Moonlight-Qt checkout. Or QT_SRC. Auto-detects current repo.
  --qt-build-dir <path>    Build dir. Default: <qt-src>/build-devkit-<platform>.
  --qt-config <debug|release>
                           Default: release.
  --qt-extra-config <cfg>  Extra qmake CONFIG values. Default: disable-prebuilts.
                           Use '' to disable.
  --qmake <path/name>      qmake command. Default: qmake6/qmake on Unix, qmake on Windows.
  --make <path/name>       build tool. Default: make on Unix, jom/nmake/mingw32-make/make on Windows.
  --install-app <path>     macOS qt-open/qt-build can copy Moonlight.app here.
  --exe <path>             Sunshine.exe for host-deploy-user.
  --assets <path>          Sunshine assets folder for host-deploy-assets.
  --tail <n>               Host log/event tail. Passed to host helper.
  --ssh-option <option>    Extra ssh/scp option. Repeatable.
  --lang <zh|en|auto>      Language. Default: zh. Or ALKAID_LANG.
  --profile <name>         Load/save profile. Default dir: ~/.alkaidlab/devkit/profiles.
  -h, --help               Show this help.

Examples:
  # macOS/Linux local Qt build
  ./scripts/alkaid-dev.sh --qt-src ~/black-qt qt-build
  ./scripts/alkaid-dev.sh --qt-src ~/black-qt qt-open

  # Windows MSYS2/Git Bash local Qt build, from a Qt/VS developer prompt or MSYS shell
  ./scripts/alkaid-dev.sh --qt-src /c/dev/black-qt --qt-config debug qt-build
  ./scripts/alkaid-dev.sh --qt-src /c/dev/black-qt qt-open

  # Deploy Sunshine.exe to any Windows host reachable by SSH
  ./scripts/alkaid-dev.sh --host user@192.168.1.50 --exe ./build/sunshine.exe host-deploy-user
  ./scripts/alkaid-dev.sh --host win-host host-status
  ./scripts/alkaid-dev.sh --profile office --host win-host --qt-src ~/black-qt profile-set
  ./scripts/alkaid-dev.sh --profile office host-deploy-user

Notes:
  This public helper has no lab-specific host nicknames. Configure targets with
  --host / --qt-src or environment variables. Windows collaborators can run it
  from MSYS2, Git Bash, or any Bash with ssh/scp in PATH.
USAGE
}

fail() { echo "$(tr err_prefix): $*" >&2; exit 1; }
is_windows() { [[ "${OS:-}" == "Windows_NT" ]] || uname -s | grep -qiE 'mingw|msys|cygwin'; }
is_macos() { [[ "$(uname -s 2>/dev/null || true)" == "Darwin" ]]; }

abs_path() {
  local p="$1"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$p" <<'PY'
import os, sys
print(os.path.abspath(os.path.expanduser(sys.argv[1])))
PY
  else
    (cd "$(dirname "$p")" && printf '%s/%s\n' "$PWD" "$(basename "$p")")
  fi
}

detect_qt_src() {
  if [[ -n "$QT_SRC" ]]; then
    QT_SRC="$(abs_path "$QT_SRC")"
    return
  fi
  if [[ -f "moonlight-qt.pro" ]]; then
    QT_SRC="$PWD"
  elif [[ -f "${REPO_ROOT}/moonlight-qt.pro" ]]; then
    QT_SRC="$REPO_ROOT"
  elif [[ -d "${HOME}/black-qt" && -f "${HOME}/black-qt/moonlight-qt.pro" ]]; then
    QT_SRC="${HOME}/black-qt"
  else
    fail "$(tr qt_missing)"
  fi
  QT_SRC="$(abs_path "$QT_SRC")"
}

qt_build_dir() {
  detect_qt_src
  if [[ -z "$QT_BUILD_DIR" ]]; then
    local suffix
    if is_macos; then suffix="macos"; elif is_windows; then suffix="windows"; else suffix="linux"; fi
    QT_BUILD_DIR="${QT_SRC}/build-devkit-${suffix}"
  fi
  QT_BUILD_DIR="$(abs_path "$QT_BUILD_DIR")"
}

detect_qmake() {
  if [[ -n "$QT_QMAKE" ]]; then return; fi
  if is_windows; then
    for c in qmake qmake.exe qmake.bat host-qmake.bat; do command -v "$c" >/dev/null 2>&1 && { QT_QMAKE="$c"; return; }; done
  else
    for c in qmake6 qmake qmake-qt6; do command -v "$c" >/dev/null 2>&1 && { QT_QMAKE="$c"; return; }; done
  fi
  fail "$(tr qmake_missing)"
}

detect_make() {
  if [[ -n "$QT_MAKE" ]]; then return; fi
  if is_windows; then
    for c in jom.exe jom nmake.exe nmake mingw32-make make; do command -v "$c" >/dev/null 2>&1 && { QT_MAKE="$c"; return; }; done
  else
    for c in gmake make; do command -v "$c" >/dev/null 2>&1 && { QT_MAKE="$c"; return; }; done
  fi
  fail "$(tr make_missing)"
}

qt_binary_candidates() {
  qt_build_dir
  if is_macos; then
    printf '%s\n' \
      "${QT_BUILD_DIR}/app/Moonlight.app" \
      "${QT_BUILD_DIR}/Moonlight.app" \
      "${QT_BUILD_DIR}/app/${QT_CONFIG}/Moonlight.app"
  elif is_windows; then
    printf '%s\n' \
      "${QT_BUILD_DIR}/app/${QT_CONFIG}/Moonlight.exe" \
      "${QT_BUILD_DIR}/app/Moonlight.exe" \
      "${QT_BUILD_DIR}/Moonlight.exe" \
      "${QT_SRC}/build/app/${QT_CONFIG}/Moonlight.exe"
  else
    printf '%s\n' \
      "${QT_BUILD_DIR}/app/moonlight" \
      "${QT_BUILD_DIR}/app/Moonlight" \
      "${QT_BUILD_DIR}/moonlight"
  fi
}

find_qt_binary() {
  local p
  while IFS= read -r p; do
    [[ -e "$p" ]] && { printf '%s\n' "$p"; return 0; }
  done < <(qt_binary_candidates)
  return 1
}

qt_status() {
  qt_build_dir
  detect_qmake || true
  detect_make || true
  echo "== Qt devkit status =="
  echo "platform      : $(uname -s 2>/dev/null || echo Windows)"
  echo "qt_src        : $QT_SRC"
  echo "qt_build_dir  : $QT_BUILD_DIR"
  echo "qt_config     : $QT_CONFIG"
  echo "extra_config  : $QT_EXTRA_CONFIG"
  echo "qmake         : ${QT_QMAKE:-not found}"
  echo "make          : ${QT_MAKE:-not found}"
  echo "install_app   : ${QT_INSTALL_APP:-not set}"
  echo "== source =="
  [[ -f "${QT_SRC}/moonlight-qt.pro" ]] && echo "moonlight-qt.pro: present" || echo "moonlight-qt.pro: missing"
  if command -v git >/dev/null 2>&1 && [[ -d "${QT_SRC}/.git" ]]; then
    git -C "$QT_SRC" status --short | sed 's/^/  /' || true
  fi
  echo "== built app candidates =="
  local found=0 p
  while IFS= read -r p; do
    if [[ -e "$p" ]]; then echo "FOUND $p"; found=1; else echo "miss  $p"; fi
  done < <(qt_binary_candidates)
  [[ $found -eq 1 ]] || true
}

qt_build() {
  qt_build_dir
  detect_qmake
  detect_make
  mkdir -p "$QT_BUILD_DIR"
  echo "== qmake =="
  (
    cd "$QT_BUILD_DIR"
    local qargs=()
    if [[ -n "$QT_EXTRA_CONFIG" ]]; then
      # shellcheck disable=SC2206
      local cfg_parts=( $QT_EXTRA_CONFIG )
      for cfg in "${cfg_parts[@]}"; do qargs+=("CONFIG+=${cfg}"); done
    fi
    "${QT_QMAKE}" "${qargs[@]}" "${QT_SRC}/moonlight-qt.pro" "${QT_ARGS[@]}"
  )
  echo "== build ${QT_CONFIG} =="
  (
    cd "$QT_BUILD_DIR"
    if [[ "$QT_MAKE" == *nmake* ]]; then
      "$QT_MAKE" "$QT_CONFIG"
    else
      "$QT_MAKE" -j"${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}" "$QT_CONFIG"
    fi
  )
  if [[ -n "$QT_INSTALL_APP" && "$(is_macos; echo $?)" == "0" ]]; then
    local app
    app="$(find_qt_binary || true)"
    if [[ -n "$app" && -d "$app" ]]; then
      mkdir -p "$(dirname "$QT_INSTALL_APP")"
      rm -rf "$QT_INSTALL_APP"
      cp -R "$app" "$QT_INSTALL_APP"
      codesign --force --deep --timestamp=none --sign - "$QT_INSTALL_APP" >/dev/null 2>&1 || true
      echo "Installed macOS app: $QT_INSTALL_APP"
    fi
  fi
  qt_status
}

qt_open() {
  local app
  app="$(find_qt_binary || true)"
  [[ -n "$app" ]] || fail "$(tr qt_not_built)"
  echo "$(tr opening_qt): $app"
  if is_macos; then
    if [[ -d "$app" ]]; then open -n "$app"; else open -n "$app"; fi
  elif is_windows; then
    if command -v powershell.exe >/dev/null 2>&1; then
      powershell.exe -NoProfile -Command "Start-Process -FilePath '$app'" >/dev/null
    else
      "$app" >/dev/null 2>&1 &
    fi
  else
    "$app" >/dev/null 2>&1 &
  fi
}

qt_package() {
  qt_build_dir
  detect_make
  if [[ -f "${QT_SRC}/scripts/build-arch.bat" && is_windows ]]; then
    (cd "$QT_SRC" && cmd.exe /c scripts\\build-arch.bat "$QT_CONFIG")
  elif [[ -f "${QT_SRC}/scripts/generate-dmg.sh" && is_macos ]]; then
    (cd "$QT_SRC" && scripts/generate-dmg.sh)
  elif [[ -f "${QT_SRC}/scripts/build-appimage.sh" ]]; then
    (cd "$QT_SRC" && scripts/build-appimage.sh)
  else
    echo "No platform package script detected; building ${QT_CONFIG} only."
    qt_build
  fi
}

qt_clean() {
  qt_build_dir
  echo "$(tr removing) $QT_BUILD_DIR"
  rm -rf "$QT_BUILD_DIR"
}

profile_path() { printf '%s/%s.env
' "$PROFILE_DIR" "$1"; }

load_profile() {
  [[ -n "$PROFILE_NAME" ]] || return 0
  local f; f="$(profile_path "$PROFILE_NAME")"
  [[ -f "$f" ]] || fail "profile not found: $PROFILE_NAME ($f)"
  # shellcheck disable=SC1090
  source "$f"
  HOST="${SUNSHINE_HOST:-$HOST}"
  QT_SRC="${QT_SRC:-}"
  QT_BUILD_DIR="${QT_BUILD_DIR:-}"
  QT_CONFIG="${QT_CONFIG:-$QT_CONFIG}"
  QT_EXTRA_CONFIG="${QT_EXTRA_CONFIG:-$QT_EXTRA_CONFIG}"
  QT_QMAKE="${QT_QMAKE:-}"
  QT_MAKE="${QT_MAKE:-}"
  QT_INSTALL_APP="${QT_INSTALL_APP:-}"
}

profile_list() {
  mkdir -p "$PROFILE_DIR"
  echo "== profiles: $PROFILE_DIR =="
  find "$PROFILE_DIR" -maxdepth 1 -type f -name '*.env' -print 2>/dev/null | sed 's#.*/##; s#\.env$##' | sort
}

profile_show() {
  [[ -n "$PROFILE_NAME" ]] || fail "profile-show requires --profile <name>"
  local f; f="$(profile_path "$PROFILE_NAME")"
  [[ -f "$f" ]] || fail "profile not found: $PROFILE_NAME ($f)"
  sed -n '1,220p' "$f"
}

profile_remove() {
  [[ -n "$PROFILE_NAME" ]] || fail "profile-remove requires --profile <name>"
  local f; f="$(profile_path "$PROFILE_NAME")"
  rm -f "$f"
  echo "removed profile: $PROFILE_NAME"
}

profile_set() {
  [[ -n "$PROFILE_NAME" ]] || fail "profile-set requires --profile <name>"
  mkdir -p "$PROFILE_DIR"
  local f; f="$(profile_path "$PROFILE_NAME")"
  {
    echo '# AlkaidLab devkit profile'
    echo '# Generated by scripts/alkaid-dev.sh profile-set'
    [[ -n "$HOST" ]] && printf 'SUNSHINE_HOST=%q\n' "$HOST"
    [[ -n "${SUNSHINE_LOCAL_EXE:-}" ]] && printf 'SUNSHINE_LOCAL_EXE=%q\n' "$SUNSHINE_LOCAL_EXE"
    [[ -n "${SUNSHINE_ASSETS_DIR:-}" ]] && printf 'SUNSHINE_ASSETS_DIR=%q\n' "$SUNSHINE_ASSETS_DIR"
    [[ -n "$QT_SRC" ]] && printf 'QT_SRC=%q\n' "$QT_SRC"
    [[ -n "$QT_BUILD_DIR" ]] && printf 'QT_BUILD_DIR=%q\n' "$QT_BUILD_DIR"
    [[ -n "$QT_CONFIG" ]] && printf 'QT_CONFIG=%q\n' "$QT_CONFIG"
    [[ -n "$QT_EXTRA_CONFIG" ]] && printf 'QT_EXTRA_CONFIG=%q\n' "$QT_EXTRA_CONFIG"
    [[ -n "$QT_QMAKE" ]] && printf 'QT_QMAKE=%q\n' "$QT_QMAKE"
    [[ -n "$QT_MAKE" ]] && printf 'QT_MAKE=%q\n' "$QT_MAKE"
    [[ -n "$QT_INSTALL_APP" ]] && printf 'QT_INSTALL_APP=%q\n' "$QT_INSTALL_APP"
    [[ -n "${ALKAID_LANG:-}" ]] && printf 'ALKAID_LANG=%q\n' "$ALKAID_LANG"
  } > "$f"
  echo "saved profile: $PROFILE_NAME -> $f"
}

run_host_helper() {
  [[ -x "$HOST_HELPER" ]] || fail "$(tr helper_missing): $HOST_HELPER"
  local cmd="$1"; shift || true
  [[ -n "$HOST" ]] && HOST_ARGS+=(--host "$HOST")
  "$HOST_HELPER" "${HOST_ARGS[@]}" "$cmd" "$@"
}

# Load --profile/ALKAID_PROFILE before executing commands. Explicit CLI options parsed below override profile values.
for ((i=1; i<=$#; i++)); do
  if [[ "${!i}" == "--profile" ]]; then
    j=$((i+1)); PROFILE_NAME="${!j:-}"; break
  fi
done
load_profile

COMMAND=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --lang) LANG_CHOICE="${2:-zh}"; shift 2 ;;
    --profile) PROFILE_NAME="${2:-}"; load_profile; shift 2 ;;
    --host) HOST="${2:-}"; shift 2 ;;
    --qt-src) QT_SRC="${2:-}"; shift 2 ;;
    --qt-build-dir) QT_BUILD_DIR="${2:-}"; shift 2 ;;
    --qt-config) QT_CONFIG="${2:-}"; shift 2 ;;
    --qt-extra-config) QT_EXTRA_CONFIG="${2:-}"; shift 2 ;;
    --qmake) QT_QMAKE="${2:-}"; shift 2 ;;
    --make) QT_MAKE="${2:-}"; shift 2 ;;
    --install-app) QT_INSTALL_APP="${2:-}"; shift 2 ;;
    --exe) SUNSHINE_LOCAL_EXE="${2:-}"; HOST_ARGS+=("$1" "${2:-}"); shift 2 ;;
    --assets) SUNSHINE_ASSETS_DIR="${2:-}"; HOST_ARGS+=("$1" "${2:-}"); shift 2 ;;
    --tail|--ssh-option|--remote-agent|--install-dir|--stage-dir|--task-name|--start-args|--capture-backend)
      HOST_ARGS+=("$1" "${2:-}"); shift 2 ;;
    --qmake-arg) QT_ARGS+=("${2:-}"); shift 2 ;;
    -h|--help|help) usage; exit 0 ;;
    host-status|host-logs|host-events|host-start-user|host-stop|host-deploy-user|host-deploy-assets|qt-status|qt-build|qt-open|qt-package|qt-clean|profile-set|profile-list|profile-show|profile-remove)
      COMMAND="$1"; shift ;;
    *) fail "unknown argument: $1" ;;
  esac
done

case "${COMMAND:-}" in
  host-status) run_host_helper status ;;
  host-logs) run_host_helper logs ;;
  host-events) run_host_helper events ;;
  host-start-user) run_host_helper start-user ;;
  host-stop) run_host_helper stop ;;
  host-deploy-user) run_host_helper deploy-user ;;
  host-deploy-assets) run_host_helper deploy-assets ;;
  qt-status) qt_status ;;
  qt-build) qt_build ;;
  qt-open) qt_open ;;
  qt-package) qt_package ;;
  qt-clean) qt_clean ;;
  profile-set) profile_set ;;
  profile-list) profile_list ;;
  profile-show) profile_show ;;
  profile-remove) profile_remove ;;
  "") usage; exit 0 ;;
  *) usage; exit 2 ;;
esac
