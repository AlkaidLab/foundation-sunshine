@echo off
:: 卸载VB-Cable虚拟音频设备
:: 需要管理员权限运行

:: 检查是否以管理员身份运行
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo 请以管理员身份运行此脚本
    pause
    exit /b
)

:: 停止并删除VB-Cable服务
echo 正在停止VB-Cable服务...
net stop "VB-Audio Virtual Cable" >nul 2>&1

echo 正在删除VB-Cable驱动...
pnputil /delete-driver oem*.inf /uninstall /force >nul 2>&1

:: 删除注册表项
echo 正在清理注册表...
reg delete "HKLM\SYSTEM\CurrentControlSet\Services\VB-Audio Virtual Cable" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\VB-Audio" /f >nul 2>&1

:: 删除程序文件
echo 正在删除程序文件...
rd /s /q "%ProgramFiles%\VB\Cable" >nul 2>&1
rd /s /q "%ProgramFiles(x86)%\VB\Cable" >nul 2>&1

echo 卸载完成！
echo 请重新启动计算机使更改生效
pause
