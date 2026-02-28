# macOS 远程麦克风系统级实现设计

**日期：** 2026-02-28
**状态：** 设计阶段
**目标：** 实现系统级远程麦克风功能，支持所有应用场景

## 1. 背景和问题

### 当前状态
- ✅ BlackHole 2ch 已安装
- ✅ Sunshine 配置了 `virtual_sink = BlackHole 2ch`
- ❌ 当前实现使用 AVAudioEngine.outputNode，输出到系统默认设备（扬声器）
- ❌ 当 BlackHole 设为系统输入时，产生音频回路 → 杂音/回声

### 用户需求
- 全场景支持：AirType 语音输入、Zoom/Teams 视频会议、游戏语音聊天、系统级语音输入（Siri、听写）
- 不需要听到 Mac 声音
- 手动控制麦克风开关（Moonlight 按钮）
- 无杂音、低延迟（< 100ms）

### 根本问题
当前代码使用 AVAudioEngine 输出到系统默认设备，无法精确控制输出目标。尝试通过 `AudioUnitSetProperty` 设置输出设备不可靠。

## 2. 技术方案

### 方案选择：AudioQueue API

**放弃 AVAudioEngine，改用 AudioQueue**

**原因：**
- AudioQueue 可以直接指定输出设备 ID
- 更底层，性能更好
- 不会输出到扬声器，避免音频回路
- 更精确的设备控制

### 架构设计

**音频流向：**
```
Moonlight 客户端麦克风
  → 网络传输（加密 Opus）
  → Sunshine 接收解密解码（src/stream.cpp）
  → write_mic_data() 格式转换（src/platform/macos/microphone.mm）
  → AudioQueue 输出队列
  → BlackHole 虚拟设备（指定设备 ID）
  → macOS 系统音频输入
  → 所有应用（AirType、Zoom、游戏、Siri）
```

**关键优势：**
- 直接写入 BlackHole，不经过扬声器
- 系统输入设为 BlackHole，所有应用自动获得远程麦克风
- 无音频回路，无杂音

## 3. 核心组件设计

### 3.1 数据结构

```objc
struct macos_audio_control_t {
  // 现有字段...

  // AudioQueue 相关（替换 AVAudioEngine）
  AudioQueueRef audio_queue;           // 输出队列
  AudioStreamBasicDescription format;  // 音频格式描述
  AudioQueueBufferRef buffers[3];      // 缓冲区池
  AudioDeviceID blackhole_device_id;   // BlackHole 设备 ID
};
```

### 3.2 初始化流程

**init_mic_redirect_device() 实现：**

```
1. 查找 BlackHole 设备 ID
   - 遍历所有音频设备（AudioObjectGetPropertyData）
   - 匹配设备名称 "BlackHole 2ch"
   - 保存设备 ID

2. 配置音频格式
   - 格式：Linear PCM
   - 采样率：48000 Hz
   - 声道：2（立体声）
   - 位深：32-bit float
   - 交错：非交错（planar）

3. 创建 AudioQueue 输出队列
   - AudioQueueNewOutput()
   - 设置输出设备：AudioQueueSetProperty(kAudioQueueProperty_CurrentDevice)
   - 绑定到 BlackHole 设备 ID

4. 分配缓冲区
   - 创建 3 个 AudioQueueBuffer
   - 每个 buffer 大小：48000 * 2 * 4 / 10 = 38400 字节（100ms）

5. 启动队列
   - AudioQueueStart()
   - 记录成功日志
```

### 3.3 音频数据处理

**write_mic_data() 实现：**

```
1. 接收客户端音频数据（int16 PCM, 48kHz, 2ch）

2. 格式转换
   - int16 → float32
   - 范围：[-32768, 32767] → [-1.0, 1.0]
   - 公式：float_value = int16_value / 32768.0f

3. 填充 AudioQueueBuffer
   - 获取空闲 buffer（从池中）
   - 复制转换后的音频数据
   - 设置 buffer 大小

4. 入队
   - AudioQueueEnqueueBuffer()
   - 回调函数：buffer 播放完成后释放

5. 错误处理
   - Buffer 分配失败 → 跳过帧，记录警告
   - 队列停止 → 尝试重启
```

### 3.4 清理流程

**release_mic_redirect_device() 实现：**

```
1. 停止队列
   - AudioQueueStop(immediate: true)

2. 释放缓冲区
   - 遍历所有 buffers
   - AudioQueueFreeBuffer()

3. 销毁队列
   - AudioQueueDispose()

4. 重置状态
   - audio_queue = nullptr
   - blackhole_device_id = kAudioDeviceUnknown
```

## 4. 数据流设计

### 4.1 初始化时序

```
T0: Moonlight 连接 → RTSP SETUP mic/0/0
T1: Sunshine 启动麦克风 socket（端口 48001）
T2: 客户端开启麦克风按钮
T3: 第一个音频包到达
T4: 调用 init_mic_redirect_device()
    ├─ 查找 BlackHole 设备 ID (10ms)
    ├─ 创建 AudioQueue (5ms)
    ├─ 分配 buffers (2ms)
    └─ 启动队列 (3ms)
T5: 记录日志："Virtual microphone initialized successfully"
T6: 开始接收音频数据
```

### 4.2 音频数据流

```
客户端 → 网络 → Sunshine
  ↓
解密 + 解码（Opus → int16 PCM）
  ↓
write_mic_data()
  ├─ 转换 int16 → float32 (< 1ms)
  ├─ 获取空闲 buffer
  ├─ 填充数据
  └─ AudioQueueEnqueueBuffer()
  ↓
AudioQueue 输出到 BlackHole (< 5ms)
  ↓
macOS 系统从 BlackHole 读取
  ↓
应用程序使用远程麦克风
```

**延迟分析：**
- 网络传输：20-50ms（取决于网络）
- 解码：< 5ms
- 格式转换：< 1ms
- AudioQueue 输出：< 5ms
- **总延迟：< 100ms**

### 4.3 清理时序

```
T0: 客户端断开或关闭麦克风
T1: ctx.mic_socket_enabled = false
T2: 麦克风线程退出循环
T3: 调用 release_mic_redirect_device()
    ├─ AudioQueueStop() (10ms)
    ├─ 释放 buffers (2ms)
    └─ AudioQueueDispose() (5ms)
T4: 记录日志："Virtual microphone released"
```

## 5. 错误处理

### 5.1 初始化错误

| 错误场景 | 处理方式 | 用户提示 |
|---------|---------|---------|
| BlackHole 未安装 | 返回 -1，禁用功能 | "BlackHole not found. Install: brew install blackhole-2ch" |
| 设备 ID 查找失败 | 返回 -1 | "Failed to find BlackHole device" |
| AudioQueue 创建失败 | 返回 -1 | "Failed to create audio queue: [error]" |
| 设备绑定失败 | 返回 -1 | "Failed to set output device to BlackHole" |

### 5.2 运行时错误

| 错误场景 | 处理方式 | 恢复策略 |
|---------|---------|---------|
| Buffer 分配失败 | 跳过当前帧 | 记录警告，继续处理下一帧 |
| 队列停止 | 尝试重启 | 调用 AudioQueueStart()，失败则禁用 |
| 格式不匹配 | 重新初始化 | 调用 release + init |
| 内存不足 | 清理资源 | 释放所有 buffers，返回错误 |

### 5.3 日志策略

**初始化日志：**
```
[Info] Initializing virtual microphone with device: BlackHole 2ch
[Debug] Found BlackHole device ID: 123
[Debug] Created AudioQueue with format: 48000Hz, 2ch, float32
[Info] Successfully set output device to BlackHole
[Info] Virtual microphone initialized successfully
```

**运行时日志：**
```
[Debug] Received mic data: 960 frames (20ms)
[Debug] Converted int16 to float32: 960 frames
[Debug] Enqueued buffer: 3840 bytes
```

**错误日志：**
```
[Error] Failed to find BlackHole audio device: BlackHole 2ch
[Error] AudioQueue creation failed: OSStatus -50
[Warning] Buffer allocation failed, skipping frame
```

## 6. 性能优化

### 6.1 内存管理

**Buffer 池策略：**
- 预分配 3 个 buffers（100ms 每个）
- 循环使用，避免频繁分配
- 总内存：3 × 38400 = 115KB

**音频数据复制：**
- 使用 memcpy 批量复制
- 避免逐样本转换
- SIMD 优化（如果需要）

### 6.2 CPU 优化

**格式转换优化：**
```c
// 批量转换 int16 → float32
for (size_t i = 0; i < frameCount * 2; i++) {
  float_buffer[i] = int16_buffer[i] / 32768.0f;
}
```

**预期 CPU 使用率：**
- 空闲：< 0.1%
- 音频传输：< 5%

### 6.3 延迟优化

**Buffer 大小调整：**
- 当前：100ms buffer
- 可选：50ms buffer（更低延迟，更高 CPU）
- 配置项：`mic_buffer_ms`（默认 100）

## 7. 测试策略

### 7.1 单元测试

**测试文件：** `tests/unit/test_macos_audioqueue_microphone.cpp`

**测试用例：**
1. `test_find_blackhole_device()` - 设备查找
2. `test_audio_format_conversion()` - int16 → float32 转换
3. `test_audioqueue_creation()` - AudioQueue 创建
4. `test_buffer_management()` - Buffer 分配和释放
5. `test_resource_cleanup()` - 资源清理验证

### 7.2 集成测试

**测试场景 1：基础功能**
```
步骤：
1. 启动 Sunshine
2. Moonlight 连接并开启麦克风
3. 验证日志包含：
   - "Initializing virtual microphone"
   - "Successfully set output device to BlackHole"
   - "Virtual microphone initialized successfully"
4. 对着平板说话
5. 打开"音频 MIDI 设置"查看 BlackHole 输入电平
6. 验证有音频信号波形

预期结果：
- ✅ 无错误日志
- ✅ BlackHole 输入电平有波动
- ✅ 无杂音
```

**测试场景 2：应用集成**
```
测试 A：AirType 语音输入
1. 打开 AirType
2. 对着平板说"你好世界"
3. 验证文字正确输入

测试 B：Zoom 视频会议
1. 加入 Zoom 会议
2. 对着平板说话
3. 验证其他参与者能听到

测试 C：游戏语音聊天
1. 启动支持语音的游戏
2. 对着平板说话
3. 验证队友能听到

测试 D：Siri 语音识别
1. 激活 Siri
2. 对着平板说"现在几点"
3. 验证 Siri 正确响应

预期结果：
- ✅ 所有应用都能使用远程麦克风
- ✅ 语音清晰，无杂音
- ✅ 延迟 < 100ms
```

**测试场景 3：错误处理**
```
测试 A：BlackHole 未安装
1. 卸载 BlackHole
2. 启动 Sunshine
3. 验证错误提示

测试 B：快速开关麦克风
1. 连接 Moonlight
2. 快速开关麦克风按钮 10 次
3. 验证无崩溃，无内存泄漏

测试 C：长时间运行
1. 开启麦克风
2. 持续说话 1 小时
3. 监控 CPU 和内存使用

预期结果：
- ✅ 错误提示清晰
- ✅ 无崩溃
- ✅ CPU < 5%
- ✅ 无内存泄漏
```

### 7.3 性能测试

**延迟测试：**
```
工具：Audacity 或 Logic Pro
方法：
1. 在 Mac 上播放音频
2. 通过 Moonlight 录制
3. 对比波形，测量延迟

目标：< 100ms
```

**CPU 测试：**
```
工具：Activity Monitor
方法：
1. 开启麦克风
2. 持续说话 5 分钟
3. 记录 Sunshine CPU 使用率

目标：< 5%
```

**内存测试：**
```
工具：Instruments (Leaks)
方法：
1. 开启麦克风
2. 说话 10 分钟
3. 关闭麦克风
4. 检查内存泄漏

目标：0 leaks
```

## 8. 配置和用户指南

### 8.1 配置文件

**sunshine.conf：**
```ini
# 远程麦克风配置
virtual_sink = BlackHole 2ch  # 虚拟音频设备名称（必需）

# 可选配置
# mic_buffer_ms = 100  # Buffer 大小（毫秒），默认 100
```

### 8.2 用户设置步骤

**步骤 1：安装 BlackHole**
```bash
brew install blackhole-2ch
```

**步骤 2：重启 macOS**
```
重启后 BlackHole 驱动才会加载
```

**步骤 3：配置系统音频**
```
1. 打开"系统设置" → "声音"
2. 输入：选择 "BlackHole 2ch"
3. 输出：保持 "Mac mini 扬声器"（或你想听到声音的设备）
```

**步骤 4：启动 Sunshine**
```bash
./build/sunshine
```

**步骤 5：连接 Moonlight**
```
1. 打开 Moonlight 应用
2. 连接到 Mac mini
3. 点击麦克风按钮开启
4. 开始说话
```

**步骤 6：验证功能**
```
1. 打开"音频 MIDI 设置"
2. 选择 BlackHole 2ch
3. 查看输入电平是否有波动
4. 打开任意应用测试麦克风
```

### 8.3 故障排除

| 问题 | 原因 | 解决方案 |
|-----|------|---------|
| 听不到声音 | 系统输入未设为 BlackHole | 系统设置 → 声音 → 输入 → BlackHole 2ch |
| 有杂音/回声 | 音频回路 | 检查输出设备不是 BlackHole |
| 延迟高 | 网络问题 | 检查网络质量，降低分辨率 |
| 初始化失败 | BlackHole 未安装 | brew install blackhole-2ch，重启 |
| 应用检测不到 | 权限问题 | 系统设置 → 隐私与安全 → 麦克风 |

### 8.4 卸载指南

**卸载 BlackHole：**
```bash
brew uninstall blackhole-2ch
```

**删除配置：**
```bash
# 编辑 sunshine.conf，删除或注释掉：
# virtual_sink = BlackHole 2ch
```

## 9. 实现清单

### 9.1 代码修改

**文件：** `src/platform/macos/microphone.mm`

**修改内容：**
1. 移除 AVAudioEngine 相关代码
2. 添加 AudioQueue 相关头文件
3. 实现 `find_blackhole_device_id()`
4. 重写 `init_mic_redirect_device()`
5. 重写 `write_mic_data()`
6. 重写 `release_mic_redirect_device()`
7. 添加详细日志

**文件：** `cmake/dependencies/macos.cmake`

**修改内容：**
1. 确保链接 CoreAudio 和 AudioUnit 框架（已完成）

### 9.2 文档更新

**文件：** `docs/configuration.md`
- 更新 `virtual_sink` 说明
- 添加 BlackHole 安装指南

**文件：** `docs/getting_started.md`
- 添加远程麦克风设置步骤

**文件：** `docs/troubleshooting.md`
- 添加远程麦克风故障排除

### 9.3 测试文件

**创建：** `tests/unit/test_macos_audioqueue_microphone.cpp`
- 单元测试用例

**创建：** `scripts/test_remote_microphone.sh`
- 自动化测试脚本

## 10. 验收标准

### 10.1 功能验收

- ✅ 所有应用都能使用远程麦克风（AirType、Zoom、游戏、Siri）
- ✅ 无杂音、无回声
- ✅ 音质清晰
- ✅ 手动控制开关正常

### 10.2 性能验收

- ✅ 延迟 < 100ms
- ✅ CPU 使用率 < 5%
- ✅ 无内存泄漏
- ✅ 长时间运行稳定（1 小时+）

### 10.3 用户体验验收

- ✅ 安装步骤清晰（< 5 分钟）
- ✅ 错误提示友好
- ✅ 故障排除文档完整
- ✅ 无需在每个应用中配置

## 11. 风险和缓解

### 11.1 技术风险

| 风险 | 影响 | 概率 | 缓解措施 |
|-----|------|------|---------|
| AudioQueue API 不稳定 | 高 | 低 | 充分测试，准备回退方案 |
| BlackHole 兼容性问题 | 中 | 低 | 测试多个 macOS 版本 |
| 性能不达标 | 中 | 低 | 优化 buffer 大小和格式转换 |

### 11.2 用户体验风险

| 风险 | 影响 | 概率 | 缓解措施 |
|-----|------|------|---------|
| 安装步骤复杂 | 中 | 中 | 提供详细文档和脚本 |
| 配置错误 | 高 | 中 | 自动检测和友好提示 |
| 与其他软件冲突 | 低 | 低 | 文档说明已知冲突 |

## 12. 时间线

**阶段 1：实现核心功能（2-3 小时）**
- 实现 AudioQueue 代码
- 基础测试

**阶段 2：测试和优化（1-2 小时）**
- 集成测试
- 性能优化
- Bug 修复

**阶段 3：文档和发布（1 小时）**
- 更新文档
- 创建测试脚本
- 用户验收

**总计：4-6 小时**

## 13. 后续优化

### 13.1 短期优化
- 添加音频增益控制
- 支持降噪功能
- 添加音频可视化

### 13.2 长期优化
- 支持多声道（5.1、7.1）
- 支持更多虚拟设备（Soundflower 等）
- 添加音频效果（均衡器、压缩器）

## 14. 参考资料

**Apple 官方文档：**
- [Audio Queue Services Programming Guide](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/AudioQueueProgrammingGuide/)
- [Core Audio Overview](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/)

**第三方资源：**
- [BlackHole GitHub](https://github.com/ExistentialAudio/BlackHole)
- [macOS Audio Device Management](https://stackoverflow.com/questions/tagged/core-audio+macos)

**项目相关：**
- `docs/plans/2026-02-27-macos-input-audio-optimization-design.md` - 之前的设计文档
- `src/platform/macos/microphone.mm` - 当前实现
