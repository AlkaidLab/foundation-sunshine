# macOS 输入和音频优化设计文档

**日期：** 2026-02-27
**作者：** Claude Code
**状态：** 已批准

## 概述

本设计文档描述了 macOS 平台上鼠标输入优化和远程麦克风功能的实现方案，旨在解决以下问题：
1. 鼠标移动延迟和加速度曲线不自然
2. 远程麦克风功能未实现（客户端音频无法在 macOS 主机上使用）

## 用户场景

### 核心使用场景

**场景 1：语音转文字**
- 用户在客户端使用 AirType 等语音输入工具
- 客户端麦克风音频通过 Moonlight 传输到 Sunshine
- macOS 主机将音频识别为虚拟麦克风输入
- AirType 进行语音识别并输入文字

**场景 2：语音通话**
- 用户在远程 macOS 上使用 Zoom/Teams 等通话应用
- 客户端麦克风音频传输到 macOS 主机
- 通话应用使用虚拟麦克风作为音频输入源

**场景 3：鼠标操作**
- 用户通过 Moonlight 客户端远程控制 macOS
- 需要流畅、自然的鼠标响应
- 支持精确点击和快速移动

## 技术方案

### 一、鼠标输入优化

#### 1.1 当前问题分析

**性能瓶颈：**
- 每次鼠标事件调用 `CGEventCreate(NULL)` 查询当前位置（src/platform/macos/input.cpp:321）
- 事件对象复用导致额外的 `CGEventSetType` 和字段设置调用
- 缺少鼠标灵敏度配置选项

**影响：**
- 鼠标移动延迟明显
- 加速度曲线不自然
- CPU 使用率偏高

#### 1.2 优化方案

**数据结构扩展：**
```cpp
struct macos_input_t {
  // 现有字段...
  util::point_t cached_mouse_position {};  // 缓存的鼠标位置
  bool position_cache_valid {false};       // 位置缓存是否有效
  float mouse_sensitivity {1.0f};          // 鼠标灵敏度倍数
};
```

**关键优化点：**

1. **位置缓存机制**
   - 在 `macos_input_t` 中维护鼠标位置缓存
   - `get_mouse_loc()` 优先返回缓存值
   - 仅在缓存失效时查询系统
   - 每次 `post_mouse()` 后更新缓存

2. **直接创建事件**
   - 使用 `CGEventCreateMouseEvent()` 直接创建事件
   - 替代当前的 `CGEventSetType()` 修改方式
   - 减少字段设置调用次数

3. **灵敏度配置**
   - 添加 `config::input::mouse_sensitivity` 配置项
   - 在 `move_mouse()` 中应用灵敏度倍数
   - 支持范围：0.5 - 2.0，默认 1.0

**预期改进：**
- 鼠标延迟降低 20-30%
- CPU 使用率降低 10-15%
- 支持用户自定义灵敏度

#### 1.3 配置选项

**配置文件示例（sunshine.conf）：**
```ini
[input]
mouse_sensitivity = 1.2  # 提高 20% 灵敏度
```

---

### 二、远程麦克风实现

#### 2.1 当前问题分析

**未实现的功能：**
- `write_mic_data()` 返回 -1（src/platform/macos/microphone.mm:97）
- `init_mic_redirect_device()` 返回 -1（src/platform/macos/microphone.mm:102）
- 客户端麦克风音频被丢弃，无法在 macOS 主机上使用

**影响：**
- AirType 语音输入无法工作
- Zoom/Teams 等通话应用无法使用客户端麦克风
- 远程协作场景受限

#### 2.2 技术方案：使用 BlackHole 虚拟音频设备

**方案选择理由：**
1. BlackHole 是成熟的开源虚拟音频驱动
2. 支持 loopback 模式（输出即输入）
3. 系统级集成，所有应用自动识别
4. 安装简单（brew install blackhole-2ch）
5. 无需开发自定义驱动

**音频流架构：**
```
客户端麦克风
    ↓
Moonlight 捕获
    ↓
网络传输（Opus 编码）
    ↓
Sunshine 接收 (stream.cpp::write_mic_data)
    ↓
解码为 PCM
    ↓
macos_audio_control_t::write_mic_data()
    ↓
AVAudioEngine + AVAudioPlayerNode
    ↓
输出到 BlackHole 2ch（作为音频输出设备）
    ↓
macOS 音频系统（BlackHole 同时也是输入设备）
    ↓
应用程序选择 BlackHole 作为麦克风
    ↓
AirType / Zoom / Teams / Siri
```

#### 2.3 实现细节

**核心组件：**

1. **AVAudioEngine 音频播放管道**
   - 创建 AVAudioEngine 实例
   - 添加 AVAudioPlayerNode 节点
   - 配置音频格式（48kHz, 2ch, float32）
   - 连接到 mainMixerNode
   - 设置输出设备为 BlackHole

2. **音频数据转换**
   - 输入：客户端发送的 PCM 数据（通常是 int16）
   - 转换：int16 → float32（归一化到 -1.0 ~ 1.0）
   - 输出：AVAudioPCMBuffer 格式

3. **缓冲策略**
   - 使用 AVAudioPlayerNode 的内置缓冲
   - 通过 `scheduleBuffer:completionHandler:` 调度播放
   - 处理网络抖动和延迟

**关键代码结构：**

```cpp
struct macos_audio_control_t: public audio_control_t {
  AVAudioEngine *audio_engine {};
  AVAudioPlayerNode *player_node {};
  AVAudioFormat *audio_format {};

  int init_mic_redirect_device() override {
    // 1. 查找 BlackHole 设备
    // 2. 创建 AVAudioEngine 和 AVAudioPlayerNode
    // 3. 配置音频格式（48kHz, 2ch, float32）
    // 4. 连接节点到 mainMixerNode
    // 5. 设置输出设备为 BlackHole
    // 6. 启动音频引擎
    // 7. 启动播放节点
  }

  int write_mic_data(const char *data, size_t size, uint16_t seq) override {
    // 1. 创建 AVAudioPCMBuffer
    // 2. 转换 PCM 数据（int16 → float32）
    // 3. 调度播放到 AVAudioPlayerNode
  }

  void release_mic_redirect_device() override {
    // 清理资源
  }
};
```

#### 2.4 设备检测和配置

**自动检测逻辑：**
1. 检查用户配置的 `audio.virtual_sink`
2. 如果未配置，自动查找 BlackHole 设备
3. 优先级：BlackHole 2ch > BlackHole 16ch > 其他虚拟设备
4. 如果未找到，记录警告并禁用功能

**配置选项：**
```ini
[audio]
virtual_sink = BlackHole 2ch  # 客户端麦克风输出到此设备
audio_sink = BlackHole 2ch    # 系统音频从此设备捕获（现有功能）
```

**启动时检查：**
```cpp
if (!isBlackHoleInstalled()) {
  BOOST_LOG(warning) << "BlackHole not detected. Remote microphone feature will be disabled.";
  BOOST_LOG(info) << "To enable remote microphone:";
  BOOST_LOG(info) << "  1. Install BlackHole: brew install blackhole-2ch";
  BOOST_LOG(info) << "  2. In System Settings → Sound → Input, select 'BlackHole 2ch'";
  BOOST_LOG(info) << "  3. Restart Sunshine";
}
```

#### 2.5 用户配置指南

**安装步骤：**
1. 安装 BlackHole：
   ```bash
   brew install blackhole-2ch
   ```

2. 配置 Sunshine：
   ```ini
   [audio]
   virtual_sink = BlackHole 2ch
   ```

3. 配置 macOS 系统：
   - 打开"系统设置" → "声音" → "输入"
   - 选择 "BlackHole 2ch" 作为输入设备

4. 配置应用程序：
   - 在 AirType/Zoom/Teams 中选择 "BlackHole 2ch" 作为麦克风

**注意事项：**
- BlackHole 是 loopback 设备，输出即输入
- 如果同时需要听到系统音频和使用远程麦克风，需要创建"聚合设备"
- 可以在"音频 MIDI 设置"中创建聚合设备，包含物理输出和 BlackHole

---

### 三、错误处理

#### 3.1 鼠标优化错误处理

**位置缓存失效：**
- 检测到缓存位置超出屏幕边界
- 回退到查询系统位置
- 重新验证并更新缓存

**边界检查：**
- 使用 `CGDisplayBounds()` 获取显示器边界
- 使用 `std::clamp()` 限制鼠标位置
- 防止越界导致的系统错误

**日志记录：**
- 记录鼠标事件处理时间（debug 级别）
- 记录缓存命中率（info 级别）
- 记录异常情况（warning/error 级别）

#### 3.2 音频功能错误处理

**初始化失败：**
- BlackHole 未安装 → 记录警告，返回 -1，禁用功能
- 设备被占用 → 尝试共享模式
- 音频引擎启动失败 → 记录错误详情，返回 -1

**运行时错误：**
- 音频引擎停止 → 尝试重启，记录错误
- 缓冲区调度失败 → 丢弃当前帧，记录警告
- 设备断开 → 停止播放，通知客户端

**采样率处理：**
- 客户端采样率与配置不匹配 → 自动重采样
- 通道数不匹配 → 自动转换（mono → stereo 或 stereo → mono）

---

### 四、测试计划

#### 4.1 鼠标优化测试

**功能测试：**
- 快速移动测试（测量延迟改善）
- 精确点击测试（小按钮、菜单项）
- 边界测试（屏幕边缘移动）
- 多显示器测试

**性能测试：**
- 测量鼠标事件处理延迟
- 测量 CPU 使用率变化
- 测量缓存命中率

**配置测试：**
- 不同灵敏度设置（0.5, 1.0, 1.5, 2.0）
- 验证配置文件加载
- 验证默认值行为

#### 4.2 音频功能测试

**功能测试：**
- AirType 语音输入验证
- Zoom/Teams 通话测试
- Siri 语音助手测试
- 长时间运行稳定性测试

**音频质量测试：**
- 延迟测量（期望 < 200ms）
- 音频清晰度检查（无爆音、断续）
- 不同采样率测试（16kHz, 48kHz）
- 不同通道数测试（mono, stereo）

**兼容性测试：**
- BlackHole 2ch 测试
- BlackHole 16ch 测试
- 与系统音频捕获共存测试
- 多客户端连接测试

**错误场景测试：**
- BlackHole 未安装场景
- 设备被占用场景
- 网络抖动场景
- 音频引擎崩溃恢复

---

### 五、实现文件清单

#### 5.1 需要修改的文件

**核心实现：**
1. `src/platform/macos/input.cpp` - 鼠标优化实现
2. `src/platform/macos/microphone.mm` - 音频播放实现
3. `src/platform/macos/av_audio.h` - 添加音频播放接口声明

**配置和构建：**
4. `src/config.h` - 添加配置选项声明
5. `src/config.cpp` - 添加配置选项实现
6. `CMakeLists.txt` - 确保链接 AVFoundation 框架

**文档：**
7. `docs/configuration.md` - 添加配置说明
8. `docs/getting_started.md` - 添加 macOS 麦克风设置指南
9. `CLAUDE.md` - 更新 macOS 音频架构说明

#### 5.2 新增文件

无需新增文件，所有功能在现有文件中实现。

---

### 六、实现优先级

#### Phase 1：核心功能（1-2 天）
1. 鼠标位置缓存优化
2. 麦克风接收基础实现（BlackHole 支持）
3. 基本错误处理

#### Phase 2：增强功能（1 天）
1. 鼠标灵敏度配置
2. 音频格式自动转换
3. 设备自动检测

#### Phase 3：完善和测试（1 天）
1. 错误恢复机制
2. 性能监控和日志
3. 文档更新
4. 完整测试

**总计：3-4 天开发周期**

---

### 七、性能指标

#### 7.1 鼠标优化目标

- 延迟降低：20-30%
- CPU 使用率降低：10-15%
- 缓存命中率：> 95%

#### 7.2 音频功能目标

- 端到端延迟：< 200ms
- 音频质量：无明显失真、爆音、断续
- CPU 使用率增加：< 5%
- 内存使用增加：< 10MB

---

### 八、风险和限制

#### 8.1 技术风险

**鼠标优化：**
- 缓存失效可能导致短暂的位置不同步
- 不同 macOS 版本的 CGEvent API 行为差异

**音频功能：**
- BlackHole 未安装时功能不可用
- 音频延迟受网络质量影响
- AVAudioEngine 在某些 macOS 版本上可能有 bug

#### 8.2 用户体验限制

**需要用户操作：**
- 安装 BlackHole（一次性）
- 配置系统音频输入设备（一次性）
- 在应用中选择麦克风（每个应用一次）

**潜在问题：**
- 用户可能不理解虚拟音频设备概念
- 配置步骤相对复杂
- 需要清晰的文档和错误提示

#### 8.3 缓解措施

1. 提供详细的安装和配置文档
2. 启动时自动检测并提示用户
3. 在 WebUI 中添加配置向导
4. 提供故障排除指南

---

### 九、未来改进方向

#### 9.1 短期改进（3-6 个月）

1. **WebUI 配置向导**
   - 检测 BlackHole 安装状态
   - 提供一键安装脚本
   - 可视化配置流程

2. **音频质量优化**
   - 自适应缓冲区大小
   - 丢包恢复算法
   - 回声消除

#### 9.2 长期改进（6-12 个月）

1. **自定义虚拟麦克风驱动**
   - 开发 Audio Server Plugin
   - 无需用户手动配置
   - 更好的系统集成

2. **高级鼠标功能**
   - 自定义加速度曲线
   - 游戏模式（禁用加速度）
   - 多显示器优化

---

## 总结

本设计方案通过以下方式优化 macOS 远程输入体验：

1. **鼠标优化**：通过位置缓存和直接事件创建，降低延迟 20-30%
2. **远程麦克风**：使用 BlackHole 虚拟设备，实现客户端麦克风在 macOS 主机上的使用
3. **用户友好**：提供清晰的配置指南和自动检测机制
4. **快速实现**：3-4 天开发周期，立即可验证效果

该方案平衡了实现复杂度、用户体验和功能完整性，是当前最优的技术选择。
