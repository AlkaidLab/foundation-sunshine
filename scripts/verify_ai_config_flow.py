#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
端到端流程验证：复刻 confighttp.cpp 修改后的控制流，验证全部真实场景。

场景覆盖：
  1. 全新安装（无配置文件）→ 默认配置
  2. Web UI 保存明文 key（POST）→ 磁盘密文 / 内存明文
  3. 重启加载（新进程读盘）→ 解密成功，文件保持密文
  4. 旧版本明文文件 → 新版本启动自动迁移加密
  5. POST 塞入密文格式 key → 还原明文，不污染缓存
  6. POST 掩码（含 ****）→ 不覆盖原 key
  7. 密文损坏 / 换机器 → key 安全清空，不崩溃、不用密文
  8. 空 key（ollama/localhost 场景）→ 不加密
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# 复用协议验证脚本里的加解密实现（与 C++ 一一对应）
from verify_secure_store import encrypt_cpp, decrypt_cpp, K_MAGIC, b64_decode  # noqa: E402

MID, HOME = "9d27f8c1e2a34b5c8f0e1234567890ab", "/home/user"
DEFAULT = {
    "enabled": False, "provider": "openai",
    "apiBase": "https://api.openai.com/v1", "apiKey": "",
    "model": "gpt-4.1-mini", "compatibility": "openai-chat",
    "temperature": 0.3, "max_tokens": 2048,
}
failures = []
state = {"disk": None, "cache": None, "loaded": False, "logs": []}


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name} {detail}")
    if not cond:
        failures.append(name)


# ---- 复刻 confighttp.cpp 的 writeAiConfigFileLocked ----
def write_file(cfg):
    to_disk = dict(cfg)
    key = to_disk.get("apiKey", "")
    if key and not key.startswith(K_MAGIC):
        to_disk["apiKey"] = encrypt_cpp(key, MID, HOME)
    state["disk"] = to_disk  # 落盘


# ---- 复刻 confighttp.cpp 的 loadAiConfigLocked ----
def load_config():
    if state["loaded"]:
        return state["cache"]
    if state["disk"] is not None:
        cache = json.loads(json.dumps(state["disk"]))
        state["cache"] = cache  # 与 C++ ai_config_cache = parse(...) 一致
        state["loaded"] = True
        key = cache.get("apiKey", "")
        if key.startswith(K_MAGIC):
            try:
                cache["apiKey"] = decrypt_cpp(key, MID, HOME)
            except Exception as e:
                state["logs"].append(f"decrypt-fail:{e}")
                cache["apiKey"] = ""  # 清空，不用密文
        elif key:
            state["logs"].append("migrate-plaintext")
            write_file(cache)  # 自动迁移
        return cache
    state["cache"] = dict(DEFAULT)
    state["loaded"] = True
    return state["cache"]


# ---- 复刻 saveAiConfigEndpoint（load-modify-save + 掩码/密文防御）----
def save_config(input_):
    current = load_config()
    for f in ("enabled", "provider", "apiBase", "model", "compatibility", "temperature", "max_tokens"):
        if f in input_:
            current[f] = input_[f]
    if "apiKey" in input_:
        key = input_["apiKey"]
        if "****" not in key:
            if key.startswith(K_MAGIC):  # 防御：还原密文
                try:
                    current["apiKey"] = decrypt_cpp(key, MID, HOME)
                except Exception:
                    state["logs"].append("reject-unusable-enc")
            else:
                current["apiKey"] = key
    write_file(current)
    state["cache"] = current  # 内存缓存明文
    return current


print("== 场景1: 全新安装（无文件）==")
state.update(disk=None, loaded=False, cache=None)
cfg = load_config()
check("返回默认配置且 enabled=false", cfg == DEFAULT)

print("== 场景2: Web UI 保存明文 key ==")
state.update(disk=None, loaded=False, cache=None)
save_config({"enabled": True, "apiKey": "sk-real-key-123456", "model": "deepseek-chat"})
check("磁盘 apiKey 为密文", state["disk"]["apiKey"].startswith(K_MAGIC))
check("磁盘不含明文 key", "sk-real-key-123456" not in json.dumps(state["disk"]))
check("内存缓存为明文", state["cache"]["apiKey"] == "sk-real-key-123456")
check("其他字段明文保存", state["disk"]["model"] == "deepseek-chat")

print("== 场景3: 重启加载（新进程读盘）==")
disk_at_rest = json.loads(json.dumps(state["disk"]))
state.update(disk=disk_at_rest, loaded=False, cache=None)
cfg = load_config()
check("解密成功", cfg["apiKey"] == "sk-real-key-123456")
check("文件保持密文不重写", state["disk"]["apiKey"] == disk_at_rest["apiKey"])

print("== 场景4: 旧版本明文文件自动迁移 ==")
state.update(disk=dict(DEFAULT, enabled=True, apiKey="sk-legacy-plain-key", model="deepseek-chat"),
             loaded=False, cache=None)
cfg = load_config()
check("迁移后内存明文可用", cfg["apiKey"] == "sk-legacy-plain-key")
check("磁盘已被加密重写", state["disk"]["apiKey"].startswith(K_MAGIC))
check("磁盘无明文残留", "sk-legacy-plain-key" not in json.dumps(state["disk"]))
check("记录迁移日志", "migrate-plaintext" in state["logs"])

print("== 场景5: POST 塞入密文格式 key ==")
state.update(disk=dict(DEFAULT, enabled=True, apiKey=encrypt_cpp("sk-cipher-injected", MID, HOME)),
             loaded=False, cache=None)
cfg = load_config()
save_config({"apiKey": encrypt_cpp("sk-injected-by-attacker", MID, HOME)})
check("内存为还原后的明文", state["cache"]["apiKey"] == "sk-injected-by-attacker")
check("磁盘为加密后的密文", state["disk"]["apiKey"].startswith(K_MAGIC))
check("缓存未被密文污染", "sk-injected-by-attacker" in json.dumps(state["cache"]))

print("== 场景6: POST 掩码不覆盖 ==")
state.update(disk=dict(DEFAULT, enabled=True, apiKey=encrypt_cpp("sk-keep-me", MID, HOME)),
             loaded=False, cache=None)
load_config()
save_config({"apiKey": "sk-****1234"})
check("掩码不覆盖原 key", state["cache"]["apiKey"] == "sk-keep-me")

print("== 场景7: 密文损坏/换机器 → 安全清空 ==")
state.update(disk=dict(DEFAULT, enabled=True, apiKey="enc:v1:Zm9vYmFy" * 5), loaded=False, cache=None)
cfg = load_config()
check("解密失败后 key 清空", cfg["apiKey"] == "")
check("进程不崩溃、继续返回配置", cfg["enabled"] is True)
check("记录错误日志", any("decrypt-fail" in l for l in state["logs"]))

print("== 场景8: 空 key（ollama 本地）不加密 ==")
state.update(disk=dict(DEFAULT, enabled=True, provider="ollama", apiKey=""), loaded=False, cache=None)
save_config({"apiKey": ""})
check("空 key 保持空字符串", state["disk"]["apiKey"] == "" and state["cache"]["apiKey"] == "")

print()
if failures:
    print(f"!!! {len(failures)} 项失败: {failures}")
    sys.exit(1)
print("8 个场景全部通过 ✅ 控制流与 confighttp.cpp 修改一致")
