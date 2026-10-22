# 升级说明

[项目首页](../../README.md) · [文档目录](README.md) · [English](../en/upgrading.md)

## 选择版本

日常使用从[最新正式发行版](https://github.com/AlkaidLab/foundation-sunshine/releases/latest)安装。排查指定问题或体验新功能时，在[发行列表](https://github.com/AlkaidLab/foundation-sunshine/releases)选择标记 **Pre-release** 的测试版，先阅读该版本说明。

首屏 Latest 徽章包含预发布版本；“下载被控端”按钮指向正式版，两者可能显示不同版本。

## 保留配置

更新前结束远程连接，记下当前版本。备份正在使用的配置目录；默认安装通常为 `%ProgramFiles%\Sunshine\config`，自定义安装或启动参数可能使用其他路径。该目录包含应用、配对与主机配置，不能只备份 `sunshine.conf`。

## 安装与检查

1. 在桌面 GUI 检查更新，或从目标发行页下载 `WindowsInstaller.exe` 安装包。
2. 结束活动连接后安装，按安装器提示保留现有配置。便携版用户按目标版本说明更新，不混用不同版本的可选组件。
3. 打开 GUI，确认版本、应用列表、配对设备和组件状态。
4. 从控制端连接 Desktop，确认画面、输入和声音，再测试此次更新涉及的功能。

版本号变化只能确认安装信息；首次实际连接也要完成。

## 更新后异常

先保存日志和复现步骤，并检查该发行版的已知问题。需要回退时保留当前配置副本，按版本说明恢复之前的安装包与匹配配置，避免新旧格式混用。反馈信息见[贡献指南](../contributing.md)。
