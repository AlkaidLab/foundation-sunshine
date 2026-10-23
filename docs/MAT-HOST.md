# MAT 主机接入

主机沿用外置采集器和 Sunshine 的实时音频会话。采集器接收混音前对象，交给独立
原生 MAT 编码器；Sunshine 接完整周期，经认证加密的实时音频通道发送。客户端
当前用 mock 接收端验证契约，实际 HDMI/eARC 播放另行验收。

## 使用入口

1. 按 [原生采集器说明](../spatial-object-spike/sonic-mat-live/README.md) 构建工具，
   对指定 audiodg 的 PID/创建时间做 Preflight，再显式 Start。状态最初可以是
   `WaitingForObjects`；应用产生受支持的空间对象后才会出现 `NativeReady`。
2. 将 Start 输出的 `host-config-snippet.conf` 两行加入目标 Sunshine 配置，按正常
   流程重启该主机使配置生效。管理工具不修改已安装主机或服务。

   ```ini
   mat_live_pipe = \\.\pipe\SunshineMatLive-<本次采集标识>
   mat_live_producer = <本次启动输出的采集器完整路径>
   ```

3. 客户端建立会话时重新协商。一个采集器只提供一个活动 MAT 租约；重连取得新
   会话和密钥，PLAY 前旧周期被清空。客户端退出释放租约，采集器可继续等待连接。
4. 用同一次 Start 产生的 `capture.json` 查询 Status 或执行 Stop，检查
   `StoppedVerified`。原生故障保留日志，修正原因后再显式重新启动。

两项配置都留空时关闭实时 MAT；只填写一项会在协商日志中给出配置错误。
采集器尚未就绪、未配置或已被占用时，主机不提供 live profile；兼容客户端可在
ANNOUNCE 前选择普通音频。严格 MAT 请求被拒绝。已经选择 MAT 后源失败会结束
该会话，重连时重新协商，不在运行中的会话里替换音频编码。

## 主机约束

- 只接经过原生进程身份核对的本地采集源；mock/test 描述符不能作为生产输入。
- 版本、格式和时钟在开始前检查；逐周期仍校验 48 kHz 载波时钟、960 ticks、
  61,440 字节、连续序号和载波头。
- 队列有界，源停滞、数据缺口、发送超时或连接失败终止，不补静音、丢帧追赶或
  重放旧数据。失败不发送成功 EOS。
- 准备失败、读取失败、正常 EOS 和停止都会释放管道与进程句柄；故障原因保留到
  日志，供定位采集、身份、数据或连接问题。

当前对象桥仍限于一个对象 renderer、48 kHz/480 帧、至多 20 个完整动态对象。
静态床、分段对象、其他采样率和多个同时工作的 renderer 会拒绝；这不等于已支持
所有游戏。原生 ABI 与组件版本固定，构建成功不能证明其他系统版本可用。

## 验证

主机软件验证入口：

```powershell
./spatial-object-spike/mat-live/verify.ps1 -Output <全新目录> -Python <Python路径>
```

测试中的 mock 客户端复用协议解析器，覆盖正常结束、连接中断后新会话、取消、
源启动/读取失败、空流、时钟/格式/序号错误、停滞看门狗和资源释放。客户端应用
及手机输出不参与本轮开发。软件生成的载波形状数据只在测试中使用。

此前受控原生短测已经通过真实 MAT、加密 RTSP 和 H.264 接收，见
[原生管理入口记录](../spatial-object-spike/mat-rtsp-live/MAT-LIFECYCLE-RESULT.txt)。
本轮主机硬化以新构建和 mock 验证为准，不把历史原生结果改写成新版本实测。
持续原生运行、主动中途停止、真实多会话重连、游戏音画同步和 HDMI/eARC Atmos
识别仍是后续验证项；本机没有相应终端条件。

## 2026-10-23 审查收尾

此前 `8a51e978` 的 18 项跨平台 CI 全部通过；职责审查随后要求收窄未发布 API。
当前主机引用 `c6b340440a14997145438d2a9f2059852c257b69`：common-c 保留协议、
认证、网络接收和 cycle/EOS 回调，文件落盘和原始录制输入迁至
[诊断工具](../spatial-object-spike/mat-diagnostics/README.md)。`matDumpPath` 和离线
接收入口已删除，codec4 通过回调交付已验证数据。线上的 MAT profile/密文格式
保持兼容；使用新库的消费者须重建并适配上述未发布 API。

客户端继续用 mock 前端，直接链接当前被审查的 common 库。Android 应用/PR 仍为
历史版本，不升级其 shared 指针，也不作为本轮验收对象；历史证据不推算为新
提交的结果。后续采集生命周期修改继续留在主机侧。

职责收敛的软件验收记录为 `build/mat-boundary-live-v1/proof.json`：当前 common 库
14 项 CTest、21 个协商场景、独立加密 oracle、两个 1300 周期 UDP/FEC 用例和
391 个刻意丢包恢复，以及源契约/租约/管道/管理入口回归通过。迁出的诊断工具
通过 7 个落盘场景、7 个离线输入场景与成功/截断文件检查，实际 AudioStream
codec4/5 的回调失败、错误 SHA/count 也有回归覆盖。数据全部由软件生成；本轮
没有新增原生实采或客户端设备播放验证。

本轮人工审查覆盖配置与协商、源身份和租约、对象桥/原生 worker、管道收尾、
加密发送及退出路径，未留下已确认的阻塞代码问题。修正两处生命周期细节：
结束的 renderer 清理其速率/时间状态；Start 的最终状态查询或输出失败时仍执行
停止清理。采集工具重新构建，回调/对象桥、IPC、配对生命周期及管理入口软件
回归通过（`build/mat-host-review-lifecycle-v1`、`build/mat-host-review-operator-v1`）。
这两处修复没有重新原生实采；上文持续运行、主动停止、真实重连和硬件验收边界
继续适用。草稿 PR 的 CodeRabbit 自动审查被跳过，不计作人工审查之外的通过项。
