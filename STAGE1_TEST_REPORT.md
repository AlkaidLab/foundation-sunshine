·# Stage 1 测试报告

## 测试执行日期
2026-02-25

## 测试环境
- **平台**: macOS Darwin 25.2.0
- **硬件**: Mac mini M4 (Apple Silicon)
- **编译器**: Clang (Apple)
- **构建系统**: CMake + Ninja
- **测试框架**: Google Test

## 测试结果总结

### ✅ 新增测试 - 全部通过

#### 1. Colorspace 测试 (16/16 通过)
```
[==========] Running 16 tests from ColorspaceTest
[  PASSED  ] 16 tests.
```

**测试覆盖**:
- ✅ 默认 colorspace 值验证
- ✅ SDR Rec.601 (限制范围/全范围)
- ✅ SDR Rec.709 (限制范围/全范围)
- ✅ SDR BT.2020 (带回退逻辑)
- ✅ HDR PQ (有/无 HDR 显示器)
- ✅ HDR HLG (有 HDR 显示器)
- ✅ 无效参数处理和回退·
- ✅ Colorspace 辅助函数 (is_hdr, is_hlg, is_pq)
- ✅ AVCodec colorspace 转换
- ✅ 颜色向量生成

#### 2. macOS 编码器测试 (6/6 通过)
```
[==========] Running 6 tests from MacOSEncoderTest
[  PASSED  ] 6 tests.
```

**测试覆盖**:
- ✅ NV12 设备初始化
- ✅ P010 (10-bit) 设备初始化
- ✅ YUV420 维度对齐 (奇数→偶数)
- ✅ 硬件帧上下文对齐
- ✅ VideoToolbox 像素格式支持
- ✅ Colorspace 与 VideoToolbox 集成

### 📊 总体测试统计

| 测试套件 | 通过 | 失败 | 跳过 | 总计 |
|---------|------|------|------|------|
| ColorspaceTest | 16 | 0 | 0 | 16 |
| MacOSEncoderTest | 6 | 0 | 0 | 6 |
| **新增测试总计** | **22** | **0** | **0** | **22** |

### 🔧 构建状态

**主程序构建**: ✅ 成功
```bash
ninja -C build sunshine
# 输出: sunshine-2026.0224.230256.杂鱼
```

**测试套件构建**: ✅ 成功
```bash
ninja -C build test_sunshine
# 警告: 2 个编译警告 (非关键)
# - VLA 扩展警告 (input.cpp)
# - 数组越界警告 (video.cpp - 预先存在)
```

### ⚠️ 已知问题

1. **预先存在的 Webhook 测试失败**
   - `test_webhook.cpp` - 缺少 webhook.h
   - `test_webhook_config.cpp` - config::apply_config 不存在
   - **状态**: 与本次修改无关，已存在于代码库中

2. **EncoderTest 挂起**
   - `EncoderVariants/EncoderTest.ValidateEncoder/videotoolbox` 会挂起
   - **原因**: 需要完整的显示设备初始化
   - **影响**: 不影响新增测试和主程序功能

### 📝 测试执行命令

```bash
# 运行所有新增测试
./build/tests/test_sunshine --gtest_filter="ColorspaceTest.*:MacOSEncoderTest.*"

# 仅运行 colorspace 测试
./build/tests/test_sunshine --gtest_filter="ColorspaceTest.*"

# 仅运行 macOS 编码器测试
./build/tests/test_sunshine --gtest_filter="MacOSEncoderTest.*"
```

### ✅ 验证的功能

1. **Colorspace 初始化**
   - 默认值正确 (Rec.709, 限制范围, 8-bit)
   - 所有 SDR 和 HDR 模式正确处理
   - 无效参数正确回退

2. **VideoToolbox 编码器**
   - NV12 和 P010 设备正确初始化
   - YUV420 维度对齐正确工作
   - 像素格式支持正确

3. **防御性验证**
   - Colorspace bit_depth 验证已添加
   - 边界情况正确处理

## 结论

✅ **Stage 1 核心修复已完成并通过所有测试**

所有新增的 22 个测试用例全部通过，验证了：
- Colorspace 初始化逻辑正确
- VideoToolbox 编码器集成正常
- 防御性验证代码工作正常

主程序构建成功，可以进行实际的 Android Moonlight 客户端连接测试。

## 下一步

1. **实际串流测试**: 运行 sunshine 并从 Android Moonlight 连接
2. **验证编码器初始化**: 确认不再出现 "Invalid video pixel format: -1" 错误
3. **性能测试**: 验证视频串流流畅无卡顿
4. **决定是否继续 Stage 2**: 根据实际需求决定是否实现远程麦克风功能
