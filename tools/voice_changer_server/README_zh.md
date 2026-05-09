# Voice Changer Sidecar 中文使用指南

Sunshine 麦克风变声扩展的旁路服务（sidecar）。把麦克风音频通过 UDP 转给本服务做实时变声推理后回传，对 Sunshine 主进程零侵入。

> 看英文版？见 [`README.md`](./README.md)。
> 协议规范：`src/voice_changer/voice_changer_ipc.h`。

---

## 0. 快速判断我能不能用

| 条件 | 要求 |
|---|---|
| Sunshine 版本 | 包含 PR-A/PR-B 提交（PR `feat/voice-changer-scaffold`） |
| Python | 3.10 及以上 |
| 仅试通信（identity 透传） | **零依赖**，5 秒能跑 |
| 跑 RVC 真变声 | NVIDIA 显卡 6GB+ 显存 / 或 CPU（很慢） |
| 跑 RVC 真变声 | 约 5GB 磁盘（PyTorch + 预训权重 + 模型） |

---

## 1. 五分钟跑通透传（确认链路）

```bash
cd Sunshine/tools/voice_changer_server
python voice_changer_server.py
```

打开 Sunshine Web UI → AudioVideo 标签 → 勾选「Voice Changer Enabled」→ 后端选 `External service IPC` → 保存。

开始一次串流，对着麦克风说话，**对端听到的应该是你原声**。说明协议接通。

---

## 2. 装 RVC 依赖

```bash
# 进入 sidecar 目录
cd Sunshine/tools/voice_changer_server

# 强烈建议用虚拟环境
python -m venv .venv
.venv\Scripts\activate          # Windows
# source .venv/bin/activate     # Linux/Mac

# CUDA 12.1 用户先装 torch（CPU 用户可跳过这步）
pip install torch --index-url https://download.pytorch.org/whl/cu121

# 装 RVC 全套依赖
pip install -r requirements-rvc.txt
```

国内网络慢可设：

```bash
set HF_ENDPOINT=https://hf-mirror.com   # cmd
$env:HF_ENDPOINT="https://hf-mirror.com" # PowerShell
export HF_ENDPOINT=https://hf-mirror.com # bash
```

---

## 3. 下载预训权重（必须）

任何 RVC 模型都依赖两个共享权重：HuBERT（内容编码器）+ RMVPE（音高检测器），合计约 350MB。

```bash
python download_models.py --pretrained
```

下载到 `models/pretrained/`：
- `hubert_base.pt`
- `rmvpe.pt`

---

## 4. 准备角色模型（.pth + .index）

RVC 模型由两个文件组成：

| 文件 | 必需 | 作用 |
|---|---|---|
| `角色名.pth` | ✅ 是 | 模型本体（生成器权重） |
| `角色名.index` | ⭕ 强烈建议 | 检索索引（提升音色稳定度） |

### 来源

| 渠道 | 说明 |
|---|---|
| HuggingFace | `huggingface.co` 搜 RVC 关键字 |
| AI Hub Discord | 海外社区集中地（需翻墙） |
| B 站云盘 | 国内分享区，搜「RVC 模型 角色名」 |
| **自己训练** | 用 [Applio](https://github.com/IAHispano/Applio) 训：10 分钟干净人声数据起步 |

### 下载到位

把下载到的 `.pth` 和 `.index` 放进 `models/voices/`。

或用工具下载：

```bash
python download_models.py --url https://huggingface.co/<某仓库>/resolve/main/某模型.pth
python download_models.py --url https://huggingface.co/<某仓库>/resolve/main/某模型.index
```

---

## 5. 启动 RVC sidecar

```bash
python voice_changer_server.py --backend rvc --warmup ^
    --opt model_path=models/voices/character.pth ^
    --opt index_path=models/voices/character.index ^
    --opt pitch_shift=12 ^
    --opt index_rate=0.75
```

| 参数 | 含义 | 推荐 |
|---|---|---|
| `model_path` | .pth 路径 | 必填 |
| `index_path` | .index 路径 | 强烈建议填 |
| `pitch_shift` | 音高升降（半音）。男变女 +12，女变男 -12 | -24 ~ 24 |
| `index_rate` | 索引检索权重 | 0.5 ~ 0.85 |
| `f0_method` | 音高算法 | `rmvpe`（默认/最稳） |
| `protect` | 清辅音保护，越大越接近原声 | 0.33 |
| `chunk_ms` | 流式块长度（毫秒） | 320 |
| `overlap_ms` | 块间交叉淡入淡出 | 80 |

启动后会看到：

```
INFO loading backend 'rvc' opts={...}
INFO RVC model loaded: .../character.pth
INFO RVC warmup ok
INFO voice_changer_server listening on udp://127.0.0.1:9876 backend=rvc
```

回到 Sunshine UI 串流测试。**对端应该听到角色的声音**。

---

## 6. 排错

### 启动报 `rvc package not installed`

`pip install -r requirements-rvc.txt` 没装，或装在了别的 Python 里。

### 启动报 `model_path required`

忘了 `--opt model_path=...`。

### 串流时声音是原声没变

1. 确认 Sunshine UI 里 backend 选了 `External service IPC`，不是 passthrough
2. 确认 host=`127.0.0.1`、port=`9876` 与 sidecar 一致
3. 看 sidecar 日志有没有 `processed N frames`；如果没动，是 IPC 没通
4. 看 sidecar 日志有没有 `backend.process failed (fallback ...)`；如果有，是 RVC 推理出错，已自动降级到透传

### 声音断断续续 / 有金属感

- `chunk_ms` 加大到 480 或 640（牺牲延迟换质量）
- `f0_method` 试试 `crepe`（精度更高但更慢）
- 检查 .index 文件是否匹配 .pth（错配会出现锯齿声）

### CUDA OOM

- 关掉显存大户（游戏除外）
- 用 `pip install onnxruntime-gpu` 改走 ONNX 路径（需要 ONNX 模型文件）

---

## 7. 性能基准（参考）

测试机：RTX 3070 Laptop / Ryzen 7 5800H / 32GB RAM

| 配置 | 单 chunk 推理时间 | 端到端额外延迟 |
|---|---|---|
| RVC + RMVPE + chunk=320ms | ~80 ms | ~240 ms |
| RVC + RMVPE + chunk=480ms | ~120 ms | ~360 ms |
| Identity（passthrough） | < 0.1 ms | < 5 ms |

加上 Sunshine 串流本身 30-80 ms，**总语音延迟 280-440 ms**。这在远程游戏开黑场景里可以接受，但比本地 Discord 变声明显高。

如对延迟极度敏感：考虑后续 PR 的 SEED-VC backend 或本地（非串流）方案。

---

## 8. 架构

```
Sunshine (主进程)
    └── mic_write.cpp::write_data
        ├── opus_decode
        ├── voice_changer::process()       ← 钩子
        │   └── UDP sendto 127.0.0.1:9876
        ├── (block on recvfrom, 15ms 超时)
        ├── 超时/错误 → 透传原音
        └── WASAPI render to virtual mic

sidecar 进程（本目录）
    └── voice_changer_server.py
        └── backends/rvc.py (或 identity)
            └── OverlapBuffer
                ├── 累积 chunk_ms 输入
                ├── 调 RVC 推理
                └── 交叉淡入淡出回 20ms 帧
```

每 20ms 一次 IPC，本地回环 < 1ms 开销。Sunshine 侧任何环节失败都会无声降级到原音透传，**不会阻塞通话**。

---

## 9. 拓展自己的 backend

新加一个变声方案？看 `backends/__init__.py` 的注释，三步：

1. 写 `backends/<your_name>.py`，实现 `Backend` 协议（`name` + `process` + 可选 `reset`/`warmup`）
2. 在 `REGISTRY` 加一行映射
3. 启动时 `--backend <your_name> --opt key=value ...`

参考：`identity.py` 是最小骨架，`rvc.py` 是带流式 + 懒加载的完整范本。
