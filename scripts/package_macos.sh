#!/bin/bash
# macOS .app bundle and DMG packaging script for Sunshine
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
ASSETS_SRC="${PROJECT_DIR}/src_assets"

# Find the built binary (use the most recently modified one)
BINARY=$(find "$BUILD_DIR" -maxdepth 1 -name "sunshine-*" -type f -perm +111 -exec ls -t {} + | head -1)
if [ -z "$BINARY" ]; then
    echo "❌ 找不到编译好的 sunshine 二进制文件，请先运行 ninja -C build"
    exit 1
fi
echo "✅ 找到二进制文件: $BINARY"

# Output paths
APP_NAME="Sunshine.app"
APP_DIR="${BUILD_DIR}/${APP_NAME}"
DMG_NAME="Sunshine-Foundation.dmg"
DMG_PATH="${BUILD_DIR}/${DMG_NAME}"

# Clean previous build
rm -rf "$APP_DIR"
rm -f "$DMG_PATH"

# ---------- Create .app bundle structure ----------
echo "📦 创建 .app 目录结构..."
mkdir -p "$APP_DIR/Contents/MacOS"
mkdir -p "$APP_DIR/Contents/Resources"

# ---------- Copy binary ----------
echo "📋 复制可执行文件..."
cp "$BINARY" "$APP_DIR/Contents/MacOS/sunshine"
chmod +x "$APP_DIR/Contents/MacOS/sunshine"

# ---------- Copy assets ----------
echo "📋 复制资源文件..."
if [ -d "$BUILD_DIR/assets" ]; then
    cp -R "$BUILD_DIR/assets" "$APP_DIR/Contents/Resources/assets"
else
    echo "⚠️  build/assets 不存在，尝试从 src_assets 复制..."
    mkdir -p "$APP_DIR/Contents/Resources/assets"
    cp -R "$ASSETS_SRC/common/assets/"* "$APP_DIR/Contents/Resources/assets/" 2>/dev/null || true
    cp -R "$ASSETS_SRC/macos/assets/"* "$APP_DIR/Contents/Resources/assets/" 2>/dev/null || true
fi

# ---------- Copy icon ----------
echo "📋 复制应用图标..."
if [ -f "$PROJECT_DIR/sunshine.icns" ]; then
    cp "$PROJECT_DIR/sunshine.icns" "$APP_DIR/Contents/Resources/sunshine.icns"
else
    echo "⚠️  sunshine.icns 未找到，跳过图标"
fi

# ---------- Generate Info.plist ----------
echo "📋 生成 Info.plist..."
cat > "$APP_DIR/Contents/Info.plist" << 'PLIST_EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key>
  <string>dev.lizardbyte.sunshine</string>
  <key>CFBundleName</key>
  <string>Sunshine</string>
  <key>CFBundleDisplayName</key>
  <string>Sunshine</string>
  <key>CFBundleExecutable</key>
  <string>sunshine</string>
  <key>CFBundleIconFile</key>
  <string>sunshine</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleVersion</key>
  <string>2026.0215</string>
  <key>CFBundleShortVersionString</key>
  <string>2026.0215</string>
  <key>LSMinimumSystemVersion</key>
  <string>12.0</string>
  <key>LSUIElement</key>
  <true/>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>NSMicrophoneUsageDescription</key>
  <string>Sunshine 需要麦克风权限来串流音频。</string>
  <key>NSScreenCaptureUsageDescription</key>
  <string>Sunshine 需要屏幕录制权限来捕获屏幕画面。</string>
  <key>NSCameraUsageDescription</key>
  <string>Sunshine 需要摄像头权限。</string>
</dict>
</plist>
PLIST_EOF

# ---------- Create launcher wrapper ----------
# The wrapper sets the working directory so sunshine can find assets
echo "📋 创建启动器..."
mv "$APP_DIR/Contents/MacOS/sunshine" "$APP_DIR/Contents/MacOS/sunshine-bin"
cat > "$APP_DIR/Contents/MacOS/sunshine" << 'LAUNCHER_EOF'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
RESOURCES_DIR="$(dirname "$DIR")/Resources"
export SUNSHINE_ASSETS_DIR="$RESOURCES_DIR/assets"
CONFIG_DIR="$HOME/.config/sunshine"
mkdir -p "$CONFIG_DIR"
exec "$DIR/sunshine-bin" "$CONFIG_DIR/sunshine.conf" "$@"
LAUNCHER_EOF
chmod +x "$APP_DIR/Contents/MacOS/sunshine"

echo "✅ .app 包创建完成: $APP_DIR"

# ---------- Create DMG ----------
echo "📦 创建 DMG 安装包..."
DMG_TEMP="${BUILD_DIR}/dmg_temp"
rm -rf "$DMG_TEMP"
mkdir -p "$DMG_TEMP"

# Copy .app to temp directory
cp -R "$APP_DIR" "$DMG_TEMP/"

# Create symbolic link to /Applications for drag-and-drop install
ln -s /Applications "$DMG_TEMP/Applications"

# Create DMG
hdiutil create -volname "Sunshine" \
    -srcfolder "$DMG_TEMP" \
    -ov -format UDZO \
    "$DMG_PATH"

# Cleanup
rm -rf "$DMG_TEMP"

echo ""
echo "🎉 打包完成！"
echo "   .app 位置: $APP_DIR"
echo "   DMG 位置: $DMG_PATH"
echo ""
echo "安装方法："
echo "  1. 双击 DMG 文件"
echo "  2. 将 Sunshine 图标拖入 Applications 文件夹"
echo "  3. 在启动台或应用程序文件夹中找到 Sunshine 并双击启动"
