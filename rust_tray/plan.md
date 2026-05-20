# 系统托盘替换方案（已更新：大部分逻辑迁移至 Rust 库）

要点结论：
- 大部分托盘逻辑、i18n 与菜单处理已迁移到 Rust 库，主实现见 [`rust_tray/src/lib.rs`](rust_tray/src/lib.rs:1)。
- 对外 C API 以扩展接口为主：请使用 [`tray_init_ex`、`tray_loop`、`tray_exit` 等](rust_tray/include/rust_tray.h:61)；旧 `tray_init` 为遗留且不推荐使用。
- C++ 端使用薄包装器 [`src/system_tray_rust.cpp`](src/system_tray_rust.cpp:1) 与 Rust 库交互，CMake 已改为始终链接 Rust 实现。

关键文件（快速索引）：
- Rust 实现：[`rust_tray/src/lib.rs`](rust_tray/src/lib.rs:1)
- 国际化：[`rust_tray/src/i18n.rs`](rust_tray/src/i18n.rs:1)
- 菜单/动作：[`rust_tray/src/actions.rs`](rust_tray/src/actions.rs:1)
- C 头（导出 API）：[`rust_tray/include/rust_tray.h`](rust_tray/include/rust_tray.h:1)
- C++ 包装器：[`src/system_tray_rust.cpp`](src/system_tray_rust.cpp:1)
- CMake 目标：[`cmake/targets/rust_tray.cmake`](cmake/targets/rust_tray.cmake:1)

架构要点：
1. Rust 负责：菜单结构、i18n、事件循环、图标/通知、动作映射（MenuAction -> TrayAction）。
2. C++ 负责：应用内响应（打开 UI、重启、退出等）和平台特殊处理（如 Windows 特权/进程管理）。
3. 边界：Rust 通过 C API 导出简单函数；C++ 通过回调接收用户操作事件。

构建与集成：
- CMake 现在包含并构建 `rust_tray`，使用 `cargo build` 生成静态库并链接到主程序（见 [`cmake/compile_definitions/*`](cmake/compile_definitions/common.cmake:1) 的改动）。
- 头文件为 [`rust_tray/include/rust_tray.h`](rust_tray/include/rust_tray.h:1)，C++ 仅需包含该头并注册回调。
- CI：需保证 Rust toolchain 可用；建议在 CI 中添加 Rust 安装步骤。

运行时与 API 变化：
- 初始化：推荐使用 `tray_init_ex(icon_normal, icon_playing, icon_pausing, icon_locked, tooltip, locale, callback)`。
- 事件循环：使用 `tray_loop(blocking)` 驱动；返回 -1 表示要求退出。可在单线程或分线程中调用（包装器提供线程化入口）。
- 运行时更新：`tray_set_icon`、`tray_set_tooltip`、`tray_set_vdd_checked`、`tray_set_vdd_enabled`、`tray_set_locale`、`tray_show_notification`。
- 兼容层：实现了 `tray_update`（部分支持）；但 `tray_init` 已被降级（返回错误并打印警告）。

i18n 与菜单：
- i18n 数据与逻辑在 Rust 层管理，参见 [`rust_tray/src/i18n.rs`](rust_tray/src/i18n.rs:1)。
- 语言切换由 Rust 处理并原子更新菜单文本，必要时会重设 TrayIcon 的菜单以确保生效（Windows 行为）。

图标与通知：
- 图标加载：Windows 优先使用 .ico（多分辨率），Linux 支持图标名称或文件路径，macOS 使用文件路径。
- 通知：当前为占位实现（日志输出），需要按平台补全真实通知接口（待办）。

测试清单（必验）：
- 编译通过并链接 Rust 静态库。
- 托盘图标在 Windows / Linux / macOS 显示正确。
- 菜单项触发后，C++ 回调收到匹配的 `TrayAction`（见头文件枚举）。
- 语言切换后菜单文本更新并在 UI 上可见。
- 图标切换与 tooltip 更新正常。
- 通知调用至少不会崩溃（后续完善行为）。

已知限制与后续工作：
- 完成平台通知实现（Rust 层需要具体实现）。
- 若需更细粒度的日志或错误上报，考虑在 Rust 层引入更丰富的日志接口并暴露给 C++。
- 可选：清理 `third-party/tray` 中多余源文件，仅保留头文件以减小仓库体积。
- 在 CI 中加入交叉编译与多平台验证。

迁移结论：
本次提交把「菜单、i18n、事件循环、图标管理、部分文件对话（导入/导出）」这些横跨平台且逻辑密集的功能迁移到 Rust，提高了可维护性与一致性。C++ 侧保留平台特性与应用逻辑，双方通过稳定的 C API 协作。

参考实现与调试入口：
- 查看实现：[`rust_tray/src/lib.rs`](rust_tray/src/lib.rs:1)
- 头文件：[`rust_tray/include/rust_tray.h`](rust_tray/include/rust_tray.h:1)
- C++ 包装示例：[`src/system_tray_rust.cpp`](src/system_tray_rust.cpp:1)
- i18n 数据：[`rust_tray/src/i18n.rs`](rust_tray/src/i18n.rs:1)

完成。