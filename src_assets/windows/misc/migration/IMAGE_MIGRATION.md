# Image Path Migration Guide

## 概述

此迁移脚本会自动将存量的本地文件路径图片迁移到新的 `covers/` 目录，并更新 `apps.json` 中的引用。

## 迁移逻辑

### 1️⃣ 识别需要迁移的图片路径

脚本会检查 `apps.json` 中每个应用的 `image-path` 字段：

**跳过（无需迁移）：**
- `desktop` - 使用桌面图片
- `app_name_123.png` - 已经是 boxart 格式（仅文件名，无路径分隔符）

**需要迁移：**
- `C:\Users\...\picture.png` - 绝对路径
- `./images/game.jpg` - 相对路径
- `covers/steam.png` - 旧的 covers 路径（已被第58行处理）

### 2️⃣ 查找源文件

脚本会按以下顺序查找图片文件：

1. **绝对路径** - 直接检查路径是否存在
2. **相对于 config 目录** - `config\<image-path>`
3. **相对于旧的 Sunshine 根目录** - `<parent>\<image-path>`

### 3️⃣ 复制到 covers 目录

找到源文件后：

```
源文件: C:\Users\mohaha\Pictures\game.png
应用名: Steam
↓
生成新文件名: app_Steam_1234567890.png
↓
复制到: config\covers\app_Steam_1234567890.png
```

### 4️⃣ 更新 apps.json

```json
// 迁移前
{
  "name": "Steam",
  "image-path": "C:\\Users\\mohaha\\Pictures\\game.png"
}

// 迁移后
{
  "name": "Steam",
  "image-path": "app_Steam_1234567890.png"
}
```

## 工作流程

```
┌─────────────────────────────────────────────────────────────┐
│ 1. migrate-config.bat 执行                                   │
│    - 迁移 covers/ 目录                                       │
│    - 更新旧的 ./covers/ 路径为 ./config/covers/              │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 2. 调用 migrate-images.ps1                                   │
│    - 读取 apps.json                                         │
│    - 查找每个应用的图片文件                                   │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 3. 对每个图片文件：                                           │
│    ✅ 复制到 config/covers/app_<name>_<timestamp>.png       │
│    ✅ 更新 apps.json 中的 image-path                        │
└─────────────────────────────────────────────────────────────┘
                           ↓
┌─────────────────────────────────────────────────────────────┐
│ 4. Sunshine 服务启动                                         │
│    ✅ getBoxArt 从 covers/ 目录加载图片                      │
│    ✅ Web UI 通过 /boxart/<filename> 访问                   │
└─────────────────────────────────────────────────────────────┘
```

## 示例场景

### 场景 1: 绝对路径

**迁移前：**
```json
{
  "apps": [
    {
      "name": "游戏A",
      "image-path": "C:\\Users\\mohaha\\Pictures\\game1.jpg"
    }
  ]
}
```

**迁移后：**
```json
{
  "apps": [
    {
      "name": "游戏A",
      "image-path": "app_游戏A_1234567890.jpg"
    }
  ]
}
```

**文件位置：**
- 源: `C:\Users\mohaha\Pictures\game1.jpg`
- 目标: `config\covers\app_游戏A_1234567890.jpg`

### 场景 2: 相对路径

**迁移前：**
```json
{
  "apps": [
    {
      "name": "Steam",
      "image-path": "./assets/steam-icon.png"
    }
  ]
}
```

**迁移后：**
```json
{
  "apps": [
    {
      "name": "Steam",
      "image-path": "app_Steam_1234567891.png"
    }
  ]
}
```

**查找顺序：**
1. ❌ `./assets/steam-icon.png` (绝对路径)
2. ✅ `config\assets\steam-icon.png` (相对于 config)
3. (找到，停止查找)

### 场景 3: 已经是新格式（跳过）

**apps.json：**
```json
{
  "apps": [
    {
      "name": "Desktop",
      "image-path": "desktop"
    },
    {
      "name": "Chrome",
      "image-path": "app_Chrome_1234567892.png"
    }
  ]
}
```

**结果：** 两个应用都跳过，不进行迁移

## 错误处理

### 文件不存在
```
⚠️  Image file not found for 游戏B: C:\old\path\missing.png
```
- **行为**: 保留原始路径，不修改 apps.json
- **原因**: 可能是网络路径或将来会可用

### 复制失败
```
❌ Failed to copy image for 游戏C: Access denied
```
- **行为**: 保留原始路径，继续处理其他应用
- **原因**: 权限问题或磁盘空间不足

## 兼容性

### 与新的 getBoxArt 配合

```cpp
// src/confighttp.cpp - getBoxArt 函数
std::string imagePath = SUNSHINE_ASSETS_DIR "boxart/" + path;

// 如果在 boxart/ 未找到，尝试 covers/
if (!fs::exists(imagePath)) {
  std::string coversPath = platf::appdata().string() + "/covers/" + path;
  if (fs::exists(coversPath)) {
    imagePath = coversPath;  // ✅ 迁移的图片在这里
  }
}
```

### 与 Web UI 配合

```javascript
// getImagePreviewUrl 函数
if (imagePath.includes('/') || imagePath.includes('\\')) {
  return imagePath;  // 旧的路径格式（迁移前）
} else {
  return `/boxart/${imagePath}`;  // ✅ 新格式（迁移后）
}
```

## 测试迁移

### 手动测试

1. 创建测试 apps.json：
```json
{
  "apps": [
    {
      "name": "Test",
      "image-path": "C:\\test\\image.png"
    }
  ]
}
```

2. 运行迁移：
```cmd
powershell -ExecutionPolicy Bypass -File migrate-images.ps1 "C:\path\to\config"
```

3. 验证结果：
- ✅ 文件已复制到 `config\covers\app_Test_*.png`
- ✅ apps.json 已更新为新路径
- ✅ Sunshine Web UI 能正常显示图片

## 日志输出示例

```
Reading apps.json from: C:\Sunshine\config\apps.json
Created covers directory: C:\Sunshine\config\covers
Found image file: C:\Users\mohaha\Pictures\game.png
✅ Migrated: Steam
   From: C:\Users\mohaha\Pictures\game.png
   To:   C:\Sunshine\config\covers\app_Steam_1234567890.png
   New path: app_Steam_1234567890.png

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ Successfully migrated 1 image(s)
   Updated: C:\Sunshine\config\apps.json
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

## 注意事项

1. **备份**: 迁移前会自动备份（通过 Copy-Item，原文件不会删除）
2. **权限**: 需要对 config 目录有写入权限
3. **文件名**: 特殊字符会被替换为下划线（`[^a-zA-Z0-9_-]` → `_`）
4. **时间戳**: 使用 Unix 时间戳确保文件名唯一
5. **编码**: apps.json 使用 UTF-8 编码保存




