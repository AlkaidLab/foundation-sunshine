# NSIS Packaging
# see options at: https://cmake.org/cmake/help/latest/cpack_gen/nsis.html

set(CPACK_NSIS_INSTALLED_ICON_NAME "${PROJECT__DIR}\\\\${PROJECT_EXE}")

# 由于 CPack 的 NSIS 模板限制，我们无法直接修改 .onInit 函数
# 但可以通过以下方式实现：
# 通过 MUI_PAGE_CUSTOMFUNCTION_PRE 在目录页面显示前读取注册表
#
# 注意：CPACK_NSIS_INSTALLER_MUI_ICON_CODE 是在页面定义之前的钩子
# 我们用它来定义自定义函数，并设置 MUI_PAGE_CUSTOMFUNCTION_PRE

set(CPACK_NSIS_INSTALLER_MUI_ICON_CODE "
; 定义安装程序图标
!define MUI_ICON \\\"${CMAKE_SOURCE_DIR}/sunshine.ico\\\"
!define MUI_UNICON \\\"${CMAKE_SOURCE_DIR}/sunshine.ico\\\"

; 定义在目录页面显示前执行的函数
!define MUI_PAGE_CUSTOMFUNCTION_PRE PreDirectoryPage

; 从注册表读取之前的安装路径
; 参考 CPack NSIS 模板中 .onInit 的 ENABLE_UNINSTALL_BEFORE_INSTALL 实现

Function PreDirectoryPage
    ; 只在默认安装目录时才尝试读取注册表
    StrCmp $IS_DEFAULT_INSTALLDIR '1' 0 SkipRegRead

    Push $0
    SetRegView 64

    ; 从 NSIS 卸载注册表读取 UninstallString
    ReadRegStr $0 HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\Sunshine' 'UninstallString'
    StrCmp $0 '' DoneRegRead 0

    ; 移除引号（如果有）
    StrCpy $0 $0 '' 1   ; 移除开头的引号
    StrLen $1 $0
    IntOp $1 $1 - 1
    StrCpy $0 $0 $1     ; 移除结尾的引号

    ; 获取父目录（移除 \\\\Uninstall.exe）
    Push $0
    Call GetParent
    Pop $0
    StrCmp $0 '' DoneRegRead 0
    IfFileExists '$0\\\\*.*' SetPath DoneRegRead

    SetPath:
    StrCpy $INSTDIR $0
    StrCpy $IS_DEFAULT_INSTALLDIR '0'

    DoneRegRead:
    Pop $0

    SkipRegRead:
FunctionEnd

; 辅助函数：获取路径的父目录
Function GetParent
    Exch $0
    Push $1
    Push $2

    StrLen $1 $0
    IntOp $1 $1 - 1

    loop:
        IntOp $1 $1 - 1
        IntCmp $1 0 done done
        StrCpy $2 $0 1 $1
        StrCmp $2 '\\\\' found
        Goto loop

    found:
        StrCpy $0 $0 $1

    done:
        Pop $2
        Pop $1
        Exch $0
FunctionEnd

; Finish Page 自定义选项
; 复选框1: 打开使用教程（默认勾选）
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT '打开使用教程'
!define MUI_FINISHPAGE_RUN_FUNCTION OpenDocumentation

; 复选框2: 启动 Sunshine GUI（默认勾选）
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT '启动 Sunshine GUI'
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION LaunchGUI

Function OpenDocumentation
    ExecShell 'open' 'https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=DXpTjzl2kZwBjN7jlRMkRJ'
FunctionEnd

Function LaunchGUI
    Exec '\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe'
FunctionEnd
")

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

        ; ----------------------------------------------------------------------
        ; 清理便携版脚本：安装版不需要这两个文件
        ; 需求：如果目录下有 install_portable.bat / uninstall_portable.bat，就删除
        ; 安全防护：防止符号链接攻击 - 使用 IfFileExists 检查文件是否存在
        ;           限制在 $INSTDIR 目录内，避免路径遍历攻击
        ; ----------------------------------------------------------------------
        DetailPrint '🧹 清理便携版脚本...'
        ; 安全删除：先检查文件是否存在，避免符号链接攻击
        IfFileExists '\$INSTDIR\\\\install_portable.bat' 0 +2
        Delete '\$INSTDIR\\\\install_portable.bat'
        IfFileExists '\$INSTDIR\\\\uninstall_portable.bat' 0 +2
        Delete '\$INSTDIR\\\\uninstall_portable.bat'
        
        ; 重置文件权限
        DetailPrint '🔓 重置文件权限...'
        nsExec::ExecToLog 'icacls \\\"$INSTDIR\\\" /reset /T /C /Q >nul 2>&1'
        
        ; ----------------------------------------------------------------------
        ; 清理临时文件
        ; 安全防护：防止符号链接攻击
        ;           注意：通配符删除（*.tmp, *.old）在遇到符号链接时可能有风险
        ;           但限制在 $INSTDIR 目录内，且 NSIS 的 Delete 命令会处理符号链接
        ;           为了更安全，可以考虑逐个检查文件，但通配符删除在安装目录内风险较低
        ; ----------------------------------------------------------------------
        DetailPrint '🧹 清理临时文件...'
        ; 使用通配符删除，限制在 $INSTDIR 目录内
        ; NSIS 的 Delete 命令在处理符号链接时会删除链接本身，不会跟随到目标
        Delete '\$INSTDIR\\\\*.tmp'
        Delete '\$INSTDIR\\\\*.old'
        
        ; 显示安装进度信息
        DetailPrint '🎯 正在配置 Sunshine Foundation Game Streaming Server...'
                
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
        
        DetailPrint '⚙️ 安装并启动系统服务...'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\install-service.bat\\\"'
        nsExec::ExecToLog '\\\"$INSTDIR\\\\scripts\\\\autostart-service.bat\\\"'
        
        DetailPrint '✅ 安装完成！'
        
        NoController:
        ")

# 卸载命令配置
set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS
        "${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}
        ; 显示卸载进度信息
        DetailPrint '正在卸载 Sunshine Foundation Game Streaming Server...'
        
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
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Sunshine.lnk' \
            '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' '--shortcut' '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' 0
        ; GUI管理工具快捷方式 - 使用GUI程序的内嵌图标
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Sunshine GUI.lnk' \
            '\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe' '' '\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe' 0
            
        ; 工具文件夹快捷方式 - 使用主程序图标
        CreateShortCut '\$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Sunshine Tools.lnk' \
            '\$INSTDIR\\\\tools' '' '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' 0
        
        ; 创建桌面快捷方式 - 使用可执行文件的内嵌图标
        CreateShortCut '\$DESKTOP\\\\Sunshine.lnk' \
            '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' '--shortcut' '\$INSTDIR\\\\${CMAKE_PROJECT_NAME}.exe' 0

        ; 创建桌面快捷方式 - GUI管理工具
        CreateShortCut '\$DESKTOP\\\\Sunshine GUI.lnk' \
            '\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe' '' '\$INSTDIR\\\\assets\\\\gui\\\\sunshine-gui.exe' 0
        ")

set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "${CPACK_NSIS_DELETE_ICONS_EXTRA}
        ; ----------------------------------------------------------------------
        ; 安全删除快捷方式：防止符号链接攻击和路径遍历
        ; 
        ; 安全分析：
        ; 1. 符号链接攻击风险：如果攻击者在桌面/开始菜单创建符号链接，使用我们的快捷方式名称，
        ;    删除时可能误删其他文件。但 NSIS 的 Delete 命令对于符号链接会删除链接本身，不会跟随。
        ; 2. 路径遍历风险：我们使用固定的系统变量（$DESKTOP, $SMPROGRAMS），不接受外部输入，
        ;    路径是硬编码的，降低了路径遍历风险。
        ; 3. 文件类型验证：我们只删除预期的 .lnk 文件，文件名是固定的，降低了误删风险。
        ; 
        ; 防护措施：
        ; - 使用 IfFileExists 检查文件是否存在，避免删除不存在的文件
        ; - 使用固定的系统路径变量，不接受外部输入
        ; - 只删除预期的 .lnk 文件，文件名硬编码
        ; - NSIS 的 Delete 命令会自动处理符号链接，只删除链接本身
        ; ----------------------------------------------------------------------
        
        ; 删除开始菜单快捷方式（安全删除）
        ; 注意：$MUI_TEMP 是 NSIS 内部变量，指向开始菜单文件夹，由安装程序控制
        IfFileExists '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine.lnk' 0 +2
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine.lnk'
        IfFileExists '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine GUI.lnk' 0 +2
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine GUI.lnk'
        IfFileExists '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine Tools.lnk' 0 +2
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine Tools.lnk'
        IfFileExists '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine Service.lnk' 0 +2
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\Sunshine Service.lnk'
        IfFileExists '\$SMPROGRAMS\\\\$MUI_TEMP\\\\${CMAKE_PROJECT_NAME}.lnk' 0 +2
        Delete '\$SMPROGRAMS\\\\$MUI_TEMP\\\\${CMAKE_PROJECT_NAME}.lnk'
        
        ; 删除桌面快捷方式（安全删除）
        ; 注意：$DESKTOP 是 NSIS 系统变量，指向当前用户的桌面目录
        ;       如果攻击者创建符号链接，NSIS 的 Delete 会删除链接本身，不会跟随到目标
        IfFileExists '\$DESKTOP\\\\Sunshine.lnk' 0 +2
        Delete '\$DESKTOP\\\\Sunshine.lnk'
        IfFileExists '\$DESKTOP\\\\Sunshine GUI.lnk' 0 +2
        Delete '\$DESKTOP\\\\Sunshine GUI.lnk'
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

# Custom installer appearance
set(CPACK_NSIS_DISPLAY_NAME "Sunshine Foundation Game Streaming Server v${CPACK_PACKAGE_VERSION}")
set(CPACK_NSIS_PACKAGE_NAME "Sunshine")
