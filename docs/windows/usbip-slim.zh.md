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
| RegisterDetachTask | Task Scheduler 2.0 注册同名 SYSTEM 按需任务及原始 detach 参数 |
| PrepareToInstall 卸载旧包 | 查固定 AppId 的 64/32 位卸载项，运行原 uninstaller；注册仍在则停止 |
| devnode remove | 调原版 devnode remove，失败仍继续其他清理并返回失败 |
| DeleteOemDriver 双查找 | DISM Get-WindowsDriver 的结构化包信息匹配两个原始 INF，再由 pnputil 删除 |
| UnregisterDetachTask、移除文件和卸载项 | 删除任务、驱动包、自己的 receipt 和所列文件；保留不相关文件 |
| AlwaysRestart / UninstallNeedRestart | 旧版接管及系统返回 3010 时报告需重启，不自动重启 |
| GUI、SDK、PDB、桌面图标、测试证书 | 有意不分发、不创建、不导入 |

上游 tag 的 setup.iss 与安装器内重建的 [Run] 同时核对；本安装器实际包含 devnode
安装/卸载步骤，不能只从某一份源码推断。旧包卸载和新包安装均在 guard 的快照与验证
之间。失败的清理继续完成其余步骤，保留必要恢复文件；根集线器恢复不完整返回 103，
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
usbip-win2 的 BSD-2-Clause 原文统一放入 `third_party/licenses/usbip-win2/LICENSE`，
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
