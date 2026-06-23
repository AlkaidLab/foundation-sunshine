# Library Maintenance Agent

`libraryMaintenance` 是第一个游戏资源搜刮之外的 domain agent，用来验证 `agents/core` 可以承载其他能力。它检查 Sunshine 应用库本身的维护问题，不依赖 LLM。

## 当前能力

| Skill id | 阶段 | 默认 | 职责 |
| --- | --- | --- | --- |
| `library.duplicates.launch-target` | `quality` | 开启 | 找出多个应用使用同一启动命令和工作目录的情况。 |
| `library.entries.health` | `quality` | 开启 | 找出缺少显示名称、启动命令或封面的应用条目。 |

## Context

```js
{
  apps: [],
  issues: [],
  events: [],
  stats: {},
  options: runOptions,
}
```

`issues` 是面向后续 UI 或 AI 修复建议的结构化问题列表：

```js
{
  id: 'entry:0:missing-command',
  type: 'missing-command',
  severity: 'error',
  skillId: 'library.entries.health',
  appIndexes: [0],
  message: 'Application entry is missing a launch command',
  labels: { zh: '应用条目缺少启动命令' },
  evidence: {},
}
```

## 使用方式

```js
import {
  runLibraryMaintenanceAgent,
} from './libraryMaintenanceAgent.js'

const result = await runLibraryMaintenanceAgent(apps)
console.log(result.issues)
```

调用方可以通过 `enabledSkills` 只运行部分维护能力：

```js
await runLibraryMaintenanceAgent(apps, {
  enabledSkills: ['library.entries.health'],
})
```

## 后续接入方向

- 在 Apps 页面增加“应用库体检”入口，展示 `issues`。
- 新增可选 AI skill，把 `issues` 转成修复建议或批量修复计划。
- 对失效路径、无效封面路径、重复名称、启动目录不存在等问题增加更多本地检查。
