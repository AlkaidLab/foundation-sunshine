# 显示器管理架构设计文档

## 目录

- [现状分析](#现状分析)
- [当前架构](#当前架构)
- [设计缺陷](#设计缺陷)
- [重构目标架构](#重构目标架构)
- [模块设计](#模块设计)
- [会话生命周期](#会话生命周期)
- [实施计划](#实施计划)

---

## 现状分析

### 当前数据流

```mermaid
graph LR
    A[Moonlight Client] -->|HTTP /launch| B[nvhttp.cpp]
    B -->|env vars| C[display_device::session_t]
    C -->|pipe/DevManView| D[VDD Driver]
    C -->|DXGI enum| E[display_base.cpp]
    E -->|select output| F[DXGI Duplication]
    F -->|frames| G[Encoder]
```

### 当前文件分布

| 文件 | 职责 |
|------|------|
| `src/nvhttp.cpp` | 解析客户端请求、构建 launch_session |
| `src/display_device/vdd_utils.cpp` | VDD 驱动控制(pipe IPC + DevManView) |
| `src/platform/windows/display_base.cpp` | DXGI 枚举、显示器选择、分辨率检测 |
| `src/platform/windows/display.h` | 显示器元数据(旋转/色彩空间/格式) |
| `src/config.cpp` | `config::video.output_name` 服务端配置 |

---

## 当前架构

```mermaid
graph TB
    subgraph CurrentArch["当前架构 (散乱)"]
        direction TB
        
        subgraph HTTP["HTTP Layer"]
            NV["nvhttp.cpp<br/>• 解析 useVdd/customScreenMode/display_name<br/>• 构建 env vars<br/>• 无冲突检测"]
        end
        
        subgraph Config["配置 (散布)"]
            CF["config.cpp<br/>video.output_name"]
            LS["launch_session<br/>CLIENT_DISPLAY_NAME<br/>CLIENT_USE_VDD<br/>CLIENT_CUSTOM_SCREEN_MODE"]
        end
        
        subgraph VDD["VDD 控制 (脆弱)"]
            VU["vdd_utils.cpp<br/>• 硬编码驱动名<br/>• 全局 static 状态<br/>• 3s 固定超时<br/>• 进程不等待"]
        end
        
        subgraph Display["显示器选择 (不确定)"]
            DB["display_base.cpp<br/>• DXGI 枚举竞态<br/>• 字符串匹配<br/>• 静默 fallback<br/>• 无客户端通知"]
        end
        
        NV --> CF
        NV --> LS
        LS --> VU
        LS --> DB
        CF --> DB
    end
    
    style CurrentArch fill:#ffebee,stroke:#c62828
```

---

## 设计缺陷

### 缺陷列表

```mermaid
graph LR
    subgraph Issues["7 个设计缺陷"]
        P1["P1: 显示器选择不确定<br/>⚠️ 高"]
        P2["P2: VDD 集成脆弱<br/>⚠️ 高"]
        P3["P3: 配置优先级混乱<br/>🔶 中"]
        P4["P4: 线程安全隐患<br/>🔶 中"]
        P5["P5: 格式不匹配<br/>🔵 低"]
        P6["P6: HDR 色彩空间<br/>🔵 低"]
        P7["P7: 孤立显示状态<br/>⚠️ 高"]
    end
    
    style P1 fill:#ffcdd2
    style P2 fill:#ffcdd2
    style P3 fill:#fff9c4
    style P4 fill:#fff9c4
    style P5 fill:#bbdefb
    style P6 fill:#bbdefb
    style P7 fill:#ffcdd2
```

### 缺陷详情

#### P1: 显示器选择不确定性

```mermaid
sequenceDiagram
    participant C as Client
    participant S as Sunshine
    participant D as DXGI
    
    C->>S: /launch?display_name=HDMI-1
    S->>D: EnumAdapters1() + EnumOutputs()
    
    Note over D: 枚举顺序不稳定<br/>可能因系统状态变化
    
    D-->>S: [DISPLAY1, DISPLAY3, DISPLAY2]
    
    alt 匹配到 HDMI-1
        S->>S: 选择对应 output ✅
    else 未匹配
        S->>S: 静默选择第一个 ⚠️
        Note over S: 用户不知道<br/>实际捕获的是哪个
    end
    
    S-->>C: 开始串流 (可能是错误的显示器)
```

#### P2: VDD 控制流程问题

```mermaid
sequenceDiagram
    participant S as Sunshine
    participant P as Named Pipe
    participant DM as DevManView
    participant V as VDD Driver
    
    S->>P: ConnectNamedPipe (3s timeout)
    
    alt Pipe 连接成功
        S->>P: WriteFile(command, 4KB max)
        P->>V: 转发命令
        V-->>P: 响应
        P-->>S: ReadFile
    else Pipe 超时
        S->>DM: CreateProcess("DevManView /enable ...")
        Note over DM: ⚠️ child.detach()<br/>进程不等待完成<br/>可能还在运行时函数已返回
        DM->>V: 操作驱动
    end
    
    Note over S: 全局 static 状态<br/>last_toggle_time<br/>无锁保护 ⚠️
```

#### P7: 崩溃后孤立状态

```mermaid
stateDiagram-v2
    [*] --> Normal: Sunshine 启动
    Normal --> VddConfigured: 客户端请求 VDD
    VddConfigured --> SessionActive: 串流中
    
    SessionActive --> Normal: 正常断开<br/>restore_state() ✅
    SessionActive --> Orphaned: 进程崩溃 ⚠️
    
    Orphaned --> [*]: VDD 保持错误模式<br/>物理显示器可能被禁用<br/>vdd_settings.xml 不一致
    
    note right of Orphaned
        下次启动时
        无法恢复到正确状态
    end note
```

---

## 重构目标架构

```mermaid
graph TB
    subgraph Client["Moonlight Client"]
        CR[Launch Request<br/>useVdd / screenMode / displayName<br/>width / height / fps]
    end

    subgraph SessionLayer["Session Layer (绿)"]
        NH["nvhttp.cpp<br/>Launch Handler"]
        SC["DisplaySessionConfig<br/>• selection priority<br/>• vdd_mode<br/>• resolution/fps<br/>• validate()"]
        SL["SessionLifecycle<br/>• start / stop / crash recovery<br/>• state persistence"]
    end

    subgraph DisplayManager["IDisplayManager (蓝)"]
        direction TB
        DM["DisplayManager<br/>• enum_displays()<br/>• select_display()<br/>• configure()<br/>• on_change listener"]
        DI["DisplayInfo 列表<br/>• device_name<br/>• friendly_name<br/>• adapter_index<br/>• supports_dup<br/>• is_vdd"]
        DV["DisplayValidator<br/>• pre-flight 检查<br/>• 冲突检测<br/>• 格式兼容性"]
    end

    subgraph VddDriver["VddDriver (橙)"]
        direction TB
        VD["VddDriver<br/>mutex 保护状态<br/>• is_available()<br/>• enable() / disable()<br/>• set_mode()"]
        VP["PipeClient<br/>• 版本化协议<br/>• 可配置超时<br/>• 消息校验"]
        VS["VddState<br/>• current_mode<br/>• crash_recovery_file<br/>• last_known_good"]
    end

    subgraph CapturePipeline["Capture Pipeline (紫)"]
        direction TB
        CS["CaptureSelector<br/>DDx / WGC / AMD DC"]
        DD["DXGI Duplication<br/>• format negotiation<br/>• rotation handling<br/>• HDR color space"]
        ENC["Encoder<br/>NVENC / AMF / QSV / SW"]
    end

    subgraph OS["Windows 显示子系统"]
        DXGI["DXGI Factory<br/>Adapters & Outputs"]
        PHY["物理显示器"]
        VDDDRV["ZakoVDD Driver<br/>IddCx Kernel"]
        NEFCON["nefconw.exe<br/>驱动安装"]
    end

    CR -->|HTTP /launch| NH
    NH -->|构建配置| SC
    SC -->|预检| DV
    DV -->|通过| DM
    DM -->|枚举| DXGI
    DXGI -.->|outputs| DI
    DI -->|选择| DM

    SC -->|VDD 请求?| VD
    VD -->|pipe IPC| VP
    VP -->|命令| VDDDRV
    VD -->|持久化| VS

    DM -->|选中的显示器| CS
    CS -->|创建 dup| DD
    DD -->|帧数据| ENC

    SL -->|崩溃恢复| VS
    SL -->|恢复状态| DM

    DXGI --- PHY
    DXGI --- VDDDRV
    NEFCON -.->|安装/卸载| VDDDRV

    style DisplayManager fill:#e1f5fe,stroke:#0288d1
    style VddDriver fill:#fff3e0,stroke:#f57c00
    style SessionLayer fill:#e8f5e9,stroke:#388e3c
    style CapturePipeline fill:#f3e5f5,stroke:#7b1fa2
```

---

## 模块设计

### DisplaySessionConfig — 配置合并

```mermaid
graph TB
    subgraph Inputs["配置来源"]
        I1["客户端请求<br/>display_name, useVdd, screenMode"]
        I2["服务端配置<br/>config::video.output_name"]
        I3["系统状态<br/>DXGI 枚举结果"]
    end
    
    subgraph Resolution["优先级解析"]
        R1{"客户端指定了<br/>display_name?"}
        R2{"服务端配置了<br/>output_name?"}
        R3["使用第一个<br/>可用显示器"]
    end
    
    subgraph Validation["预检"]
        V1["显示器存在?"]
        V2["支持 Duplication?"]
        V3["VDD 可用?"]
        V4["分辨率/帧率<br/>编码器支持?"]
        V5["冲突检测"]
    end
    
    I1 --> R1
    I2 --> R1
    R1 -->|是| V1
    R1 -->|否| R2
    R2 -->|是| V1
    R2 -->|否| R3
    R3 --> V1
    
    V1 --> V2 --> V3 --> V4 --> V5
    
    V5 -->|全部通过| OK["DisplaySessionConfig ✅"]
    V5 -->|失败| ERR["Result::Error ❌<br/>明确的错误类型"]
```

### VddDriver — 状态机

```mermaid
stateDiagram-v2
    [*] --> Unknown: 进程启动
    
    Unknown --> Checking: check_availability()
    Checking --> NotInstalled: 驱动未安装
    Checking --> Disabled: 驱动已禁用
    Checking --> Enabled: 驱动已启用
    
    NotInstalled --> [*]: 不可用
    
    Disabled --> Enabling: enable()
    Enabling --> Enabled: pipe/nefcon 成功
    Enabling --> Error: 失败 + 重试耗尽
    
    Enabled --> Configuring: set_mode(mode)
    Configuring --> Enabled: 配置完成
    Configuring --> Error: 配置失败
    
    Enabled --> Disabling: disable()
    Disabling --> Disabled: 成功
    
    Error --> Disabled: recover()
    
    note right of Enabled
        状态持久化到
        crash_recovery.json:
        {mode, timestamp, session_id}
    end note
    
    note right of Unknown
        启动时读取
        crash_recovery.json
        恢复到 last_known_good
    end note
```

### DisplayManager — 变更检测

```mermaid
sequenceDiagram
    participant DM as DisplayManager
    participant DXGI as DXGI Factory
    participant L as ChangeListener
    participant S as SessionManager
    
    loop 每 2 秒
        DM->>DXGI: factory->IsCurrent()
        alt 显示器布局变化
            DXGI-->>DM: false (not current)
            DM->>DM: re-enumerate()
            DM->>L: on_change(DisplayChangeEvent)
            L->>S: handle_display_change()
            
            alt 当前捕获的显示器断开
                S->>S: 暂停串流
                S->>DM: select_display(fallback)
                S->>S: 恢复串流 (新显示器)
            else 新显示器接入
                S->>S: 通知客户端 (可选)
            end
        else 无变化
            DXGI-->>DM: true (current)
        end
    end
```

---

## 会话生命周期

### 正常流程

```mermaid
sequenceDiagram
    participant C as Client
    participant N as nvhttp
    participant SC as SessionConfig
    participant V as VddDriver
    participant DM as DisplayManager
    participant Cap as Capture
    
    C->>N: POST /launch
    N->>SC: build_config(request)
    SC->>SC: validate()
    
    alt 需要 VDD
        SC->>V: enable() + set_mode()
        V->>V: persist_state(crash_recovery.json)
        V-->>SC: ok
    end
    
    SC->>DM: select_display(config)
    DM->>DM: enum_displays()
    DM-->>SC: DisplayInfo
    
    SC->>Cap: start_capture(display_info)
    Cap-->>C: streaming...
    
    Note over C,Cap: 串流中...
    
    C->>N: POST /cancel 或 断开
    N->>Cap: stop_capture()
    
    alt 使用了 VDD
        N->>V: restore_state()
        V->>V: delete crash_recovery.json
    end
    
    N-->>C: ok
```

### 崩溃恢复流程

```mermaid
sequenceDiagram
    participant OS as Windows
    participant S as Sunshine (重启)
    participant V as VddDriver
    participant F as crash_recovery.json
    
    Note over OS: 上次进程崩溃<br/>VDD 停留在 "VDD Only" 模式<br/>物理显示器被禁用
    
    OS->>S: Sunshine 启动
    S->>F: 读取 crash_recovery.json
    
    alt 文件存在 (异常退出)
        F-->>S: {mode: "vdd_only", prev: "all_enabled"}
        S->>V: restore_to(prev_state)
        V->>V: enable physical displays
        V->>V: disable VDD (或恢复 extend)
        S->>F: 删除 crash_recovery.json
        Note over S: 恢复完成 ✅
    else 文件不存在 (正常退出)
        Note over S: 无需恢复
    end
```

---

## 实施计划

### 分阶段重构

```mermaid
gantt
    title 显示器管理重构计划
    dateFormat YYYY-MM-DD
    
    section Phase 1: 基础设施
    DisplayInfo 结构体定义          :p1a, 2026-04-10, 2d
    DisplaySessionConfig + validate :p1b, after p1a, 3d
    Result 类型替代返回码          :p1c, after p1a, 2d
    
    section Phase 2: VDD 加固
    VddDriver 类封装              :p2a, after p1b, 3d
    crash_recovery.json 持久化     :p2b, after p2a, 2d
    启动时自动恢复                :p2c, after p2b, 1d
    pipe 协议版本化               :p2d, after p2a, 2d
    
    section Phase 3: DisplayManager
    IDisplayManager 接口           :p3a, after p2c, 2d
    DxgiDisplayManager 实现        :p3b, after p3a, 3d
    变更检测 + 监听器              :p3c, after p3b, 2d
    
    section Phase 4: 集成
    nvhttp 使用新接口              :p4a, after p3c, 2d
    配置优先级文档化               :p4b, after p4a, 1d
    端到端测试                    :p4c, after p4a, 3d
```

### 优先级

| 优先级 | 改动 | 影响 | 风险 |
|--------|------|------|------|
| P0 | VDD 崩溃恢复 (P7) | 用户体验直接影响 | 低 |
| P1 | VDD 驱动封装 (P2) | 消除全局状态/进程泄漏 | 中 |
| P2 | 配置优先级统一 (P3) | 消除配置歧义 | 低 |
| P3 | 线程安全 (P4) | 并发会话 | 中 |
| P4 | DisplayManager 抽象 (P1) | 架构改善 | 高 (改动面大) |

---

## restore_state_impl 完整场景覆盖

### 决策流程图

```mermaid
flowchart TD
    START["restore_state_impl(reason)"] --> VDD_CHECK{"VDD 存在?<br/>find_device_by_friendlyname(ZAKO_NAME)"}
    
    VDD_CHECK -->|不存在| KEEP_NO["vdd_destroyed = false"]
    VDD_CHECK -->|存在| KEEP_CHECK{"keep_enabled?"}
    
    KEEP_CHECK -->|true| KEEP_YES["保留 VDD<br/>vdd_destroyed = false"]
    KEEP_CHECK -->|false| DESTROY["销毁 VDD<br/>destroy_vdd_monitor()<br/>sleep(1000ms)<br/>vdd_destroyed = true"]
    
    KEEP_NO --> BRANCH
    KEEP_YES --> BRANCH
    DESTROY --> BRANCH
    
    BRANCH{"current_use_vdd<br/>has_value?"}
    
    BRANCH -->|"nullopt (启动)" | STARTUP_PATH
    BRANCH -->|"has_value (会话结束)"| SESSION_PATH
    
    subgraph STARTUP_PATH["启动恢复路径"]
        S1{"vdd_destroyed ||<br/>vdd_id.empty()?"}
        S1 -->|true| S2{"CCD API 可用?"}
        S1 -->|false| S3["跳过 revert_settings<br/>(常驻模式VDD仍存活)"]
        S2 -->|可用| S4["revert_settings(reason, true)<br/>从 persistent_data 恢复拓扑"]
        S2 -->|不可用| S3
        S4 --> S5
        S3 --> S5
        S5{"无头模式 &&<br/>devices.empty()?"}
        S5 -->|true| S6["create_vdd_monitor('')<br/>创建基地显示器"]
        S5 -->|false| S7["stop_timer_and_clear_vdd_state()<br/>RETURN"]
        S6 --> S7
    end
    
    subgraph SESSION_PATH["会话结束恢复路径"]
        E1{"has_persistent?"}
        E1 -->|false| E2["跳过拓扑恢复<br/>(apply_config从未成功)<br/>RETURN"]
        E1 -->|true| E3{"无头模式 &&<br/>devices.empty()?"}
        E3 -->|true| E4["create_vdd_monitor('')"]
        E3 -->|false| E5
        E4 --> E5
        E5{"is_no_operation?"}
        E5 -->|true| E6["跳过拓扑恢复<br/>RETURN"]
        E5 -->|false| E7{"CCD API 可用?"}
        E7 -->|可用| E8["revert_settings(reason, true)<br/>恢复拓扑"]
        E7 -->|不可用| E9["add_unlock_task()<br/>延迟恢复"]
        E8 --> E10["RETURN"]
        E9 --> E10
    end
    
    style DESTROY fill:#ffcdd2
    style KEEP_YES fill:#c8e6c9
    style S4 fill:#bbdefb
    style E8 fill:#bbdefb
    style E9 fill:#fff9c4
```

### 场景矩阵

```mermaid
graph LR
    subgraph Scenarios["11 个场景完整覆盖"]
        S1["#1 启动/无VDD<br/>revert no-op → return"]
        S2["#2 启动/VDD/!keep<br/>销毁 → revert → return"]
        S3["#3 启动/VDD/keep<br/>保留 → skip revert → return"]
        S4["#4 结束/无VDD/persistent<br/>→ revert"]
        S5["#5 结束/VDD/!keep/persistent<br/>销毁 → revert"]
        S6["#6 结束/VDD/keep/persistent<br/>保留 → revert"]
        S7["#7 结束/VDD/!keep/!persistent<br/>销毁 → 跳过拓扑"]
        S8["#8 结束/无VDD/!persistent<br/>→ 跳过拓扑"]
        S9["#9 结束/no_operation<br/>→ 跳过拓扑"]
        S10["#10 结束/CCD锁定<br/>→ 延迟重试"]
        S11["#11 启动/VDD/CCD锁定<br/>销毁 → skip revert"]
    end
    
    style S2 fill:#e3f2fd
    style S5 fill:#e3f2fd
    style S10 fill:#fff9c4
    style S11 fill:#fff9c4
```

### 配置优先级

```mermaid
flowchart LR
    subgraph Priority["配置优先级 (高→低)"]
        direction TB
        P1["显示器ID"] --> P1A["CLIENT_DISPLAY_NAME (客户端)"]
        P1 --> P1B["config.output_name (服务端)"]
        P2["屏幕模式"] --> P2A["custom_screen_mode (客户端)"]
        P2 --> P2B["config.display_device_prep (服务端)"]
        P3["VDD决策"] --> P3A["session.use_vdd (客户端请求)"]
        P3 --> P3B["设备不可用 (自动检测)"]
        P3 --> P3C["VDD设备检测"]
    end
    
    P1A -.->|覆盖| P1B
    P2A -.->|覆盖| P2B
```
