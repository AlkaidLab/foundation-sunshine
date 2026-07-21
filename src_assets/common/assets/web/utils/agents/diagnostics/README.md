# Diagnostics Agent

`diagnostics` 是面向 Troubleshooting 的日志诊断 domain agent。它复用 `agents/core`，但只处理日志和诊断发现，不保存 AI 配置，也不直接调用 LLM。

## 当前能力

| Skill id | 阶段 | 默认 | 职责 |
| --- | --- | --- | --- |
| `diagnostics.logs.severity` | `log-analysis` | 开启 | 统计 fatal、error、warning 日志行，并保留少量证据。 |
| `diagnostics.logs.patterns` | `log-analysis` | 开启 | 检测编码器、显示捕获、网络连接、端口绑定和配置风险模式。 |
| `diagnostics.logs.remediation` | `log-analysis` | 开启 | 根据本地 findings 生成可执行修复建议。 |

## Context

```js
{
  logs: '',
  findings: [],
  suggestions: [],
  severitySummary: null,
  events: [],
  stats: {},
  options: runOptions,
}
```

`findings` 和 `suggestions` 会作为本地预诊断显示在 AI 日志诊断弹窗中，并作为上下文传给 `/api/ai/chat/completions`。这样即使 AI 配置不可用，用户仍能看到本地规则发现和基础修复建议。

## 边界

- LLM provider、model 和 key 仍由 control panel 的米塔页面统一配置。
- WebUI 诊断弹窗只消费共享 Sunshine AI proxy。
- 本地规则只提供可解释线索，不自动修复配置或重启服务。
