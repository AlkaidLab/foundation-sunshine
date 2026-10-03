# 网络丢包与 FEC 发布验证记录

本记录配套[实施文档](adaptive-fec-implementation.zh-CN.md)，面向可发布源码组合。更新日期为 2026 年 10 月 3 日。当前仍为实验方案，主方案 M4 尚未通过，不能宣称共享预算完整验收、设备性能或 QoE 收益已经完成。

## 版本与证据范围

Sunshine 发布分支基于 `59bea4757333550b954ae67bc1cf1ece649e3cf6`，合入原工作区以 `87ea00bc4` 为基线的传输变更，未纳入控制面板、USB 或其他无关修改。主机最终发布提交和构建验证将在完成后登记。公共依赖固定如下：

| 依赖 | 固定提交 | 可审查入口 |
| --- | --- | --- |
| ENet | `d906d15bfc52792c12d851305a5eae58fe14c968` | [发送准入 PR #1](https://github.com/qiin2333/enet/pull/1) |
| moonlight-common-c | `fd3b370f1ffaae00b2a59f98773f58a5235175aa` | [逐包反馈与通知 PR #31](https://github.com/qiin2333/moonlight-common-c/pull/31)；基于 `mic`，保留既有 RFI/IDR 修复 |
| GoogCC 及依赖 | [versions.json](../third-party/webrtc-googcc/versions.json) | [获取、构建及兼容说明](../third-party/webrtc-googcc/README.md) |

每项结果只证明所列版本、工具链和运行范围。原工作区的长期实验记录与原始失败日志继续保留；私有配对凭据、设备信息、逐包日志与媒体不进入发布源码。实施文档提到的历史阶段用于定位既有实现，不能替代本发布版本的复验。

## 本次依赖复验

| 验证 | 结果 | 证明范围 |
| --- | --- | --- |
| ENet 独立 Windows/UCRT GCC Release | 构建与 CTest 通过；六项实际 IPv4 loopback GTest | 拒绝后 ACK/命令保留、可靠超时、分片字节核对、未安装回调兼容、拒绝到期 ping 后有界重试、重新准入后的自动发送 |
| ENet 最终提交 GitHub CI | Windows/Linux/macOS Debug/Release 构建、Linux 六项回归通过 | 依赖编译和回环测试；不覆盖 Sunshine 多会话公平性、真实 OS 部分提交或业务成本 |
| common c Windows/UCRT GCC Debug/Release | 两种静态构建与各 11 个 CTest 通过 | 公共测量、反馈、通知、协商、AES-GCM 及既有协议回归；不包含网络控制握手、Qt/Android 应用运行 |
| common c Windows/MSVC Debug | DLL 和内部静态测试库构建及 11 个 CTest 通过 | 保留 Debug 断言及真实 RS 合成恢复校验，验证原子支持、导出构建和会话重置/清理；不是 DLL 消费者的运行验收 |
| common c Linux/WSL GCC 11.4 + MbedTLS 3.6.7 | Debug/Release 共享构建与各 11 个 CTest 通过 | 执行既有 PSA 加密后端的已知向量、认证失败和复用测试；官方依赖发布包校验 SHA-256 |
| common c 多平台 CI | `70f41ae` 的 9 个任务通过；日志表明旧矩阵的 Linux Clang 标签实际使用 GCC。`fd3b370` 修正 CC/CXX 后重新验证，终态待检查 | 已有 Windows x64/ARM64、macOS、Linux x64/ARM GCC、OpenSSL/MbedTLS 证据；真实 Linux Clang 覆盖与新 review 暂未通过 |
| Sunshine 发布版本传输组件 | Windows/UCRT GCC Release，267 个 GTest、4/4 CTest 通过 | 新 master 与公开依赖组合的策略、预算、反馈、期限和通知组件；不代表完整主机或应用闭环 |
| Sunshine 发布版本出口组件 | Windows/UCRT GCC Release，2/2 CTest 通过 | 平台 UDP 与 ENet 发送准入，含实际回环；独立构建显式引用已准备的 FFmpeg 头文件 |
| Sunshine 发布版本 GoogCC | Windows/UCRT GCC Release，3/3 CTest 通过 | 固定上游、适配/运行、线格式的独立测试；容量重放是合成输入，不代表媒体收益或完整闭环 |
| Sunshine 发布版本完整主机 | 待 OFF/ON 构建与运行 | 需在最终公共依赖组合上继续真实出口、SDK 与应用闭环复验 |
| PC/Android 发布版本 | 待准备与复验 | 旧工作区证据不直接提升为新发布版本通过 |

通知回归覆盖独立 72 字节向量、高位 64 位计数、版本/长度/保留位校验与原子拒绝、历史进度和可下降的 encoder-ready 状态、停止与重初始化隔离。队列生命周期测试保留真实 RS 生成、丢失五个源分片后的恢复和载荷逐字节核对；快照读取与反复销毁重建并发测试单列。

## 已复现并处理的失败

| 失败 | 证据与处理 | 当前边界 |
| --- | --- | --- |
| ENet 拒绝到期 ping 时服务循环空转 | 修复前 60 ms 内发生 258972 次拒绝；增加 deferred 标志和名义 2 ms 有界等待，不修改未发送包的成功发送时间 | 回归验证有界重试、显式 flush 和早于下一次 ping 的自动重新准入；不是 CPU 或延迟性能验收 |
| common c Debug 清理断言与队列夹具失败 | 初始化的控制流可在启动前被连接阶段回滚；保持停止状态直到 startup。夹具分别初始化会话，普通完成使用无 FEC 块，真实恢复测试保留 Debug 校验 | Debug/Release 复验通过；完整应用的启动、停止及重连仍单独验收 |
| MSVC 编译警告与原子支持 | 控制加密封装校验 16 位长度后转换；主库与原子测试启用 C11 原子，主库明确 UTF-8 | MSVC Debug 构建和回归通过；跨工具链结果以 CI 为准 |
| 默认 DLL 和旧加密依赖的链接失败 | Windows 生成导出表，内部全局状态测试使用相同配置的静态库；配置检测 PSA multipart AEAD，CI 改用校验散列的官方 MbedTLS 3.6.7 | 未改生产密码算法；两种后端执行同一已知向量、认证拒绝和复用回归，9 个 CI 任务通过 |
| CI 编译器标签与实际执行不符 | 从配置日志发现 Linux Clang 任务实际运行 GNU 13.3；工作流显式设置矩阵的 CC/CXX | 保留旧结果的实际 GCC 范围；修正后真实 Clang 结果单独检查 |

历史隔离串流还观察到无损场景中的编码回压和强突发严格恢复目标未达标。回压继续默认关闭，强突发仍需参考链、有效帧交付、画质和延迟的独立验收；策略已经变化不等于体验改善。

## 主方案剩余验收

- P0：固定两端内容与设备，完成 V1/V3 和 V6 的实际参数、测量方法与预登记容限。
- P1：共享预算的多会话公平性、非 loopback、真实部分提交/重传、大音频分片及控制/音频连续性。
- P2：完整容量下降与恢复、静态转运动、探测收益和 ALR/处理受限原因消歧。
- P3：两端业务统计和完整控制会话，通知故障及成本，手动接管与旧 epoch 隔离。
- P4：SDK 失败与重建、参考链和期限，PC 默认渲染及 Android Game/JNI 实际设备生命周期、温升。
- P5：同实际 IP 字节预算、同恢复期限的单项消融，画质/冻结/互动延迟尾部/成本，兼容与可执行回退。

M4 按实施文档逐项判定。M5 RTX 和 M6 窗口编码/预测另行验收，缺少期限和独立收益证据时保持关闭。实验开关及自动控制权限保持原有边界。

## 复验与结果登记

组件、构建和网络矩阵的命令见[实施文档](adaptive-fec-implementation.zh-CN.md#测试计划与验收记录)。每次保存准确源码/子模块提交、构建开关、工具链、终态日志、原始输入和结果；性能对照前冻结阈值，失败和未测与通过一并报告。仅 Debug/Release 组件通过时，不宣告完整应用或闭环收益通过。
