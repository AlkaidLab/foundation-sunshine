# Atmos / MAT 换机交接 — 2026-10-22

这是研究分支的进度和复现说明，不是自动部署或提权授权。以本文件为当前
入口；2026-10-18 的 JOC 交接包属于旧路线，不能代替当前 MAT 状态。

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
两者父仓库使用的 shared commit 为 `0d5b78271e7ee5c7bb3af351724e5fd9e16a20ba`。
common-c 改了 STREAM_CONFIGURATION/JNI 接口，必须一起重新编译，不能混用旧库。
分支基于原已验证快照，未为本次交接擅自合并最新上游，也不自动合并 PR。
检查 PR 冲突并在集成后重跑测试，不能把旧快照的通过记录当新提交的验收。

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
本次提交只对注释/格式、交接文档和 build-host 的 gitlink 检查做整理；以前的
proof 仍描述当时的源文件哈希，不能重写成当前提交的全量重新测试证明。

本次 PR 整理另行重编译 common-c/JNI/Java 会话工具及 6 组 playback 生命周期，
15 组负协商全部通过；两份已有 CTest 构建各 9 项复跑通过。未重跑完整三模式
正向会话、完整 APK 或真机验收。

当前机可运行 `./tools/mat-handoff/make-bundle.ps1 -Output <全新目录>`，生成仅含
固定录制、本文和历史 proof 摘要的本地 ZIP；不上传、不含密钥/证书/运行状态/
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

不要用 `--remote` 更新已锁定 gitlink。PyroWave 验证版本
`019ca97d69ded38bdd58615e8b4c909294fc3757`；其 Granite
`fb178c8080d163419e8d20f10715c61c53c1ec9b`。客户端不嵌入主机 Git 树，须单独 clone。
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

该脚本编排此前逐项运行的验收；原机未将整个编排一次性跑完。先分层 build/unit、
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
