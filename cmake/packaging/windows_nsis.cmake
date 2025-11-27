# NSIS Packaging
# see options at: https://cmake.org/cmake/help/latest/cpack_gen/nsis.html

set(CPACK_NSIS_INSTALLED_ICON_NAME "${PROJECT__DIR}\\\\${PROJECT_EXE}")

# ==============================================================================
# File Conflict Prevention - 在文件解压前停止进程
# ==============================================================================

# 策略：直接禁用 ENABLE_UNINSTALL_BEFORE_INSTALL，手动在安装过程中处理
# 这样可以避免在选择目录阶段就检查文件导致冲突

# 自动卸载功能
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL "ON")

# Windows Restart Manager 支持和高DPI位图优化
set(CPACK_NSIS_EXTRA_DEFINES "
\${CPACK_NSIS_EXTRA_DEFINES}
!define MUI_FINISHPAGE_REBOOTLATER_DEFAULT
ManifestDPIAware true
")

# Basic installer configuration
set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}\\\\sunshine.ico")
set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}\\\\sunshine.ico")

# 设置DPI感知
set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)
set(CPACK_NSIS_MUI_WELCOMEFINISHPAGE_BITMAP "${CMAKE_SOURCE_DIR}\\\\welcome.bmp")
set(CPACK_NSIS_MUI_UNWELCOMEFINISHPAGE_BITMAP "${CMAKE_SOURCE_DIR}\\\\welcome.bmp")

# 头部图像（需要150x57像素）
# set(CPACK_NSIS_MUI_HEADERIMAGE_BITMAP "${CMAKE_SOURCE_DIR}\\\\cmake\\\\packaging\\\\welcome.bmp")

# Custom branding
set(CPACK_NSIS_BRANDING_TEXT "Sunshine Foundation Game Streaming Server v${CPACK_PACKAGE_VERSION}")
set(CPACK_NSIS_BRANDING_TEXT_TRIM_POSITION "LEFT")

# ==============================================================================
# Page Customization and Enhanced User Experience
# ==============================================================================

# Custom welcome page text
set(CPACK_NSIS_WELCOME_TITLE "Welcome to Sunshine Foundation Game Streaming Server Install Wizard")
set(CPACK_NSIS_WELCOME_TITLE_3LINES "ON")

# Custom finish page configuration
set(CPACK_NSIS_FINISH_TITLE "安装完成！")
set(CPACK_NSIS_FINISH_TEXT "Sunshine Foundation Game Streaming Server 已成功安装到您的系统中。\\r\\n\\r\\n点击 '完成' 开始使用这个强大的游戏流媒体服务器。")

# ==============================================================================
# Installation Progress and User Feedback
# ==============================================================================

# Enhanced installation commands with progress feedback
SET(CPACK_NSIS_EXTRA_INSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}
        ; 确保覆盖模式仍然生效
        SetOverwrite try
        
        ; 重置文件权限
        DetailPrint '🔓 重置文件权限...'
        nsExec::ExecToLog 'icacls \\\"$INSTDIR\\\" /reset /T /C /Q >nul 2>&1'
        
        ; 清理临时文件
        DetailPrint '🧹 清理临时文件...'
        Delete '\\\"$INSTDIR\\\\*.tmp\\\"'
        Delete '\\\"$INSTDIR\\\\*.old\\\"'
        
        ; 显示安装进度信息
        DetailPrint '🎯 正在配置 Sunshine Foundation Game Streaming Server...'
        
        ; 创建桌面快捷方式（仅在非静默安装时）
        IfSilent skip_shortcuts 0
        DetailPrint '🖥️ 创建桌面快捷方式...'
        ; 使用可执行文件的内嵌图标，确保图标能正确显示
        CreateShortCut '\\\"$DESKTOP\\\\Sunshine GUI.lnk\\\"' '\\\"$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe\\\"' '' '\\\"$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe\\\"' 0
        ExecShell 'startpin' '\\\"$DESKTOP\\\\Sunshine GUI.lnk\\\"'
        skip_shortcuts:
        
        ; 系统配置
        DetailPrint '🔧 配置系统权限...'
        nsExec::ExecToLog 'icacls \\\"$INSTDIR\\\" /reset'
        
        DetailPrint '🛣️ 更新系统PATH环境变量...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\update-path.bat\\\" add'
        
        DetailPrint '📦 迁移配置文件...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\migrate-config.bat\\\"'
        
        DetailPrint '🔥 配置防火墙规则...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\add-firewall-rule.bat\\\"'
        
        DetailPrint '📺 安装虚拟显示器驱动...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\install-vdd.bat\\\"'
        
        DetailPrint '🎯 安装虚拟游戏手柄...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\install-gamepad.bat\\\"'
        
        DetailPrint '🎤 安装音频重定向组件...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\install-vsink.bat\\\"'
        
        DetailPrint '⚙️ 安装并启动系统服务...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\install-service.bat\\\"'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\autostart-service.bat\\\"'
        
        DetailPrint '✅ 安装完成！正在启动配置界面...'
        ; 安装完成后自动打开使用教程
        IfSilent skip_config_open 0
        ExecShell 'open' 'https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=DXpTjzl2kZwBjN7jlRMkRJ'
        skip_config_open:
        
        NoController:
        ")

# 卸载命令配置
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}
        ; 显示卸载进度信息
        DetailPrint '正在卸载 Sunshine Foundation Game Streaming Server...'
        
        ; 清理桌面快捷方式
        DetailPrint '清理桌面快捷方式...'
        ExecShell 'startunpin' '\\\"$DESKTOP\\\\Sunshine GUI.lnk\\\"'
        Delete '\\\"$DESKTOP\\\\Sunshine GUI.lnk\\\"'
        
        ; 停止运行的程序
        DetailPrint '停止运行的程序...'
        nsExec::ExecToLog 'taskkill /f /im sunshine-gui.exe'
        nsExec::ExecToLog 'taskkill /f /im sunshine.exe'
        
        ; 卸载系统组件
        DetailPrint '删除防火墙规则...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\delete-firewall-rule.bat\\\"'
        
        DetailPrint '卸载系统服务...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\uninstall-service.bat\\\"'
        
        DetailPrint '卸载虚拟显示器驱动...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\uninstall-vdd.bat\\\"'
        
        DetailPrint '恢复NVIDIA设置...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe\\\" --restore-nvprefs-undo'
        
        MessageBox MB_YESNO|MB_ICONQUESTION \
            'Do you want to remove Virtual Gamepad?' \
            /SD IDNO IDNO NoGamepad
            nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\uninstall-gamepad.bat\\\"'; skipped if no
        NoGamepad:
        MessageBox MB_YESNO|MB_ICONQUESTION \
            'Do you want to remove Virtual Sink?' \
            /SD IDNO IDNO NoSink
            nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\uninstall-vsink.bat\\\"'; skipped if no
        NoSink:
        MessageBox MB_YESNO|MB_ICONQUESTION \
            'Do you want to remove $INSTDIR (this includes the configuration, cover images, and settings)?' \
            /SD IDNO IDNO NoDelete
            RMDir /r \\\"$INSTDIR\\\"; skipped if no
        
        DetailPrint '清理环境变量...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\update-path.bat\\\" remove'
        
        NoDelete:
        DetailPrint 'Uninstall complete!'
        ")

# ==============================================================================
# Start Menu and Shortcuts Configuration
# ==============================================================================

set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_INSTALLED_ICON_NAME "${CMAKE_PROJECT_NAME}.exe")

# Enhanced Start Menu shortcuts with better icons and descriptions
set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "${CPACK_NSIS_CREATE_ICONS_EXTRA}
        SetOutPath '\$INSTDIR'
        
        ; 主程序快捷方式 - 使用可执行文件的内嵌图标
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Sunshine Service.lnk' \
            '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' '--shortcut' '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' 0
            
        ; GUI管理工具快捷方式 - 使用GUI程序的内嵌图标
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Sunshine GUI.lnk' \
            '\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe' '' '\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe' 0
            
        ; 工具文件夹快捷方式 - 使用主程序图标
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Sunshine Tools.lnk' \
            '\$INSTDIR\\\\tools' '' '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' 0
        ")

set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "${CPACK_NSIS_DELETE_ICONS_EXTRA}
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine Service.lnk'
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine GUI.lnk'
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine Tools.lnk'
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\${CMAKE_PROJECT_NAME}.lnk'
        ")

# ==============================================================================
# Advanced Installation Features
# ==============================================================================

# Custom installation options
set(CPACK_NSIS_COMPRESSOR "lzma")  # Better compression
set(CPACK_NSIS_COMPRESSOR_OPTIONS "/SOLID")  # Solid compression for smaller file size

# ==============================================================================
# Support Links and Documentation
# ==============================================================================

set(CPACK_NSIS_HELP_LINK "https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html")
set(CPACK_NSIS_URL_INFO_ABOUT "${CMAKE_PROJECT_HOMEPAGE_URL}")
set(CPACK_NSIS_CONTACT "${CMAKE_PROJECT_HOMEPAGE_URL}/support")

# ==============================================================================
# System Integration and Compatibility
# ==============================================================================

# Enable high DPI awareness for modern displays
set(CPACK_NSIS_MANIFEST_DPI_AWARE true)

# Request administrator privileges for proper installation
set(CPACK_NSIS_REQUEST_EXECUTION_LEVEL "admin")

# Enable modern installer features
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe")
set(CPACK_NSIS_MUI_FINISHPAGE_RUN_TEXT "启动 Sunshine 控制面板")

# Custom installer appearance
set(CPACK_NSIS_DISPLAY_NAME "Sunshine Foundation Game Streaming Server v${CPACK_PACKAGE_VERSION}")
set(CPACK_NSIS_PACKAGE_NAME "Sunshine")
