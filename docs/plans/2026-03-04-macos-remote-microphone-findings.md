# macOS 远程麦克风实现总结

## 架构概述

远程麦克风功能将 Moonlight 客户端的麦克风音频传输到 macOS 主机，通过 BlackHole 虚拟音频设备实现系统级麦克风模拟。

```
Moonlight 客户端 → [OPUS/UDP] → Sunshine → [OPUS 解码] → [Ring Buffer] → [AudioUnit] → BlackHole → 系统麦克风输入
```

## 核心组件

### 文件结构

| 文件 | 职责 |
|------|------|
| `src/platform/macos/microphone.mm` | 远程麦克风初始化、OPUS 解码、AudioUnit 渲染 |
| `src/audio.cpp` | 独立的 `mic_control` 实例管理，线程安全的 write/init/release |
| `src/stream.cpp` | UDP 接收、RTP 解包、加密解密、调用 `audio::write_mic_data()` |
| `src/platform/macos/av_audio.h/.m` | 音频捕获（主机音频→客户端方向） |

### 数据流

1. **接收**: `stream.cpp` 的 `micRecvThread` 通过 UDP 接收 RTP 包
2. **解密**: 如果启用加密，使用 AES-CBC 解密音频数据
3. **解码**: `write_mic_data()` 使用 OPUS 解码器将压缩数据解码为 48kHz 单声道 PCM
4. **格式转换**: 单声道 int16 → 立体声 float32 交错格式 `[L,R,L,R,...]`
5. **缓冲**: 写入 TPCircularBuffer（无锁环形缓冲区，1秒容量）
6. **渲染**: AudioUnit 渲染回调从环形缓冲区读取，去交错为 L/R 分离通道
7. **输出**: 通过 HAL Output AudioUnit 输出到 BlackHole 虚拟设备

## 关键设计决策

### 为什么使用交错立体声环形缓冲区 + 去交错渲染回调

**正确的模式（当前实现）：**
```
write_mic_data: mono int16 → stereo float32 interleaved → Ring Buffer
render callback: Ring Buffer → de-interleave to L/R buffers
```

**错误的模式（曾尝试但失败）：**
```
write_mic_data: mono int16 → mono float32 → Ring Buffer
render callback: Ring Buffer → copy mono to each L/R buffer
```

原因：HAL Output AudioUnit 使用非交错格式（`kAudioFormatFlagIsNonInterleaved`），渲染回调的 `AudioBufferList` 包含 2 个独立的缓冲区（左/右声道）。环形缓冲区存储交错数据，渲染回调负责去交错，这样可以：
- 保持环形缓冲区的简单性（单一连续数据流）
- 在渲染回调中精确控制每个声道的数据

### 为什么不使用 AVAudioEngine

早期实现使用 `AVAudioEngine` + `AVAudioPlayerNode`，但存在问题：
- `AVAudioEngine` 的输出设备切换不够可靠
- `scheduleBuffer` 的延迟不可控
- AudioUnit 直接操作提供更低的延迟和更精确的控制

### 为什么使用独立的 mic_control

`src/audio.cpp` 中的 `mic_control` 是独立于音频捕获的 `audio_control_t` 实例：
- 远程麦克风的生命周期与音频捕获不同
- 避免与音频捕获的 `set_sink` 操作冲突
- 使用 `std::mutex` 保护，支持多线程安全访问

## 配置

```ini
[audio]
virtual_sink = BlackHole 16ch  # 远程麦克风输出设备（默认 BlackHole 2ch）
```

用户需要：
1. 安装 BlackHole: `brew install blackhole-2ch`
2. 在系统设置 → 声音 → 输入中选择 BlackHole 作为麦克风输入
3. 在 Moonlight 客户端设置中启用麦克风

## 已知限制

1. **`set_sink` 未实现**: 音频捕获路由需要用户手动配置（通过 Audio MIDI Setup 创建多输出设备）
2. **无增益控制**: 当前直接使用原始 PCM 值（`pcm_mono[i] / 32768.0f`），无增益提升
3. **无采样率验证**: 假设 BlackHole 设备支持 48kHz，未验证实际采样率

## 故障排除

| 症状 | 可能原因 | 解决方案 |
|------|----------|----------|
| 麦克风无声音 | BlackHole 未安装或未选为系统输入 | 安装 BlackHole，在系统设置中选择 |
| 麦克风有杂音/爆音 | 环形缓冲区格式不匹配 | 确保使用交错立体声格式 |
| 麦克风延迟高 | 环形缓冲区过大 | 检查缓冲区大小配置 |
| 日志显示 "Ring buffer full" | 渲染回调未运行 | 检查 AudioUnit 是否正确启动 |
