#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WINDOWS_AGENT_DEFAULT="${SCRIPT_DIR}/sunshine-host-windows.ps1"

SUNSHINE_HOST="${SUNSHINE_HOST:-}"
REMOTE_AGENT="${SUNSHINE_REMOTE_AGENT:-C:\\ProgramData\\SunshineDeployTools\\sunshine-host-windows.ps1}"
INSTALL_DIR="${SUNSHINE_INSTALL_DIR:-C:\\Program Files\\Sunshine}"
STAGE_DIR="${SUNSHINE_STAGE_DIR:-C:\\ProgramData\\SunshineDeployTools\\stage}"
TASK_NAME="${SUNSHINE_TASK_NAME:-SunshineUserMode}"
WINDOWS_AGENT="$WINDOWS_AGENT_DEFAULT"
LOCAL_EXE="${SUNSHINE_LOCAL_EXE:-}"
ASSETS_DIR="${SUNSHINE_ASSETS_DIR:-}"
START_ARGS="${SUNSHINE_START_ARGS:-}"
CAPTURE_BACKEND="${SUNSHINE_CAPTURE_BACKEND:-wgc}"
TAIL="${SUNSHINE_TAIL:-220}"
SSH_OPTS=()

LANG_CHOICE="${ALKAID_LANG:-zh}"
detect_lang() {
  case "${LANG_CHOICE}" in
    zh|zh_CN|zh-Hans|cn) echo zh ;;
    en|en_US|en-GB) echo en ;;
    auto|"") case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in zh*|ZH*) echo zh ;; *) echo en ;; esac ;;
    *) echo en ;;
  esac
}
tr() {
  local key="$1"
  case "$(detect_lang):$key" in
    zh:err_prefix) echo "错误" ;;
    zh:missing_host) echo "缺少 --host 或 SUNSHINE_HOST" ;;
    zh:agent_missing) echo "找不到 Windows agent" ;;
    zh:exe_required) echo "deploy-user 需要 --exe <本地 Sunshine.exe>" ;;
    zh:exe_missing) echo "找不到本地 exe" ;;
    zh:assets_required) echo "deploy-assets 需要 --assets <本地 assets 目录>" ;;
    zh:assets_missing) echo "找不到本地 assets 目录" ;;
    zh:uploading) echo "正在上传" ;;
    zh:unknown_choice) echo "未知选择" ;;
    zh:select_action) echo "选择操作" ;;
    zh:local_exe_path) echo "本地 Sunshine.exe 路径" ;;
    zh:local_assets_dir) echo "本地 assets 目录" ;;
    *)
      case "$key" in
        err_prefix) echo "error" ;; missing_host) echo "missing --host or SUNSHINE_HOST" ;;
        agent_missing) echo "Windows agent not found" ;; exe_required) echo "deploy-user requires --exe <local-Sunshine.exe>" ;;
        exe_missing) echo "missing local exe" ;; assets_required) echo "deploy-assets requires --assets <local-assets-dir>" ;;
        assets_missing) echo "missing local assets directory" ;; uploading) echo "Uploading" ;;
        unknown_choice) echo "Unknown choice" ;; select_action) echo "Select action" ;;
        local_exe_path) echo "Local Sunshine.exe path" ;; local_assets_dir) echo "Local assets directory" ;;
        *) echo "$key" ;;
      esac ;;
  esac
}

usage() {
  if [[ "$(detect_lang)" == "zh" ]]; then
    cat <<'USAGE'
通用 Sunshine 远程助手（macOS/Linux/Windows MSYS2 Bash）

用法:
  scripts/sunshine-remote-macos.sh [全局选项] <命令> [命令选项]

命令:
  interactive             显示交互菜单。
  install-agent           只上传 Windows PowerShell agent。
  status                  查看远程 Sunshine 服务、进程、hash、监听端口状态。
  logs                    查看远程 Sunshine 日志尾部。
  events                  查看最近 Windows Application Error / WER 事件。
  start-user              在 Windows 交互用户会话中启动 Sunshine。
  stop                    停止 SunshineService 和所有 Sunshine.exe。
  deploy-user             上传 --exe 并部署，然后以用户态启动 Sunshine。
  deploy-assets           将 --assets 打包上传并复制到 InstallDir\assets。
  help, --help, -h        显示帮助。

全局选项:
  --lang <zh|en|auto>           语言。默认 zh，也可用 ALKAID_LANG。
  --host <ssh-host>             SSH 目标，例如 user@host 或 ~/.ssh/config alias。也可用 SUNSHINE_HOST。
  --agent <path.ps1>            要上传的本地 Windows agent 脚本。默认 scripts/sunshine-host-windows.ps1。
  --remote-agent <win-path>     远程 agent 路径。
  --install-dir <win-path>      Sunshine 安装目录。默认 C:\Program Files\Sunshine。
  --stage-dir <win-path>        远程暂存目录。
  --task-name <name>            用户态启动计划任务名。默认 SunshineUserMode。
  --exe <local-Sunshine.exe>    deploy-user 使用的本地 Sunshine.exe。
  --assets <local-assets-dir>   deploy-assets 使用的本地 assets 目录。
  --start-args <args>           额外 Sunshine 命令行参数。
  --capture-backend <name>      start-user 强制的 capture backend。默认 wgc；传 '' 跳过。
  --tail <n>                    日志/事件行数。默认 220。
  --ssh-option <option>         额外 ssh/scp 选项，可重复。

示例:
  SUNSHINE_HOST=win-host ./scripts/sunshine-remote-macos.sh status
  ./scripts/sunshine-remote-macos.sh --host user@192.168.1.50 --exe ./build/sunshine.exe deploy-user
  ./scripts/sunshine-remote-macos.sh --host win-host --assets ./src_assets/windows/assets deploy-assets
  ./scripts/sunshine-remote-macos.sh --host win-host interactive

此脚本不包含实验室专用主机昵称。所有目标通过 --host 或环境变量配置。
USAGE
    return
  fi
  cat <<'USAGE'
Generic Sunshine remote helper for macOS/Linux

Usage:
  scripts/sunshine-remote-macos.sh [global options] <command> [command options]

Commands:
  interactive             Show an interactive menu.
  install-agent           Upload the Windows PowerShell agent only.
  status                  Show remote Sunshine service/process/hash/listener status.
  logs                    Tail remote Sunshine log.
  events                  Show recent Windows Application Error / WER entries.
  start-user              Start Sunshine in the interactive Windows user session.
  stop                    Stop SunshineService and all Sunshine.exe processes.
  deploy-user             Upload --exe and deploy it, then start user-mode Sunshine.
  deploy-assets           Upload --assets as a zip and copy it to InstallDir\assets.
  help, --help, -h        Show this help.

Global options:
  --lang <zh|en|auto>           Language. Default: zh. Or ALKAID_LANG.
  --host <ssh-host>             SSH target, e.g. user@host or a ~/.ssh/config alias.
                                Can also use SUNSHINE_HOST.
  --agent <path.ps1>            Local Windows agent script to upload.
                                Default: scripts/sunshine-host-windows.ps1
  --remote-agent <win-path>     Remote agent path.
                                Default: C:\ProgramData\SunshineDeployTools\sunshine-host-windows.ps1
  --install-dir <win-path>      Sunshine install dir. Default: C:\Program Files\Sunshine
  --stage-dir <win-path>        Remote staging dir. Default: C:\ProgramData\SunshineDeployTools\stage
  --task-name <name>            User-mode scheduled task name. Default: SunshineUserMode
  --exe <local-Sunshine.exe>    Artifact for deploy-user.
  --assets <local-assets-dir>   Assets directory for deploy-assets.
  --start-args <args>           Extra Sunshine command-line args.
  --capture-backend <name>      Capture backend enforced by start-user. Default: wgc. Use '' to skip.
  --tail <n>                    Log/event tail count. Default: 220
  --ssh-option <option>         Extra ssh/scp option. Repeatable, e.g. --ssh-option=-p --ssh-option=2222

Examples:
  SUNSHINE_HOST=win-host ./scripts/sunshine-remote-macos.sh status
  ./scripts/sunshine-remote-macos.sh --host user@192.168.1.50 --exe ./build/sunshine.exe deploy-user
  ./scripts/sunshine-remote-macos.sh --host win-host --assets ./src_assets/windows/assets deploy-assets
  ./scripts/sunshine-remote-macos.sh --host win-host interactive

The script intentionally has no lab-specific host nicknames. All targets are
configured through --host or environment variables.
USAGE
}

fail() {
  echo "$(tr err_prefix): $*" >&2
  exit 1
}

win_to_scp_path() {
  local p="$1"
  p="${p//\\//}"
  printf '%s' "$p"
}

ps_literal() {
  local s="$1"
  s="${s//\'/\'\'}"
  printf "'%s'" "$s"
}

need_host() {
  [[ -n "$SUNSHINE_HOST" ]] || fail "$(tr missing_host)"
}

ssh_remote() {
  need_host
  ssh "${SSH_OPTS[@]}" "$SUNSHINE_HOST" "$@"
}

scp_to_remote() {
  need_host
  local src="$1"
  local dst_win="$2"
  local dst_scp
  dst_scp="$(win_to_scp_path "$dst_win")"
  scp "${SSH_OPTS[@]}" -r "$src" "${SUNSHINE_HOST}:${dst_scp}"
}

encoded_ps() {
  iconv -f UTF-8 -t UTF-16LE | base64 | command tr -d '\n'
}

remote_ps() {
  local script="$1"
  local encoded
  encoded="$(printf '%s' "$script" | encoded_ps)"
  ssh_remote "powershell -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -EncodedCommand $encoded"
}

ensure_agent() {
  [[ -f "$WINDOWS_AGENT" ]] || fail "$(tr agent_missing): $WINDOWS_AGENT"
  local remote_dir="${REMOTE_AGENT%\\*}"
  local upload_agent="$WINDOWS_AGENT"
  local tmp_agent=""
  remote_ps "New-Item -ItemType Directory -Force -Path $(ps_literal "$remote_dir") | Out-Null"
  # Windows PowerShell 5.1 treats UTF-8 scripts without BOM as the local ANSI
  # code page.  The helper contains localized strings, so upload a BOM-prefixed
  # copy to keep parsing stable on both English and Chinese hosts.
  if [[ "$(head -c 3 "$WINDOWS_AGENT" | od -An -tx1 | awk '{$1=$1; print}')" != "ef bb bf" ]]; then
    tmp_agent="$(mktemp -t sunshine-host-windows.XXXXXX.ps1)"
    printf '\xEF\xBB\xBF' > "$tmp_agent"
    cat "$WINDOWS_AGENT" >> "$tmp_agent"
    upload_agent="$tmp_agent"
  fi
  scp_to_remote "$upload_agent" "$REMOTE_AGENT"
  if [[ -n "$tmp_agent" ]]; then
    rm -f "$tmp_agent"
  fi
}

run_agent() {
  local action="$1"
  local extra="$2"
  ensure_agent
  local ps
  ps="& $(ps_literal "$REMOTE_AGENT") -Action $(ps_literal "$action") -InstallDir $(ps_literal "$INSTALL_DIR") -StageDir $(ps_literal "$STAGE_DIR") -TaskName $(ps_literal "$TASK_NAME") -StartArgs $(ps_literal "$START_ARGS") -CaptureBackend $(ps_literal "$CAPTURE_BACKEND") -Tail $TAIL $extra"
  remote_ps "$ps"
}

upload_exe_and_deploy() {
  [[ -n "$LOCAL_EXE" ]] || fail "$(tr exe_required)"
  [[ -f "$LOCAL_EXE" ]] || fail "$(tr exe_missing): $LOCAL_EXE"
  ensure_agent
  local remote_pkg="${STAGE_DIR}\\Sunshine.exe"
  remote_ps "New-Item -ItemType Directory -Force -Path $(ps_literal "$STAGE_DIR") | Out-Null"
  echo "$(tr uploading) $LOCAL_EXE -> ${SUNSHINE_HOST}:$(win_to_scp_path "$remote_pkg")"
  scp_to_remote "$LOCAL_EXE" "$remote_pkg"
  run_agent "deploy-user" "-PackageExe $(ps_literal "$remote_pkg")"
}

upload_assets_and_deploy() {
  [[ -n "$ASSETS_DIR" ]] || fail "$(tr assets_required)"
  [[ -d "$ASSETS_DIR" ]] || fail "$(tr assets_missing): $ASSETS_DIR"
  ensure_agent
  local tmp_zip remote_zip parent base
  tmp_zip="$(mktemp -t sunshine-assets.XXXXXX.zip)"
  parent="$(dirname "$ASSETS_DIR")"
  base="$(basename "$ASSETS_DIR")"
  (cd "$parent" && zip -qr "$tmp_zip" "$base")
  remote_ps "New-Item -ItemType Directory -Force -Path $(ps_literal "$STAGE_DIR") | Out-Null"
  remote_zip="${STAGE_DIR}\\sunshine-assets.zip"
  echo "$(tr uploading) assets zip -> ${SUNSHINE_HOST}:$(win_to_scp_path "$remote_zip")"
  scp_to_remote "$tmp_zip" "$remote_zip"
  rm -f "$tmp_zip"
  run_agent "deploy-assets" "-AssetsPath $(ps_literal "$remote_zip")"
}

interactive_menu() {
  while true; do
    echo
    if [[ "$(detect_lang)" == "zh" ]]; then echo "Sunshine 远程助手"; else echo "Sunshine remote helper"; fi
    echo "  1) status"
    echo "  2) logs"
    echo "  3) events"
    echo "  4) start-user"
    echo "  5) stop"
    echo "  6) deploy-user"
    echo "  7) deploy-assets"
    echo "  q) quit"
    read -r -p "$(tr select_action): " choice
    case "$choice" in
      1) run_agent status "" ;;
      2) run_agent logs "" ;;
      3) run_agent events "" ;;
      4) run_agent start-user "" ;;
      5) run_agent stop "" ;;
      6)
        if [[ -z "$LOCAL_EXE" ]]; then read -r -p "$(tr local_exe_path): " LOCAL_EXE; fi
        upload_exe_and_deploy
        ;;
      7)
        if [[ -z "$ASSETS_DIR" ]]; then read -r -p "$(tr local_assets_dir): " ASSETS_DIR; fi
        upload_assets_and_deploy
        ;;
      q|Q) return 0 ;;
      *) echo "$(tr unknown_choice)" ;;
    esac
  done
}

COMMAND=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --lang) LANG_CHOICE="${2:-zh}"; shift 2 ;;
    --host) SUNSHINE_HOST="${2:-}"; shift 2 ;;
    --agent) WINDOWS_AGENT="${2:-}"; shift 2 ;;
    --remote-agent) REMOTE_AGENT="${2:-}"; shift 2 ;;
    --install-dir) INSTALL_DIR="${2:-}"; shift 2 ;;
    --stage-dir) STAGE_DIR="${2:-}"; shift 2 ;;
    --task-name) TASK_NAME="${2:-}"; shift 2 ;;
    --exe) LOCAL_EXE="${2:-}"; shift 2 ;;
    --assets) ASSETS_DIR="${2:-}"; shift 2 ;;
    --start-args) START_ARGS="${2:-}"; shift 2 ;;
    --capture-backend) CAPTURE_BACKEND="${2:-}"; shift 2 ;;
    --tail) TAIL="${2:-}"; shift 2 ;;
    --ssh-option) SSH_OPTS+=("${2:-}"); shift 2 ;;
    --ssh-option=*) SSH_OPTS+=("${1#*=}"); shift ;;
    -h|--help|help) usage; exit 0 ;;
    interactive|install-agent|status|logs|events|start-user|stop|deploy-user|deploy-assets|help)
      COMMAND="$1"; shift ;;
    *) fail "unknown argument: $1" ;;
  esac
done

COMMAND="${COMMAND:-help}"
case "$COMMAND" in
  help) usage ;;
  interactive) need_host; interactive_menu ;;
  install-agent) ensure_agent ;;
  status|logs|events|start-user|stop) run_agent "$COMMAND" "" ;;
  deploy-user) upload_exe_and_deploy ;;
  deploy-assets) upload_assets_and_deploy ;;
  *) usage; exit 2 ;;
esac
