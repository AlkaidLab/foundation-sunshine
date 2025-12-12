# `run_command` 函数开发说明

## 1. 函数签名

```cpp
bp::child run_command(
    bool elevated,                      // 是否以提升权限运行
    bool interactive,                   // 是否为交互式进程（需要控制台窗口）
    const std::string &cmd,             // 要执行的命令字符串
    boost::filesystem::path &working_dir, // 工作目录
    const bp::environment &env,         // 环境变量（包含 SUNSHINE_* 变量）
    FILE *file,                         // 输出重定向文件（可为 nullptr）
    std::error_code &ec,                // 错误码输出
    bp::group *group                    // 进程组/作业对象（可为 nullptr）
);
```

## 2. 功能概述

`run_command` 是 Windows 平台的**统一进程启动接口**：

- **跨权限执行**：SYSTEM 服务可以用户身份启动进程
- **智能命令解析**：自动处理 URL、文件关联、可执行文件
- **环境变量展开**：支持 `%SUNSHINE_CLIENT_WIDTH%` 等变量
- **作业对象管理**：支持进程生命周期跟踪

## 3. 执行流程

```
run_command 入口
      │
      ▼
1. 初始化：创建 STARTUPINFOEXW，克隆环境变量
      │
      ▼
2. 环境变量展开：expand_env_vars_in_cmd(cmd, cloned_env)
   - %SUNSHINE_CLIENT_WIDTH% → 1920
   - %SUNSHINE_CLIENT_HEIGHT% → 1080
      │
      ▼
3. 命令解析：resolve_command_string()
   - URL → 查询注册表找默认浏览器
   - .exe → 直接执行
   - 其他文件 → 查询关联程序
      │
      ▼
4. 进程创建
   - SYSTEM 模式：CreateProcessAsUserW（模拟用户身份）
   - 普通模式：CreateProcessW
      │
      ▼
5. 返回 bp::child 对象
```

## 4. 环境变量展开机制

### 4.1 Sunshine 内置环境变量

在 `process.cpp` 和 `nvhttp.cpp` 中设置，存储于 `_env`，可在命令中使用 `%VAR%` 语法访问：

#### 应用相关变量

| 变量名 | 类型 | 说明 | 示例值 |
|--------|------|------|--------|
| `SUNSHINE_APP_ID` | 数字 | 应用 ID | `"123"` |
| `SUNSHINE_APP_NAME` | 字符串 | 应用名称 | `"Game"` |

#### 客户端标识相关变量

| 变量名 | 类型 | 说明 | 示例值 |
|--------|------|------|--------|
| `SUNSHINE_CLIENT_ID` | 数字 | 客户端会话 ID | `"12345"` |
| `SUNSHINE_CLIENT_UNIQUE_ID` | 字符串 | 客户端唯一标识符 | `"unique-id-string"` |
| `SUNSHINE_CLIENT_NAME` | 字符串 | 客户端设备名称 | `"iPhone"` |

#### 客户端显示相关变量

| 变量名 | 类型 | 说明 | 示例值 |
|--------|------|------|--------|
| `SUNSHINE_CLIENT_WIDTH` | 数字 | 客户端屏幕宽度（像素） | `"1920"` |
| `SUNSHINE_CLIENT_HEIGHT` | 数字 | 客户端屏幕高度（像素） | `"1080"` |
| `SUNSHINE_CLIENT_FPS` | 数字 | 客户端刷新率（FPS） | `"60"` |
| `SUNSHINE_CLIENT_HDR` | 布尔 | 是否启用 HDR | `"true"` 或 `"false"` |
| `SUNSHINE_CLIENT_CUSTOM_SCREEN_MODE` | 数字 | 自定义屏幕模式 | `"0"` |

#### 客户端音频相关变量

| 变量名 | 类型 | 说明 | 示例值 |
|--------|------|------|--------|
| `SUNSHINE_CLIENT_AUDIO_CONFIGURATION` | 字符串 | 音频配置（仅在支持时设置） | `"2.0"`, `"5.1"`, `"7.1"` |
| `SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS` | 字符串 | 环绕声参数 | 取决于客户端 |

#### 客户端功能相关变量

| 变量名 | 类型 | 说明 | 示例值 |
|--------|------|------|--------|
| `SUNSHINE_CLIENT_GCMAP` | 数字 | 游戏控制器映射 | `"0"` |
| `SUNSHINE_CLIENT_HOST_AUDIO` | 布尔 | 是否使用主机音频 | `"true"` 或 `"false"` |
| `SUNSHINE_CLIENT_ENABLE_SOPS` | 布尔 | 是否启用 SOPS | `"true"` 或 `"false"` |
| `SUNSHINE_CLIENT_ENABLE_MIC` | 布尔 | 是否启用麦克风 | `"true"` 或 `"false"` |
| `SUNSHINE_CLIENT_USE_VDD` | 布尔 | 是否使用虚拟显示器 | `"true"` 或 `"false"` |
| `SUNSHINE_CLIENT_CERT_UUID` | 字符串 | 客户端证书 UUID（稳定的客户端标识符，仅在存在时设置） | `"uuid-string"` |

#### 使用示例

```bash
# 使用分辨率变量
qres.exe /x:%SUNSHINE_CLIENT_WIDTH% /y:%SUNSHINE_CLIENT_HEIGHT% /r:%SUNSHINE_CLIENT_FPS%

# 使用 HDR 状态
if "%SUNSHINE_CLIENT_HDR%"=="true" (
    enable_hdr.exe
)

# 使用客户端名称
echo "Connected from: %SUNSHINE_CLIENT_NAME%"

# 使用麦克风状态
if "%SUNSHINE_CLIENT_ENABLE_MIC%"=="true" (
    configure_mic.exe
)
```

### 4.2 展开实现

使用 `expand_env_vars_in_cmd` 函数（misc.cpp）：

```cpp
std::string expand_env_vars_in_cmd(const std::string &cmd, const bp::environment &env) {
    // 1. 快速检查：无 '%' 直接返回
    // 2. 逐字符解析 %VAR% 模式
    // 3. 先在 cloned_env 中查找（包含 SUNSHINE_* 变量）
    // 4. 再在系统环境中查找
    // 5. 找不到则保持原样
}
```

### 4.3 为什么不用 `ExpandEnvironmentStringsW`

- `ExpandEnvironmentStringsW` 只能访问**当前进程的环境变量**
- `SUNSHINE_CLIENT_WIDTH` 等变量在 `cloned_env` 中，不在 Sunshine 进程环境中
- 因此需要手动从 `cloned_env` 中查找并替换

### 4.4 变量查找顺序

1. 首先在 `cloned_env` 中查找（大小写不敏感）
2. 然后在系统环境中查找（使用 `GetEnvironmentVariableA`）
3. 找不到则保留原始 `%VAR%` 语法

### 4.5 使用示例

```bash
# 直接使用环境变量（✅ 现在支持）
unlocker.exe -screen-width %SUNSHINE_CLIENT_WIDTH% -screen-height %SUNSHINE_CLIENT_HEIGHT%

# 展开后
unlocker.exe -screen-width 1920 -screen-height 1080

# 现有 cmd /C 用法仍然有效（向后兼容）
cmd /C qres.exe /x:%SUNSHINE_CLIENT_WIDTH% /y:%SUNSHINE_CLIENT_HEIGHT%
```

### 4.6 特殊处理

| 输入 | 输出 | 说明 |
|------|------|------|
| `%SUNSHINE_CLIENT_WIDTH%` | `1920` | 正常展开 |
| `%UNKNOWN%` | `%UNKNOWN%` | 未找到，保持原样 |
| `%%` | `%` | 转义字符 |
| `100%` | `100%` | 无配对，保持原样 |

## 5. URL 处理机制

当命令是 URL 时，自动查询注册表找到默认浏览器：

```
输入: "https://github.com/example"
  │
  ▼
PathIsURLW() → 是 URL
  │
  ▼
提取 scheme: "https"
  │
  ▼
AssocQueryStringW() 查询注册表
  │
  ▼
获取默认浏览器命令: "C:\...\chrome.exe" "%1"
  │
  ▼
替换 %1: "C:\...\chrome.exe" "https://github.com/example"
```

## 6. 注意事项

1. **环境隔离**：传入的 `env` 会被克隆，不会污染其他调用
2. **PATH 临时修改**：工作目录会临时添加到 PATH 前面
3. **SYSTEM 模式**：作为服务运行时，自动模拟用户身份启动 GUI 进程
4. **向后兼容**：现有 `cmd /C` 配置仍然有效

---

*最后更新: 2024*
