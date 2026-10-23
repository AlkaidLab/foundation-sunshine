# MAT 主机接入

主机沿用外置采集器和 Sunshine 的实时音频会话。采集器接收混音前对象，交给独立
原生 MAT 编码器；Sunshine 接完整周期，经认证加密的实时音频通道发送。客户端
当前用 mock 接收端验证契约，实际 HDMI/eARC 播放另行验收。

## 使用入口

1. 按 [原生采集器说明](../tools/sonic-mat-live/README.md) 构建工具，
   对指定 audiodg 的 PID/创建时间做 Preflight，再显式 Start。状态最初可以是
   `WaitingForObjects`；应用产生受支持的空间对象后才会出现 `NativeReady`。
2. 在 Windows Web UI 的音频/视频页展开「MAT 实时音频（实验）」，填写
   Start 输出的管道和采集程序路径，保存并重启；也可将 `host-config-snippet.conf`
   两行加入配置。管理工具不修改已安装主机或服务。

   ```ini
   mat_live_pipe = \\.\pipe\SunshineMatLive-<本次采集标识>
   mat_live_producer = <本次启动输出的采集器完整路径>
   ```

   程序路径用于核对管道发布进程的身份，Sunshine 不会执行或启动该路径。

3. 客户端建立会话时重新协商。一个采集器只提供一个活动 MAT 租约；重连取得新
   会话和密钥，PLAY 前旧周期被清空。客户端退出释放租约，采集器可继续等待连接。
4. 用同一次 Start 产生的 `capture.json` 查询 Status 或执行 Stop，检查
   `StoppedVerified`。原生故障保留日志，修正原因后再显式重新启动。

两项配置都留空时关闭实时 MAT；Web UI 和保存接口均拒绝只填写一项。
手工配置文件的半套配置仍在协商时拒绝。UI 的状态和 `GET /api/mat/status` 只读取
运行中主机的最近观测，不打开管道、不启动采集，也不随尚未应用的设置变化。
已配置但尚未建立过源连接时显示「尚未探测」；租约释放后不继续标为就绪。
采集器的 `WaitingForObjects` 等外部状态仍通过管理工具查看；主机就绪不代表设备播放验收。
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
./tests/mat-live/verify.ps1 -Output <全新目录> -Python <Python路径>
```

测试中的 mock 客户端复用协议解析器，覆盖正常结束、连接中断后新会话、取消、
源启动/读取失败、空流、时钟/格式/序号错误、停滞看门狗和资源释放。客户端应用
及手机输出不参与本轮开发。软件生成的载波形状数据只在测试中使用。

实时共享协议已通过 [common-c #40](https://github.com/qiin2333/moonlight-common-c/pull/40)
合入 `mic`，主机固定到 `24f23691a1c303e49cab9334414d35e023ba7f97`。本地 AUTO6
在 ANNOUNCE 前选择 live7 或 Opus0；旧 codec4/5 明确拒绝。生产主机没有录制文件
输入，客户端应用保持 mock，不更新历史 Android 应用或其 shared 指针。

[当前测试入口](../tests/mat-live/README.md) 覆盖协议、管道、对象回调、管理入口和
生成数据的 UDP/FEC 回归。软件构建及 mock 验证不等于原生实采或硬件播放验收。
持续原生运行、主动中途停止、真实多会话重连、游戏音画同步和 HDMI/eARC Atmos
识别仍是后续验证项，本机没有相应终端条件。

## 历史资料

清理前的完整版本 `be20f80bb8622b16ac6d45bbb339b1e9e55919cf` 已保存到
[归档分支 codex/mat-history-20261023](https://github.com/AlkaidLab/foundation-sunshine/tree/codex/mat-history-20261023)。
旧手机实验、有限回放、离线落盘、一次性探针、打包脚本和阶段报告仅在那里保留；
当前构建与测试不依赖归档目录。历史原生 MAT/RTSP/H.264 短测结果仍只对应当时版本。
