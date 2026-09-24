# Foundation Sunshine Game Bar 小部件

Xbox Game Bar widget,把 Foundation Sunshine 的主机侧串流状态以"串流内 UI"的形式呈现(穿出画面、
跨所有 Moonlight 客户端、手柄可导航)。父设计稿:`_dev_notes/gamebar-integration-design.md`,
原型:`_dev_notes/gamebar-widget-prototype.md/.html`。

## 状态

- **M0 工具链**(本仓库):工程可构建(见下),真机部署进 Game Bar 待验。
- **M1 静态 UI**:主视图(会话卡/FPS 曲线/管线延迟条/两段式确认动作)+ 无会话/错误态已实现;
  紧凑模式未做。
- **M2 活数据**:`/api/widget/state` 服务端已落地(见主仓库 `src/widget_http.cpp`),
  widget 轮询客户端已实现,真机联调待做。
- M3 动作:stop_session / restart_service 客户端已实现,与 M2 一起真机验证。

## 构建依赖

- Visual Studio 2022 + 「通用 Windows 平台开发」工作负载(老式 UWP 工程,`dotnet build` 不支持)
- Windows SDK 10.0.26100(或改 csproj 的 `TargetPlatformVersion` 指向已装的 SDK)
- NuGet 自动还原:`Microsoft.Gaming.XboxGameBar` 7.3.2607010、
  `Microsoft.NETCore.UniversalWindowsPlatform` 6.2.9

## 构建

```bash
MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
cd gamebar-widget/src
"$MSBUILD" FoundationSunshineWidget.csproj -t:Restore -p:Configuration=Debug -p:Platform=x64
"$MSBUILD" FoundationSunshineWidget.csproj -p:Configuration=Debug -p:Platform=x64 -p:AppxPackageSigningEnabled=false
```

## 部署到真机(M0 验证步骤)

1. Windows 设置 → 系统 → 开发者选项 → 打开「开发人员模式」;
2. 服务端配置:sunshine.conf 加 `widget_token = <随机串>` 后重启 Foundation Sunshine
   (不配置则 `/api/widget/*` 返回 404,小部件不可用);
3. 生成并安装 MSIX(未签名包用开发者模式安装):
   ```powershell
   # VS 里 F5,或(仓库根目录起):
   Add-AppxPackage -Register gamebar-widget\src\bin\x64\Debug\AppxManifest.xml   # 免打包注册
   ```
4. **回环豁免**(packaged app 访问 localhost 的前提,只需一次):
   ```powershell
   CheckNetIsolation LoopbackExempt -a -n="AlkaidLab.FoundationSunshineWidget_<发布哈希>"
   # 查发布哈希:Get-AppxPackage FoundationSunshineWidget | select PackageFamilyName
   ```
5. 按 Win+G → 小组件列表 → 添加「Foundation Sunshine 串流仪表盘」;
6. 无会话态里填端口(默认 47990)与 widget_token → 保存并连接。

## 已知边界(spike 期)

- 图标是生成的占位 PNG(深底 + Sunshine 蓝条);
- token 为手动配对(小部件内 PasswordBox,存于系统 Credential Locker/PasswordVault,不回显,可清除),产品化配对流程见设计稿开放问题 1;
- 客户端报告丢包字段服务端尚未暴露(客户端 IDX_LOSS_STATS 只进日志),NetText 暂不显示;
- 紧凑模式(Game Bar pinned 小尺寸)未实现。
