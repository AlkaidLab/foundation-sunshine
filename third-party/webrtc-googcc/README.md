# 固定版本 GoogCC 验证依赖

此目录保存依赖锁定、稀疏获取和最小 Windows 构建入口。`versions.json` 锁定实际 WebRTC、它的 DEPS 所指定 Chromium Abseil 检出与 GoogleTest；不使用浮动分支。算法实现来自上游完整源文件，当前没有改写 GoogCC 算法。

```powershell
python third-party/webrtc-googcc/bootstrap.py --output .codex-build/googcc-deps
cmake -S tests/googcc -B .codex-build/adaptive-fec-googcc -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DSUNSHINE_GOOGCC_DEPS_DIR="$PWD/.codex-build/googcc-deps"
cmake --build .codex-build/adaptive-fec-googcc --parallel 4
ctest --test-dir .codex-build/adaptive-fec-googcc --output-on-failure
```

使用本机 C++23 编译器和 Python 3。需要时显式指定 `CMAKE_C_COMPILER`、`CMAKE_CXX_COMPILER`、`Python3_EXECUTABLE`。当前实际验证为 Windows/UCRT GCC 15.2；此入口尚未验证其他平台和 MSVC。普通 Sunshine 构建没有启用这个控制器。

获取脚本拒绝覆盖非 Git 目录或脏检出；配置检查固定 HEAD 与原始 tracked 源码。依赖许可证、WebRTC `PATENTS`、`AUTHORS` 以及 Chromium 的 Abseil 补丁来源保留在检出中。发布时必须将适用许可证与实际构建产物一起交付。

构建生成两类兼容文件：Chromium GoogleTest 路径的纯 include 转发头，以及 MinGW 下的线程命名源文件副本。后者只为 MSVC 专用的旧 SEH 调试器命名分支增加条件编译，保留 Windows `SetThreadDescription`；校验原文件规范化换行后的 SHA-256，不修改依赖检出。字段试验头由固定上游脚本生成，启用严格字段试验检查。

`googcc_upstream_tests` 运行选定的十组原始算法测试，不声称运行了整个 WebRTC 串流测试套件。`googcc_input_tests` 使用 `src/googcc_adapter.*` 与成功发送账本检查输入映射、重放、时钟 epoch 与探测元数据，并输出 `googcc-capacity-trace.csv`。

适配器使用每个接收时钟 epoch 的固定偏移将到达时间映射到估计器时域，保留到达差分；这不能用于测量单向网络延迟。`receiver_sample_time_us` 必须来自报告生成时的接收单调时钟，不能用服务端收到反馈的时间冒充。真实时钟漂移、应用层时间噪声及反馈聚合须通过项目 V3 实测。

容量模型是合成的工作守恒链路，生成流量跟随估计目标，报告周期 50 ms、两向固定传播各 20 ms、在链路后按包身份确定性损伤约 1%。容量为 40→8→25 Mbps，每段 5 秒；最后再排空 1 秒。该轨迹没有编码器、操作系统出口、期限调度和真实探测流量，不能作为 V6 性能验收或真实串流闭环。模型的探测请求被读取但未执行；探测元数据另由独立输入场景验证。

发送账本对接收字节与缺失更正只计一次；估计器则按上游接口收到首次缺失和一次后到接收转换。上游 LossBasedBweV2 的内部观察窗口属于估计统计，不能当作 UI 的唯一原始包分母，也不能据此宣称更正会回写所有既有估计窗口。

主方案完成状态与剩余验收见[实施文档](../../docs/adaptive-fec-implementation.zh-CN.md)和[验证记录](../../docs/adaptive-fec-validation.zh-CN.md)。
