#!/bin/bash
# qiin-tabtip.exe 快速编译脚本

set -e  # 遇到错误立即退出

echo "========================================"
echo "qiin-tabtip 编译脚本"
echo "========================================"
echo ""

# 检查源文件
if [ ! -f "qiin-tabtip.cpp" ]; then
    echo "❌ 错误: 找不到 qiin-tabtip.cpp"
    exit 1
fi

# 检测编译器
COMPILER=""
COMPILE_CMD=""

if command -v cl.exe &> /dev/null; then
    COMPILER="MSVC"
    COMPILE_CMD="cl.exe /EHsc /std:c++17 /DUNICODE /D_UNICODE /Os /Fe:qiin-tabtip.exe qiin-tabtip.cpp shell32.lib user32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib /link /SUBSYSTEM:CONSOLE /nologo"
elif command -v g++ &> /dev/null; then
    COMPILER="MinGW/GCC"
    COMPILE_CMD="g++ -std=c++17 -DUNICODE -D_UNICODE -Os -s -o qiin-tabtip.exe qiin-tabtip.cpp -lshell32 -luser32 -ladvapi32 -lole32 -loleaut32 -luuid -static-libgcc -static-libstdc++ -Wl,--gc-sections -municode"
elif command -v clang++ &> /dev/null; then
    COMPILER="Clang"
    COMPILE_CMD="clang++ -std=c++17 -DUNICODE -D_UNICODE -Os -s -o qiin-tabtip.exe qiin-tabtip.cpp -lshell32 -luser32 -ladvapi32 -lole32 -loleaut32 -luuid -static-libgcc -static-libstdc++ -Wl,--gc-sections -municode"
else
    echo "❌ 未找到 C++ 编译器"
    echo ""
    echo "请安装以下之一:"
    echo "  - Visual Studio (MSVC)"
    echo "  - MinGW-w64"
    echo "  - Clang"
    exit 1
fi

echo "编译器: $COMPILER"
echo ""
echo "开始编译..."
echo "========================================"

# 执行编译
eval $COMPILE_CMD

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "✓ 编译成功！"
    echo "========================================"
    echo ""
    
    # 显示文件信息
    if [ -f "qiin-tabtip.exe" ]; then
        FILE_SIZE=$(stat -c%s "qiin-tabtip.exe" 2>/dev/null || stat -f%z "qiin-tabtip.exe" 2>/dev/null || echo "unknown")
        if [ "$FILE_SIZE" != "unknown" ]; then
            SIZE_KB=$((FILE_SIZE / 1024))
            echo "文件: qiin-tabtip.exe"
            echo "大小: ${SIZE_KB} KB"
            echo ""
        fi
    fi
    
    echo "使用方法:"
    echo "  ./qiin-tabtip.exe        # 切换键盘"
    echo "  ./qiin-tabtip.exe show   # 显示键盘"
    echo "  ./qiin-tabtip.exe osk    # 屏幕键盘"
    echo "  ./qiin-tabtip.exe help   # 查看帮助"
    echo ""
else
    echo ""
    echo "========================================"
    echo "❌ 编译失败"
    echo "========================================"
    exit 1
fi

