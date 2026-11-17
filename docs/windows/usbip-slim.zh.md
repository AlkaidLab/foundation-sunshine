# USB/IP 签名包原样分发

本方案固定 usbip-win2 v.0.9.8.1 的 x64 安装器和 SHA-256，使用仅构建时运行的
Inno Unpacker 2.71.3（ui_2_2_13 发布包）提取。旧 innoextract 1.9 和其当前主线最多支持
Inno 6.3.3，不能读本包的 Inno 7.0.0.3。无需执行上游安装器，也不重新编译或签名驱动。

驱动包的全部六个文件（两个 INF、两个 SYS、两个 CAT）逐字节保留，清单见
`scripts/packaging/usbip-payload.json`。Windows 构建必须用 SDK SignTool 验证两个
catalog 的 `/kp` 策略及 `/kp /c` 下四个 INF/SYS 的成员关系；任何失败均停止打包。
这比仅检查 CAT 的 Authenticode 更完整。原始 INF 的广泛匹配保持不变，外层根集线器
guard 继续检查并清理异常状态，绝不通过修改 INF 缩小匹配。

## 上游安装行为对应

| 上游 setup.iss 步骤 | 对应实现 |
| --- | --- |
| main：usbip.exe、devnode.exe、所有 DLL | 原样复制四个工具文件至共享 USBip 目录 |
| client：package 全部文件 | 原样复制至 driver 子目录，SHA-256 固定 |
| VC++ 安装程序 | 有意不分发、不执行；从同一个固定安装器的运行库内容提取 app-local DLL |
| pnputil 安装 usbip2_filter.inf | 系统 pnputil `/add-driver … /install` |
| devnode install UDE INF 和 ROOT\\USBIP_WIN2\\UDE | 先由 pnputil 暂存 UDE INF，再用原版 devnode 注册根设备 |
| AddService、AddReg、AddFilter | 全部交给原样签名 INF，脚本不重写服务或驱动注册表 |
| RegisterDetachTask | Task Scheduler 2.0 注册同名 LocalService 任务，保留关机/重启事件触发及原始 detach 参数 |
| PrepareToInstall 卸载旧包 | 先只读判断归属；仅确认自有旧包后使用固定 AppId 原 uninstaller，外部安装原样保留 |
| devnode remove | 调原版 devnode remove，失败仍继续其他清理并返回失败 |
| DeleteOemDriver 双查找 | DISM Get-WindowsDriver 的结构化包信息匹配两个原始 INF，再由 pnputil 删除 |
| UnregisterDetachTask、移除文件和卸载项 | 删除任务、驱动包、自己的 receipt 和所列文件；保留不相关文件 |
| AlwaysRestart / UninstallNeedRestart | 旧版接管及系统返回 3010 时报告需重启，不自动重启 |
| GUI、SDK、PDB、桌面图标、测试证书 | 有意不分发、不创建、不导入 |

上游 tag 的 setup.iss 与安装器内重建的 [Run] 同时核对；本安装器实际包含 devnode
安装/卸载步骤，不能只从某一份源码推断。自有旧包卸载和新包安装均在 guard 的快照与验证
之间；外部安装在 guard、状态文件、互斥量和复制文件之前返回。失败的清理继续完成其余步骤，保留必要恢复文件；根集线器恢复不完整返回 103，
不能宣称回滚成功。互斥量防止两个顶层生命周期操作并发，内部回滚不重复取锁。

## 运行库与许可证

`objdump -p` 检查两个 EXE、libusbip.dll、resources.dll 及运行库自身的导入表：
完整非系统依赖闭包为 MSVCP140.dll、MSVCP140_ATOMIC_WAIT.dll、VCRUNTIME140.dll、
VCRUNTIME140_1.dll。其余依赖为系统 DLL 与 Windows 10 内置 UCRT API 集。
固定运行库为 14.51.36247；不是任意构建机上较旧的 VC 安装。构建提取固定 Burn CAB
字节区间，交给 Windows 自带 expand 解包，再逐文件核对固定哈希与微软签名。
不执行运行库安装程序，不分发 MSI、CAB 或安装 EXE。上游原有动态链接保持不变。

微软文档允许 app-local，但因维护成本推荐集中安装。本方案按包体积要求选择允许的
app-local 方式，并负责运行库版本维护；不采用已弃用的 merge modules，也不改成静态链接。
再分发以拥有有效 Visual Studio 使用许可并遵守其软件许可条款为前提，本文不宣称替代
该许可。来源记录 usbip-slim-ms-vc-redist-20261117 与 usbip-slim-ms-redist2026-20261117（2022 清单亦作交叉核对）。
usbip-slim-license-20261117 记录的 usbip-win2 BSD-2-Clause 原文统一放入 `third_party/licenses/usbip-win2/LICENSE`，
在 payload 中保留副本；与第三方声明任务采用相同目录。

## 探测、性能与验证边界

共享工具位置继续使用原标准目录，主机 doctor 已从工具文件及 UDE/VHCI 服务探测，
不依赖上游卸载项，因此两种安装均兼容。新的安装记录附加 InstallLayout、SourceSHA256、
InstallLocation 和版本；Panel 需同时识别这份 receipt 和旧包，执行时验证工具/驱动字节。
`status` 不把注册表版本当作驱动可用的证据：还检查工具完整性、唯一根设备问题码及
注册驱动哈希。

目标是解压后的核心 payload 小于 4 MiB；原安装器 26,083,632 字节，提取 payload
3,476,484 字节（含清单和许可证）。这不是最终整包大小；最终安装器和便携包须比较同一
构建配置的产物。更换打包与安装机制不改传输热路径，没有增加每帧复制、分配、系统调用
或锁；吞吐、延迟、CPU 和跨实现性能没有测量，不据此声称性能领先。

Windows 使用 PowerShell 5.1、系统 PnPUtil/DISM、原版 devnode 与 Task Scheduler，避免
额外目标机运行时；构建工具用 Python 标准库。原签名驱动要求 Windows 10 1903，不能
通过修改 INF 改变最低版本。Linux 仅作提取/隔离回归；macOS、Android、iOS、HarmonyOS
不受此 Windows 分发变更影响，没有新增 Backend 或 Adapter。USB/IP 生态兼容性没有
本卡实测，远程 USB 与原生手柄需独立验收。真实设备、资源授权、跨租户隔离均未核实。

Windows CI 验证文件和签名，不在共享 runner 安装驱动。虚拟机全新安装、旧包接管、卸载、
模拟 guard 失败回滚和真实远程 USB/原生手柄测试通过之前，PR 保持草稿，不得合并。

## 安装归属与兼容规则（2026-11-17）

| 观察结果（从上往下优先） | 归属 | 安装动作 | 卸载动作 |
| --- | --- | --- | --- |
| 检查失败、注册冲突或来源不明 | 外部管理 | 不改动，提示检查或自行选择替换 | 不改动 |
| 有 GUI 或 SDK（wusbip.exe、include 或 lib） | 外部管理 | 同版本可复用；不改动 | 不改动 |
| 版本不同或无法读取 | 外部管理 | 不改动，返回 106，提示用户自行决定 | 不改动 |
| 同版本、自有固定 receipt、来源哈希与位置匹配，且没有旧 ARP 项 | 自有瘦身版 | 校验后复用，必要时只修复自有包 | 受 guard 保护卸载自有包 |
| 同版本、无 GUI/SDK、无瘦身 receipt，主机 scripts/usbip 中仍有固定版本原安装器 | 自有旧版 | 固定 AppId 卸载器接管 | 仅处理已确认自有旧版 |
| 没有注册、工具、驱动服务或残留 | 未安装 | 安装瘦身版 | 无操作 |

旧版兼容判断是明确的历史例外；原安装器还必须匹配固定 SHA-256，文件名存在本身
不足以证明来源。普通安装 journal 的成功记录不单独作为所有权证据，因为复用外部
安装也会记录成功。没有可靠记录而又不满足上述完整例外时，一律按外部管理处理。
来源：usbip-policy-reg-20261117（ARP 值不证明安装者）、usbip-policy-read-20261117
（LiteralPath 只读检查）、usbip-policy-files-20261117（固定目录内文件检查）。

兼容范围严格为 0.9.8.1，不接受 0.9.8.0、0.9.8.2 或推断的“更新版本兼容”。这是
已固定且现有主机和面板共同使用的工具/驱动组合，而非跨版本 ABI 承诺。版本元数据
只决定能否候选复用；外部 status 继续核对 CLI、库、注册 UDE/filter 驱动的固定字节，
并检查唯一根设备的硬件 ID 和问题码。Panel 保留现有 CLI/注册驱动验证与 port 只读
探测。驱动文件版本 12.1.30.403 与发行版本 0.9.8.1 是两种不同版本号，不能相互代替。
GUI/SDK 与重复上游卸载器不会阻止兼容工具的复用，也不授权删除它们。

安装器的独立根设备 helper 同样先判定归属，外部安装的 install、repair、uninstall
不会登记、改名、禁用或移除任何节点。主机体检显示“外部管理”，面板保留就绪功能，
并隐藏托管安装和清理按钮；不兼容提示用户自行决定是否在 Windows 已安装的应用中
替换或修复。没有自动卸载外部包的入口。

夹具覆盖完整外部版、自有同版本旧包、不兼容版、瘦身版、未安装，以及包含两个
卸载器、GUI/SDK、运行中的双驱动服务和现有根节点的观察布局。CI 只用模拟注册表
和隔离文件；不安装真实驱动。不改 INF，不重新签名，不修改已有驱动字节。
只有可快照的测试系统具备后才验证全新安装、接管、真实卸载与回滚；当前共享系统
仅允许对外部安装执行无修改路径并比较前后驱动、服务、注册表和文件。远程 USB、
原生手柄和麦克风的真实设备功能仍需各自独立实测，不能用只读 port 成功替代。

此改动是安装/探测控制路径，不触及 USB/IP 数据热路径。新增元数据查询和固定哈希
读取的延迟、分配、系统调用数及相对直接调用开销未测量。当前上游完整安装仍作为
原样复用对象；没有移植其他实现，也没有吞吐竞争结论。Windows 延用 PowerShell 5.1
和原生注册表/文件查询以兼容既有最低系统要求；C++ 体检仅只读查询。其他 OS 无新增
Backend，未构建、未运行；USB/IP 跨实现兼容和潜在 Adapter 未验证。

读取当日 upstream latest 仍解析为 v.0.9.8.1（usbip-policy-release-20261117，官方发行页），
未据发行标签扩大兼容矩阵。安装归属不属于线协议算法；本卡不采纳额外协议草案或改换
驱动 API，保留当前签名包与系统支持范围。更高吞吐、跨实现设备功能和元数据探测
延迟均未测量，不以本卡的“零状态变更”正确性结果代替性能或硬件验收。
