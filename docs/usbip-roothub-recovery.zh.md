# USB/IP 安装后 USB 键鼠失灵：恢复步骤

适用情形：安装 USB/IP（usbip-win2）之后，设备管理器里所有「USB Root Hub (USB 3.0)」带黄色感叹号（代码 19），USB 键盘鼠标都不能用。这是 [#1272](https://github.com/AlkaidLab/foundation-sunshine/issues/1272) 的症状。

## 原因

usbip-win2 的 `usbip2_filter` 是它虚拟控制器驱动的配套筛选驱动。它的扩展 INF 匹配 `USB\ROOT_HUB30`，**所有**实体 USB 3 根集线器都有这个硬件 ID，所以安装和卸载都会重启全部根集线器；上游 issue [#201](https://github.com/vadimgrn/usbip-win2/issues/201) 里还记录了筛选驱动名残留在注册表里、只删驱动包不够的情形。哪一步让根集线器变成代码 19，微软文档没有写明，上游也还没有结论。

新版安装器默认不勾选 USB/IP；勾选后，安装前后各检查一次实体根集线器，出现新的问题就自动撤销，并在完成页提示。以下步骤用于装过旧版、或自动撤销没能恢复的电脑。

## 先判断你能不能进系统操作

| 你手上有 | 用哪一节 |
| --- | --- |
| 另一台电脑上的远程控制（向日葵、RDP 等）已经连着本机 | 第 1 节 |
| 笔记本自带键盘和触控板（多数不挂在 USB 上）、PS/2 键盘、或触屏 | 第 2 节 |
| 都没有，或系统起不来 | 第 3 节（Windows 恢复环境） |

## 1. 有远程控制：删除驱动包并重启

以管理员身份打开 PowerShell：

```powershell
# 找到 usbip2_filter 对应的 oemNN.inf 并删除
Get-ChildItem 'HKLM:\SYSTEM\DriverDatabase\DriverPackages' |
  Where-Object { $_.PSChildName -like 'usbip2_filter.inf_*' } |
  ForEach-Object { $oem = $_.GetValue(''); pnputil /delete-driver $oem /uninstall /force }
```

然后清理残留并检查（脚本在安装目录的 `scripts` 里，路径按实际安装位置改）：

```powershell
$g = 'C:\Program Files\Sunshine\scripts\usbip-roothub-guard.ps1'
powershell -NoProfile -ExecutionPolicy Bypass -File $g -Action cleanup
powershell -NoProfile -ExecutionPolicy Bypass -File $g -Action status
```

`status` 里「persisted filter names」为 0 后重启电脑。上游自己的清理命令也可以用：

```bat
FOR /f %P IN ('findstr /M /L /Q:u "usbip2_filter usbip2_ude" C:\WINDOWS\INF\oem*.inf') DO pnputil.exe /delete-driver %~nxP /uninstall
```

提出 issue 的用户就是这样恢复的：远程连入，`pnputil /delete-driver oem77.inf /uninstall`，重启，6 个根集线器恢复。

## 2. 键鼠不能用，但屏幕或触控可用

1. 用笔记本自带键盘、PS/2 键盘或触屏进入系统，再按第 1 节操作。
2. 安全模式不会加载不在 SafeBoot 列表里的驱动，`usbip2_filter` 不在其中，所以安全模式里键盘很可能恢复。这一点我们没有在出故障的机器上验证过。

## 3. 进不了系统：Windows 恢复环境（WinRE）

恢复环境用的是它自己的 USB 驱动，不带 `usbip2_filter`，USB 键鼠通常可用（我们没有在出故障的机器上验证过）。进入方法：开机出现 Windows 标志时按电源键强制关机，连续三次，第三次会进入自动修复；选「高级选项 → 疑难解答 → 高级选项 → 命令提示符」。

1. 找到系统盘符（恢复环境里经常不是 `C:`）：`dir C:\Windows` 不存在就换 `D:`、`E:`。下文用 `C:`。
2. 删除驱动包。先列出：`dism /image:C:\ /get-drivers /format:table`，找出「原始文件名」是 `usbip2_filter.inf` 和 `usbip2_ude.inf` 的 `oemNN.inf`，逐个删除：`dism /image:C:\ /remove-driver /driver:oemNN.inf`。
3. **只删驱动包不够**（上游 issue [#201](https://github.com/vadimgrn/usbip-win2/issues/201) 的用户就是这样仍然启动失败）。还要清掉注册表里留下的筛选驱动名：
   1. 运行 `regedit`，选中 `HKEY_LOCAL_MACHINE`，菜单「文件 → 加载配置单元」，选 `C:\Windows\System32\config\SYSTEM`，名称填 `OFFSYS`。
   2. **先确定离线系统实际使用的控制集。** 展开 `OFFSYS\Select`，读 `Current` 和 `Default` 两个值（DWORD：1 对应 `ControlSet001`，2 对应 `ControlSet002`，依此类推）。下次启动用的是 `Default` 指向的控制集，所以必须清理它；`Current` 是上次启动用过的，如果它和 `Default` 不一样，两个都要清理。下文用 `ControlSet00N` 代表要清理的那个（微软的离线恢复步骤同样按 `Select\Default` 选控制集：[Troubleshoot INACCESSIBLE_BOOT_DEVICE](https://learn.microsoft.com/en-us/windows/client-management/troubleshoot-inaccessible-boot-device)）。
   3. 展开 `OFFSYS\ControlSet00N\Enum\USB\ROOT_HUB30\`，对其下每个实例：
      - 如果有 `Filters` 子项，打开其中名为 `*Upper` 的值，删去 `usbip2_filter` 这一行（保留其他行）；值里没有别的内容时删掉这个值。
      - 展开 `Properties\{a193307d-1245-46d3-be67-d8128670e09b}\0002`，打开「（默认）」值，数据形如 `*Upper` 换行 `usbip2_filter`，**只删 `usbip2_filter` 这一项**，保留 `*Upper`，值类型保持不变。
   4. 若 `Current` 与 `Default` 不同，对另一个控制集重复上一步。
   5. 选中 `OFFSYS`，菜单「文件 → 卸载配置单元」。
4. 关闭命令提示符，选「继续」重启。

## 恢复之后

- 设备管理器里 6 个根集线器应为正常。不要马上重新安装 USB/IP。
- 需要原生 DualSense 时，在新版安装器的「自定义功能」里勾选 USB/IP，或在管理界面的设备中心确认安装。装之前请确认有一条不依赖 USB 的远程控制通道。
- 若 USB/IP 又让根集线器出问题，附上 `scripts\usbip-roothub-guard.log` 提交到 [#1272](https://github.com/AlkaidLab/foundation-sunshine/issues/1272)。

## 资料

`usbip-win2-filter-readme-20261115`、`usbip-win2-filter-inf-20261115`、`usbip-win2-issue-201-20261115`、`ms-extension-inf-20261115`、`ms-device-manager-error-messages-20261112`、`ms-pnputil-command-syntax-20261112`（见 `docs/authority/source-index.tsv`）。
