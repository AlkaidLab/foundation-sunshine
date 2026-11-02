# MAT 主机接入

主机沿用外置采集器和 Sunshine 的实时音频会话。采集器接收混音前对象，交给独立
原生 MAT 编码器；Sunshine 接完整周期，经认证加密的实时音频通道发送。客户端
当前用 mock 接收端验证契约，实际 HDMI/eARC 播放另行验收。

## 使用入口

1. 按 [原生采集器说明](../tools/sonic-mat-live/README.md) 构建工具，
   对指定 audiodg 的 PID/创建时间做 Preflight，再显式 Start。状态最初可以是
   `WaitingForObjects`；应用产生受支持的空间对象后才会出现 `NativeReady`。
2. 在 Windows Web UI 的音频/视频页开启「MAT 音频（实验）」，保存并应用。
   采集工具启动后提供固定的本地入口，主机自动连接，无需复制管道或程序路径。
   开关控制主机是否使用 MAT，不会启动或提权运行采集工具。

3. 客户端建立会话时重新协商。一个采集器只提供一个活动 MAT 租约；重连取得新
   会话和密钥，PLAY 前旧周期被清空。客户端退出释放租约，采集器可继续等待连接。
4. 用同一次 Start 产生的 `capture.json` 查询 Status 或执行 Stop，检查
   `StoppedVerified`。原生故障保留日志，修正原因后再显式重新启动。

主机只需 `mat_live = true`（默认关闭）。旧 `mat_live_pipe` / `mat_live_producer`
不再用于接入，Web UI 读取和保存配置时会去除它们。
状态自动更新，`GET /api/mat/status` 只读主机最近观测，不连接采集管道或占用租约。
保存的开关仍需应用后生效；首次会话连接前显示「等待采集工具」，释放后等待重新连接。
源就绪不代表设备播放验收。外置采集器的对象输入状态仍通过其管理工具查看。

自动入口只接受受信任的提升权限进程，程序必须来自 Program Files 下的
`SunshineSonicMatLive/<exe-SHA256>/sunshine-plugin-mat-capture.exe`，并核对内容哈希。
随后仍检查管道服务端 PID、发布器/引擎创建时间和原生描述符；只有客户端协商 MAT
时才尝试连接。入口被冒充、未就绪或被占用时仍走现有协商失败/回退逻辑。

采集器尚未就绪、未配置或已被占用时，主机不提供 live profile；兼容客户端可在
ANNOUNCE 前选择普通音频。严格 MAT 请求被拒绝。已经选择 MAT 后源失败会结束
该会话，重连时重新协商，不在运行中的会话里替换音频编码。

## 主机约束

- 只接经过原生进程身份核对的本地采集源；mock/test 描述符不能作为生产输入。
- 配对采集器使用固定 8 个 encoder 槽位，允许不同空域与对象域并发回调。
  同一 encoder 的重叠 pass、pass 内 reset/destroy、容量耗尽或无效输入仍终止采集；回调不等待、
  不分配内存、不静默丢事件。实际编码仍只选择一个有对象的域。
- 版本、格式和时钟在开始前检查；逐周期仍校验 48 kHz 载波时钟、960 ticks、
  61,440 字节、连续序号和载波头。
- 队列有界，源停滞、数据缺口、发送超时或连接失败终止，不补静音、丢帧追赶或
  重放旧数据。失败不发送成功 EOS。
- 准备失败、读取失败、正常 EOS 和停止都会释放管道与进程句柄；故障原因保留到
  日志，供定位采集、身份、数据或连接问题。

当前对象桥仍限于一个对象 renderer、48 kHz/480 帧、至多 20 个完整输入对象，
支持动态对象和 7.1.4 的 12 个静态声道类型（包含 LFE）。静态声道不转换成动态对象；
同一静态声道多个来源暂不混音，缺失声道不补静音。不支持的静态类型、分段对象、
其他采样率和多个同时工作的 renderer 会拒绝；这不等于已支持
所有游戏。原生 ABI 与组件版本固定，构建成功不能证明其他系统版本可用。

## 系统组件版本

采集钩子和原生 worker 依赖 `audioeng.dll` 与 Dolby Access（`DolbyMatEnc.dll`、
`DolbyMatEngine.dll`）的内部偏移，只在
[`mat_compat.h`](../src/spatial_audio/mat_compat.h) 列出的已验证文件哈希上运行。
Windows 累积更新或 Dolby Access 商店更新改变这些文件后，MAT 不可用，普通音频不受影响：

- 采集工具的 Preflight/Start 先输出每个组件的 `COMPONENT` 行（版本、哈希、是否支持），
  未验证时以退出码 3 结束，不挂钩也不提权；管理入口提示附上这些行反馈，
  Status 显示 `Unsupported`。主机状态页此时仍显示「等待采集工具」。
- Dolby Access 按包族名查找当前安装目录，商店更新版本号或安装到其他盘不需要改代码；
  只要 DLL 内容仍在已验证列表即可继续使用。worker 在 audiodg 内仍校验目录形状和哈希。
- 新版本须重新完成原生验收后，在 `mat_compat.h` 增加对应哈希。

## 验证

常规 CI 运行轻量主机契约、对象回调、IPC、管道和管理入口回归；客户端使用 mock，
不启动原生捕获、现场信号或完整 RTSP/视频会话。
扩展软件集成按需执行：

```powershell
./tests/mat-live/verify.ps1 -Output <全新目录> -Python <Python路径>
```

测试中的 mock 客户端复用协议解析器，覆盖正常结束、连接中断后新会话、取消、
源启动/读取失败、空流、时钟/格式/序号错误、停滞看门狗和资源释放。客户端应用
及手机输出不参与本轮开发。软件生成的载波形状数据只在测试中使用。

实时共享协议已通过 [common-c #40](https://github.com/qiin2333/moonlight-common-c/pull/40)
合入 `mic`（`24f23691a1c303e49cab9334414d35e023ba7f97`），主机随 master 固定到包含它的
`733025a254e6e166a4afb577cbe056c10257f950`。本地 AUTO6
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

独立 Sonic HRTF 入口、tap 及其专用测试另外保存在
[codex/mat-sonic-archive-20261023](https://github.com/AlkaidLab/foundation-sunshine/tree/codex/mat-sonic-archive-20261023)
（`d006e4b`）。当前仅构建 plugin 捕获入口，保留共用对象桥、原生 worker 和回归。
