# 2026-10-22 续作：原生实时 MAT 链路

## 23:00 原生短测更新：已产出真实 MAT，尚未完整验收

用户本轮授权最多 3 次有界短测，已全部执行，不据此再次挂接。
前两次 UUYC 源均现场通过公开空间音频 API 提交 10 秒双对象，没有文件输入。
Sonic HRTF 挂接零回调；第二次在总分发入口实测 347 次调用，实际入口为
`MainPluginRenderer::Process (RVA 0xfa3b0)`，并加载 `DolbyHrtfEnc.dll`。
因此 UUYC 的 maxDynamic=128 不能作为当前选择 Windows Sonic 的证据。

第三次沿现有插件入口，用既有 q4 调用前 PCM/属性快照、返回配对及生命周期
记录器接独立 MAT worker；从 renderer 的状态条目实读 48 kHz，入口实读 480 帧。
两个动态对象已采到，原生 create/mapper/initialize/submit/process 均为 S_OK；
133 个原生 30,720 字节块、4,085,760 输出字节，主机组成 64 个完整 MAT 周期。
其中 47 个周期经过正式 live sender、AES-GCM、UDP/RTP/FEC 和真实客户端
AudioStream 重组，每周期 SHA256 对比通过。没有样本回放或载荷落盘。

随后一轮缺少仍存活对象，桥接器按规则终止，不猜测静音/结束；没有干净 EOS，
因此 `PASS_NATIVE_LOOPBACK`、完整 8 秒连续性和整流 SHA 验收均未通过。
缺失具体是空轮、对象身份变化还是输入欠载，现有日志不足以区分；已加入下一轮
失败对象 ID/代次/属性 flags/帧数诊断，尚未再做原生测试。
运行脚本的空日志就绪误判已修复；第三次结果中的“Live source did not finish”
是早退后等待时间不足的次生报告，源最终正常提交完 1000 块。新脚本保留主错误。

三次均独立检查原始入口字节恢复、子进程退出及同一 audiodg PID/创建时间；
第三次检查全部六个插件入口，未切换默认声卡、空间格式、驱动或服务。
DLL 按设计仍 pin 在 audiodg，挂接已停。手机仍走 Opus。
证据：`build/sonic-mat-native-run01..03/`、对应管理员 wrapper 结果及冻结 build v9。
下一步是定位缺失对象轮，再做完整实时验收；不是继续追查“编码器无法初始化”。
完整 Sunshine RTSP/视频、游戏同步和 HDMI/eARC Atmos 识别仍未验收。

下面保留 23:00 以前各阶段状态，不能覆盖上述实测结论或续用已消耗授权。

---

最新纠正：用户已选择现有 UUYC / Realtek 的 Windows Sonic 实时对象输入，接独立
MAT 编码器；主机 HDMI 不是这条路线的前提。之前只监听了现有编码器的输出，
零周期不能否定 Sonic 对象捕获。新连接器见 `spatial-object-spike/sonic-mat-live/`：
Sonic 混音前对象/设备帧数/EOS -> 有界 IPC -> 独立原生 MAT worker -> 既有实时
管道与串流。自身回调及 IPC FAKE-worker 测试已通过，仍待 UUYC 真回调/原生编码
及完整 Sunshine/硬件验收；尚未取得本轮新的 audiodg 注入授权。

后续本机续查：全端点只读枚举确认 F32D80U、SONY TV 等 HDMI 音频端点均为
`DEVICE_STATE_NOTPRESENT (0x4)`，默认 Steam Streaming Speakers 为双声道
48 kHz、可用动态对象数 0。UUYC 和 Realtek 为 128，但不能据此认定输出 MAT。
当前会话审计的瞬时峰值全为 0；这不证明整个探测期间无声。Dolby 两个组件的
文件 SHA 与历史原生实验一致。当前未进行新的注入、端点切换或样本回放。

修正采集循环旧时间快照可能早于刚收到的周期、导致无符号下溢误报停滞的问题。
新增逐记录零周期诊断、严格显式重用已停止 tap 和无文件输入的实时管道 loopback
检查器。生成软件字节的 UDP/RTP/FEC 回归通过，包含回绕及 391 包丢失恢复；
缺失管道和文件路径均拒绝。检查器尚未接到真实 MAT，不能当作实采成功。
新证据入口为 `build/mat-live-source-audit-v1/`、
`build/mat-live-source-software-v3/proof.json`；实现/使用边界见 live README。
主机实采源与完整 Sunshine/硬件端到端验收仍待；用户后续已选择 Sonic 对象输入
接独立编码器。不要从本文或历史一次授权推导出新的 audiodg 注入权限。

当前开发分支为 `codex/atmos-mat-live`。正常偏好值 6 现在只协商实时 codec 7 或
Opus 0，不会自动选择有限回放 codec 5。客户端继续沿用输出能力预检与路由失效
终止逻辑，不支持 MAT 的手机走 Opus；不继续手机 MAT 还原或对象渲染。

新增持续原生 Process 输出采集助手、私有本地管道、逐会话 live profile、64 位
AES-GCM 认证包计数和正式发送/接收路径。正常路径没有录音文件或固定周期数。
实现约束、构建和验收边界见 `spatial-object-spike/mat-live/README.md`。

软件测试和构建不能作为真实 MAT 数据采集成功的证据。本机普通权限预检返回
Win32 5；随后用户在本聊天分别明确授权管理员只读预检和一次最长 15 秒实采。
管理员预检通过；tap 在指定 audiodg 中成功挂接，结束后原始字节恢复，采集器退出，
audiodg 保持同一 PID/创建时间。实际观测为 0 个完整 MAT 周期，不能宣称采到了
MAT 数据或完成真实 MAT 串流。只读端点检查显示默认输出为 Steam Streaming
Speakers，活动列表为 Steam/虚拟线/UUYC/Realtek 扬声器，未显示 HDMI 输出。

本轮授权已用完，不自动再次注入/重启引擎/切换端点。tap 模块按设计保持加载，
hook 已解除；没有持久化音频载荷或修改端点/驱动/服务配置。完整证据边界见
`spatial-object-spike/mat-live/MAT-LIVE-RESULT.txt`。原生 MAT 数据、游戏音视频同步、
HDMI/eARC 真机与 Atmos 识别仍待在可产生 MAT 的输出链路上验收。

以下为之前阶段的历史记录，不代表当前正常路径仍使用有限样本回放。

---

# Atmos / MAT 换机交接 — 2026-10-22

这是研究分支的进度和复现说明，不是自动部署或提权授权。以本文件为当前
入口；2026-10-18 的 JOC 交接包属于旧路线，不能代替当前 MAT 状态。

## 2026-10-22 最新范围：能力协商与不支持设备回退

用户已明确：不支持 MAT 的手机无需还原 MAT，手机对象渲染实验已停放在独立
本地分支。当前方向为主机 MAT 采集/串流到支持设备直通，先完成会话前的能力
检查和普通音频回退。新的验证入口见
[MAT capability selection](../spatial-object-spike/mat-capabilities/README.md)。

正常设置改为“优先 MAT”：本机输出不满足条件，或主机 DESCRIBE 不提供兼容
播放 profile 时，在 ANNOUNCE 前选择 Opus。共享库的本地偏好值 6 不上线路；
最终仍为原 MAT codec 5 或 Opus codec 0。严格诊断 codec 5 和落盘 codec 4
保持原语义。已经选定 MAT 后确认不匹配、路由丢失/变化均终止，重连后重新协商。

当前主机 profile 仍为有限样本回放，不表示已完成无限实时捕获、手机 MAT 解码、
HDMI/eARC 真机播放或 Atmos 点亮。下节手机发声属于历史固定样本研究证据。

## 2026-10-22 早期本机续作：手机扬声器试听

用户已明确将本轮验证目标改为手机内置扬声器发声。原始 HDMI/eARC 路线和
历史验收记录保留在下文；续作报告见
`spatial-object-spike/mat-android-offline/MAT-PHONE-RESULT.txt`，复现入口见同目录
`README.md`。

- 完整 Moonlight debug APK/native application 构建已通过，包含 arm64-v8a 和
  armeabi-v7a 的完整 JNI 库；使用独立测试包名，尚未做完整客户端真机网络验收。
- 魅族 17 / Android 13 的真实 ART/JNI/player 检查通过；MAT、TrueHD、E-AC-3/
  JOC、AC-4 MediaCodec 创建均不可用，当前 IEC bitstream direct support 为 0。
- 新增**仅限固定 SHA 样本**的研究性 PCM 提取及显式扬声器试听。Java 提取与
  既有 Python 研究解析器逐字节一致；保留三周期 bootstrap 时长为静音，31 个
  fullband slot 合成单声道并复制到双声道。试听归一化使用独立数组和独立哈希。
- 3.56 秒 / 170,880 帧实际提交到内置扬声器并 drain；修正过低试听增益后，
  用户在本聊天明确确认听到测试音。手机系统媒体音量未由脚本修改。

这只补齐固定样本的本地发声实验，不是通用 MAT 解码器、Atmos 对象/坐标渲染、
完整网络客户端播放或无限实时捕获。旧的“尚未构建完整 APK”描述属于初次交接
时的历史状态，不能覆盖本节的新构建结果；HDMI/eARC 和真实 Atmos 点亮仍未验证。

## 三个关联仓库

| 仓库 | 分支 | PR base |
| --- | --- | --- |
| AlkaidLab/foundation-sunshine | codex/atmos-mat-handoff | master |
| qiin2333/moonlight-common-c | codex/atmos-mat-transport | **mic** |
| qiin2333/moonlight-vplus | codex/atmos-mat-passthrough | master |

两个父仓库必须引用同一个 common-c 提交；先审 shared PR，再处理父仓库。
共享 PR：[moonlight-common-c #40](https://github.com/qiin2333/moonlight-common-c/pull/40)，
安卓 PR：[moonlight-vplus #750](https://github.com/qiin2333/moonlight-vplus/pull/750)。
主机 PR：[foundation-sunshine #1177](https://github.com/AlkaidLab/foundation-sunshine/pull/1177)。
两个父仓库使用的 shared commit 为 `773bebded05624e275106ce1fa4bb9aa32726535`。
common-c 改了 STREAM_CONFIGURATION/JNI 接口，必须一起重新编译，不能混用旧库。
已按用户要求合并主机 master `c211d711`、共享库 mic
`2aa14ffeb877f9093993d9c9cf3c3440e2d2220d`，未执行 PR 合并。
保留上游新版 RTSP 解析器/大小写规则/重复节点释放，将 MAT duplicate marker
适配到 replacement node；不能回退旧 caller-side free。主机保留上游 NVIDIA
vsync lease reset、RC 构建策略和新 PyroWave/Granite，MAT 初始化同时保留。
新增 `RtspParserMatTest.c` 防止相同/大小写混合/三重 ACK 被 coalesce 后漏检，
同时验证上游空白字符与含 NUL 二进制 payload 行为。

## 当前结论与边界

当前目标是 PC 对象音频+坐标 -> MAT -> Sunshine -> 安卓 TV/盒子 -> HDMI/eARC
-> Atmos Soundbar。**不是无 HDMI 的手机本机 Atmos 解码路线。**

- M1/M2/M3：`src/spatial_audio/` 有源接口、捕获 reader、坐标状态 broker、
  有界桥接和独立编码 worker；仍为 opt-in 模块，未接默认实时会话。
  新 stream DLL 做过构建及自有 fake 测试，不等于已完成 audiodg 原生常驻验收。
- M4 / A/B/C：已验证的 **3.56 秒有限 MAT 录制**接正式 RTSP/RTP/FEC，
  codec 4 重组落盘，codec 5 独立 IEC61937 播放路径；C 的最后硬件 sink 是 fake。
- 已有对象采集/原生编码实验源码和阶段报告保留在 `spatial-object-spike/`。
  原生 Windows 私有 ABI 的成功只适用于原机版本，不是跨机兼容保证。
- 尚未验证：完整 Moonlight APK/native application 链接安装、Android AudioTrack/
  HAL、TV 内部应用到 eARC 的实际路由、Soundbar 接受、Atmos 点亮、设备延迟；
  无限实时源协议、生产采集器生命周期、丢包/路由切换恢复也未完成。

普通 FFmpeg E-AC3 编解码不等于 JOC 对象编码。Windows 对象 MAT 录制也不能仅凭
TrueHD-in-MAT 或 DD+ Atmos 的支持说明就认定设备可用；必须测真实载荷。
本工作没有提供或重新分发 Dolby 编码器，不代表认证或商用授权。

## 固定数据和协议：不要改错

- 私下传输的源文件放回：
  `spatial-object-spike/mat-spike/mat-native-session-run02/capture/encoder-payload.raw`
- 文件 **10,936,320 bytes / 178 cycles / 3.56 s**，SHA256：
  `79cd69e33adcd1bcffe95673564b6fc727a7aaed7370745d43ec269925a492f6`。
- 一个完整载波周期 61,440 B = 30,720 个 16-bit 字 = 3,840 个物理帧，
  192 kHz / 8ch / s16le，20 ms；SMAT 逻辑 PTS 为 48 kHz，每周期 960 ticks。
  原生 Process 的 **30,720 B** 块不等于一个完整周期；RTP/FEC tick=1 不是 PTS。
- codec 4 `mat-dump-v1` 只落盘；codec 5 `mat-iec61937-v1` 只显式协商的有限播放。
  两者不能互相静默回退，也不能回退 Opus/PCM 或走 16 KB AC3 callback。
- 完整周期原字节不再封装、不再次 byteswap、不进入 Kodi 的 TrueHD MAT packer。
  有界队列满/丢包/序号错/不完整周期须失败，不做 PLC 或静默丢帧。
- 流式 SHA/count 在 EOS 检查后才请求 drain；这不意味着播放前预读验证整段。
  FNV 和现有 AES-CBC 不提供音频真实性认证。

## 本机已有验收（历史记录，不冒充新机重测）

主要报告：

- `spatial-object-spike/mat-android-session/MAT-ANDROID-C-RESULT.txt`
- `spatial-object-spike/mat-android-offline/MAT-ANDROID-OFFLINE-RESULT.txt`
- `spatial-object-spike/mat-rtsp-live/MAT-RTSP-LIVE-RESULT.txt`
- `spatial-object-spike/mat-spike/SPATIAL-BROKER-M1-M3-RESULT.txt`
- `src/spatial_audio/README.md`（M1–M3 特有限制）

真实 Sunshine + 配对隔离 NVHTTP + production LiStartConnection/RTSP/RTP/FEC，
plain / AES audio / encrypted RTSP+AES 三组；实际生产 JNI 和 Java session/player，
仅最终硬件替换 fake，均 178 周期且 SHA 一致，decodeCalls=0，无落盘副产物。
codec 4 三组落盘回归也一致。15 个负协商场景在 PLAY/载波输入前拒绝；
另有 12 组 JNI/player、6 组 playback 生命周期、6 组原 dump 生命周期、两份
common-c 各 9 个 CTest。完整 app Kotlin/Java 编译及 NDK MAT 模块链接通过，
但不等于完整 APK/native application 链接通过。

原始 `C-PROOF.json`、run 输出和二进制不放公开 Git。必要证明随私下交接包保留。
以前的 proof 仍描述当时的源文件哈希，不能重写成上游合并后的验收证明。

首次 PR 整理曾另行重编译 common-c/JNI/Java 会话工具及 6 组 playback 生命周期，
15 组负协商全部通过；两份已有 CTest 构建各 9 项复跑通过。未重跑完整三模式
正向会话、完整 APK 或真机验收。

### 上游合并后的本轮验收（2026-10-22）

报告：`spatial-object-spike/mat-android-session/MAT-UPSTREAM-RESULT.txt`。
最终新证据 `build/mat-upstream-final/proof.json`，不是旧 C-PROOF 的改写。
已重新构建正常主机，再完整执行 verify.ps1 编排（`-SkipHostBuild` 仅跳过刚完成的主机构建）：
三种加密组合的 codec5 播放与 codec4 落盘共六次真实隔离会话，全部 178 周期/
10,936,320B/SHA 一致；播放最终硬件仍为 fake。两台测试主机均正常退出。
两份 common-c 各 10/10 CTest，12 组 JNI/player、15 组负协商、6 组播放生命周期、
6 组落盘生命周期及其他既有 profile/manifest 回归全部通过；Android NDK/JNI
编译与诊断 APK 构建通过。完整 app Kotlin/Java 编译、上游 CMake 7 项回归和
M1–M3 自有进程 fake 回归也通过；完整 Moonlight native application/APK 仍未验证。

本轮主机 binary SHA256：`03ee0e2743b44dba586f712cdf2ecb58f3666b9b52025b326a1805d9a3bf9c7c`。
详细源版本、源文件和构建产物哈希在新 proof 中；随后的纯交接文档提交不冒充重建。
`supplemental.json` 收录额外测试的证据与日志哈希。转移包的
`evidence/upstream-20261022/` 为本轮记录，原路径的 C-PROOF 保留历史含义。
M1–M3 全量 fake 回放还需私下保留的对象/调用计划 oracle，默认 MAT 转移包不含
这些原始捕获；不要误以为仅 3.56 秒 MAT 即可复现所有历史原生桥接实验。

远端 CI 首次暴露的 Linux indentation warning 和 MSVC CRT warning 已修复：
不降低警告级别，拆开两处压缩语句；有界路径复制用 memcpy；恢复 Platform.h
先于 MAT 标准库头文件包含的顺序。最终共享库另经本机 GCC -Wall/-Wextra/-Werror
与 MSVC /W3 /WX 严格编译，修正后再次完整跑无真机编排并冻结最终 proof。
较早的 `mat-upstream-full` 及转移包仅为中间快照，使用 `mat-upstream-final` 和
最终新版交接包。远端 CI 与本地验收结果分开记录，不把 pending 当通过。

当前机可运行 `./tools/mat-handoff/make-bundle.ps1 -Output <全新目录> -Verification <新验收目录>`，生成仅含
固定录制、本文、历史 proof 和新验收摘要的本地 ZIP；不上传、不含密钥/证书/运行状态/
二进制/工具链。新机在完成 clone 后解压到仓库根目录，核对 BUNDLE-MANIFEST.json。

## 新电脑检出

建议路径无空格，例如 `C:/AtmosMAT`。使用三个 PR 的 head，而非只 clone master：

```powershell
git clone --branch codex/atmos-mat-handoff https://github.com/AlkaidLab/foundation-sunshine.git C:/AtmosMAT
cd C:/AtmosMAT
git submodule update --init --recursive
git clone --branch codex/atmos-mat-passthrough https://github.com/qiin2333/moonlight-vplus.git spatial-object-spike/moonlight-vplus
git -C spatial-object-spike/moonlight-vplus submodule update --init --recursive
git submodule status third-party/moonlight-common-c third-party/pyrowave
git -C spatial-object-spike/moonlight-vplus submodule status app/src/main/jni/moonlight-core/moonlight-common-c
```

不要用 `--remote` 更新已锁定 gitlink。上游合并后的 PyroWave 版本
`cc91cbf6363721d19d135d05ca8e3d8665ea6afd`；其 Granite
`fd1c9f6652e708fdc52773883c60b58ee3f80855`。build-host 同时检查两个层级的 gitlink。
旧 proof 的 PyroWave `019ca97d` / Granite `fb178c80` 是历史版本，不能混用做新验收。
客户端不嵌入主机 Git 树，须单独 clone。
录制样本从私下交接包恢复后先 `Get-FileHash -Algorithm SHA256`，不符就停。

## 工具与复现：现有脚本不是新机一键安装器

Windows x64，MSYS2 UCRT64 GCC/CMake/Ninja/OpenSSL，JDK 22，Python 3.11 + cryptography，
Android SDK platform/build-tools 36、NDK 28.2.13676358；host 还需现有 FFmpeg/Boost、
C++/WinRT 2.0.250303.1、MSVC/Windows SDK（仅原生模块）和项目构建依赖。

先审阅脚本并按新机安装位置调整路径；若依赖未缓存，按项目正常构建流程准备。
不能复制 CMakeCache/compile_commands 中旧机绝对路径当作新机配置。尤其：

- `mat-rtsp-live/build-host.ps1` 仍引用原机 `build/codex/_deps/boost-src`、
  `build/miniupnp-current/install`、`build/dlssnr-toolchain` 和预建 GUI assets；
  `mat-rtsp-spike/build-tests.ps1` 复用 `build/dlssnr-host/compile_commands.json`。
  新机须先配置相应构建或调整到等价的新机目录，这些缓存不随 PR/交接包提供。
- 脚本多处默认 `C:/msys64/ucrt64`、JDK/Python/SDK 路径；部分可传参，部分需调整。
  windres adapter 仅处理一个已生成 windows.rc，要求现有无空格路径/8.3 alias，
  不改系统短文件名策略。优先无空格检出路径。
- M1–M3/原生构建脚本的 MSVC 14.39、SDK 22621 路径也需适配。构建成功不解决 ABI 锁。

准备好后从 Sunshine 根目录使用**全新**输出目录：

```powershell
./spatial-object-spike/mat-android-session/verify.ps1 -Output ./build/newpc-mat-verify -Python C:/Python311/python.exe
```

本轮已完整执行编排，主机构建先单独完成。新机仍建议先分层 build/unit、
负协商、dump，再隔离会话；任何失败保留结果，不删除旧证据。`-SkipHostBuild`
仅供已明确匹配当前源码的主机构建。Java/Kotlin 编译见安卓仓库 docs/MAT-HANDOFF.md。
完整会话脚本会生成仅限测试的配对证书/私钥和状态，**不得提交或分享这些目录**。

## 接下来按这个顺序

1. 三个分支检出、依赖对齐，先无设备回归，记录新机源版本/哈希与失败点。
2. 完整 Moonlight APK/native application 构建；连接目标 TV/盒子，记录型号、
   Android 版本、HDMI/eARC 拓扑及 Soundbar 型号；设备明确支持 MAT 后做短测。
3. 保留 dump 验收路径，先检查 178 周期输入/提交/实际 drain，再验声和 Atmos。
   capability、系统 Atmos 开关、FFmpeg 标签都不是终端点亮证据。
4. 真机通过后再设计无限流 manifest/epoch/EOS、生产采集器启动/停止、故障恢复，
   将 broker 的有界输出接正式会话；不要直接把有限录制循环播放称实时捕获。

如需恢复 audiodg 实验，必须先只读检查新机 Windows/audioeng/组件版本和权限，
按对应原生源码 guard 检查 prologue/ABI。旧采集 audioeng 锁为
10.0.26100.9711 / image 0x1ef000 / timestamp 0x0627570d；其他原生编码组件
还有独立检查。版本不符先离线适配，不能改常量硬试。没有 Home Theater 端点的
独立编码实验成功不代表默认实时采集已端点无关。
无自动 UAC、服务部署、驱动安装、信任根证书、关闭安全机制、强制重启 audiodg；
过去一次性 UAC 授权不延续到新机。先询问用户再运行任何进程内原生探针。
