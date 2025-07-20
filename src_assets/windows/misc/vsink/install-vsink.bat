@echo off
:: 检查管理员权限
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo 请以管理员身份运行此脚本
    pause
    exit /b
)

:: 检查是否已安装VB-Cable驱动
reg query "HKLM\SOFTWARE\VB\VBAudioVAC" >nul 2>&1
if %errorLevel% equ 0 (
    echo VB-Cable驱动已安装
    pause
    exit /b
)

:: 设置变量
set "installer=VBCABLE_Driver_Pack43.zip"
set "download_url=https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip"
set "temp_dir=%TEMP%\vb_cable_install"

:: 创建临时目录
if not exist "%temp_dir%" mkdir "%temp_dir%"

:: 下载安装包
echo 正在下载VB-Cable驱动...
powershell -Command "Invoke-WebRequest -Uri '%download_url%' -OutFile '%temp_dir%\%installer%'"

:: 解压文件
echo 正在解压文件...
powershell -Command "Expand-Archive -Path '%temp_dir%\%installer%' -DestinationPath '%temp_dir%' -Force"

:: 安装驱动
echo 正在安装VB-Cable驱动...
start /wait "" "%temp_dir%\VBCABLE_Setup_x64.exe" /S

:: 清理临时文件
echo 正在清理临时文件...
rd /s /q "%temp_dir%"

echo VB-Cable 驱动安装完成!
pause
