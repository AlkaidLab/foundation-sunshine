# Agent Core

`agents/core` 是 WebUI 里可复用的 agent/skill 底座。它不包含任何游戏、日志、配置或 UI 业务字段，只负责通用执行模型。

## 提供什么

- `createAgent()`：顺序运行 skills，支持 `enabledSkills` 选择、`stopOnSkillError` 中断和默认失败隔离。
- `createAgentSkill()`：校验 skill id 和 `run(context)`。
- `createSkillRegistry()`：注册扩展 skill，并生成对应 capability。
- capability helpers：获取默认启用项、可选择项、图标、文案和归一化后的启用列表。

## Context 约定

Core 只要求 context 可以被 skill 接收并返回。推荐每个 domain 保持这些通用字段：

```js
{
  events: [],
  stats: {},
  options: runOptions,
}
```

业务字段由 domain 自己定义，例如：

- `gameLibrary` 使用 `apps`。
- `diagnostics` 使用 `logs`、`findings` 和 `severitySummary`。

## 新建 Domain Agent

每个 domain agent 应该自己提供：

- 稳定的 agent id。
- 稳定的 skill id 常量。
- capability 列表和本地化文案。
- `createXxxSkill()` 包装，用于给错误信息加 domain 名称。
- `createDefaultXxxSkills()`。
- `createXxxAgent()`，通过 `createContext()` 定义自己的 context 形状。
- `registerXxxSkillExtension()`，如果允许外部扩展。

Core 不应该直接 import 任何 domain 文件。
