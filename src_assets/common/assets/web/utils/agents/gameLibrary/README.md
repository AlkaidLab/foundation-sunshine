# 游戏资源搜刮 Agent / Skills 体系

这套代码负责在 WebUI 的应用扫描流程中，对扫描到的游戏资源做 AI 辅助整理：复用用户已确认的修正、清洗游戏标题、生成封面搜索词、匹配封面，并把低置信度结果留给用户复核。

## 分层设计

- 通用底座：`utils/agents/core/agentCore.js` 提供 agent runner、skill 校验、capability registry、默认启用能力计算和用户选择归一化。
- 领域适配：`utils/agents/gameLibrary/gameLibraryCuratorAgent.js` 定义游戏资源搜刮的 skill id、capability 文案、默认 skills 和 `apps` context。
- 领域能力：`skills/` 下的每个文件只实现一个游戏资源处理能力。
- 领域策略：`policies/` 放不依赖 UI 的判断规则，例如复核队列阈值。

游戏资源之外的新能力不应该塞进 `gameLibrary`。如果要接入日志诊断、配置建议、库维护等能力，应新增自己的 domain agent，并复用 `agents/core`。

## 入口与边界

- UI 入口：`views/Apps.vue` 展示扫描结果和用户可选的 AI skill 开关。
- 组合逻辑：`composables/useApps.js` 调用扫描能力，并运行 `runGameLibraryCuratorAgent()`。
- Agent 入口：`utils/agents/gameLibrary/gameLibraryCuratorAgent.js` 注册游戏能力，底层执行由 `utils/agents/core/agentCore.js` 完成。
- AI 请求：WebUI 只调用 `/api/ai/chat/completions`，不保存独立 LLM 配置。
- 配置入口：LLM provider、base URL、model、API key 等配置统一由 control panel 的米塔页面维护，并同步到 Sunshine 后端 `/api/ai/config`。

不要在 WebUI 扫描页新增第二套 AI 配置入口。扫描页只负责选择启用哪些 game-library skills。

## 当前数据流

1. `scanDirectory()` 或 `scanGameLibraries()` 得到扫描结果。
2. `applyGameLibraryOverrides()` 先套用用户此前确认过的名称和封面。
3. `withScanKeys()` 给本轮扫描结果加 `__scan-key`，用于异步更新时稳定定位条目。
4. `runGameLibraryCuratorAgent()` 根据用户启用的 skill id 顺序运行 skills。
5. `onTitlesEnhanced()` 和 `onCoverResolved()` 增量更新扫描结果弹窗。
6. 用户快速添加、批量添加或编辑保存时，`rememberGameLibraryApp()` 记录本次确认结果，供下次扫描复用。

## 内置 Skills

| Skill id | 阶段 | 默认 | 用户可选 | 职责 |
| --- | --- | --- | --- | --- |
| `game.scan.memory` | `memory` | 开启 | 否 | 应用已确认的扫描覆盖记录，并在用户确认后学习新记录。 |
| `game.title.normalize` | `metadata` | 开启 | 是 | 通过 LLM 清洗标题、输出规范名、搜索词和名称置信度。 |
| `game.cover.select` | `asset` | 开启 | 是 | 搜索候选封面，再用 LLM 或本地 fallback 选择最佳封面。 |

必选能力用 `required: true` 标记。`normalizeGameLibrarySkillIds()` 会保证 required skill 始终启用，并过滤未知 id。

### 封面证据校准

`game.cover.select` 不直接信任模型给出的封面置信度。`coverSelectionAi.js` 会先计算候选封面与规范名、显示名、原始名、搜索词之间的本地匹配证据：

- `exact-title`：候选标题与已知名称完全一致。
- `prefix-title` / `contains-title`：候选标题与已知名称存在强前缀或包含关系。
- `token-overlap`：标题 token 高度重合。
- `source-prior`：只有来源先验，没有足够标题证据。

AI 选择封面后，`calibrateCoverConfidence()` 会用这份证据校准 `ai-cover-confidence`，并写入 `cover-match-confidence`、`cover-match-relation`、`cover-match-reason`。这样模型理解语义，本地证据负责防止同名游戏、DLC、工具、原声带等弱匹配以高置信度通过。审核队列会优先使用 `ai-cover-confidence`，缺失时回退到 `cover-match-confidence`。

## Skill 契约

每个 skill 至少需要：

```js
{
  id: 'game.example.skill',
  type: 'metadata',
  label: 'Example skill',
  async run(context) {
    return {
      ...context,
      apps: nextApps,
      stats: { ...(context.stats || {}), exampleCount },
    }
  },
}
```

`context` 字段约定：

- `apps`：当前扫描结果数组。skill 应返回新数组或等价的更新结果。
- `events`：调试事件数组。skill 可以 push `{ skillId, type, ... }`。
- `stats`：聚合统计，例如 `titleChanges`、`coversFound`、`skillFailures`。
- `options`：运行时回调，例如 `onTitlesEnhanced`、`onCoverResolved`、`onSkillError`。

Agent 默认会捕获单个 skill 的异常，记录 `skill:error` 事件并继续后续 skill。只有传入 `stopOnSkillError: true` 时才会中断整轮。

## 新增其他 Domain Agent

新增游戏资源之外的能力时，优先建立独立目录，例如：

```text
utils/agents/diagnostics/
utils/agents/configAdvisor/
```

推荐结构：

```js
import {
  createAgent,
  createAgentSkill,
  createSkillRegistry,
} from '../core/agentCore.js'

export const DIAGNOSTICS_AGENT_ID = 'diagnostics'

export function createDiagnosticsSkill(definition) {
  return createAgentSkill(definition, {
    skillSubject: 'Diagnostics skills',
    runSubject: 'Diagnostics skill',
  })
}

const registry = createSkillRegistry({
  baseCapabilities: DIAGNOSTICS_CAPABILITIES,
  createSkill: createDiagnosticsSkill,
  duplicateMessage: 'Diagnostics skill already registered',
})

export function createDiagnosticsAgent(options = {}) {
  return createAgent({
    id: DIAGNOSTICS_AGENT_ID,
    skills: options.skills || createDefaultDiagnosticsSkills(),
    createContext(input, runOptions) {
      return { input, events: [], stats: {}, options: runOptions }
    },
  })
}
```

每个 domain agent 应自己定义 context 形状、capability 文案、默认启用规则和 UI 接入点。通用 core 不应该知道具体业务字段。

## 新增内置 Skill

1. 在 `skills/` 下新增 `xxxSkill.js`，导出稳定的 `XXX_SKILL_ID` 和 `createXxxSkill()`。
2. 在 `gameLibraryCuratorAgent.js` 中导入并加入：
   - `GAME_LIBRARY_SKILL_IDS`
   - `GAME_LIBRARY_AGENT_CAPABILITIES`
   - `createDefaultGameLibrarySkills()`
3. 给 capability 填好：
   - `stage`
   - Font Awesome `icon`
   - 英文 `label`
   - 中文 `labels.zh`
   - `defaultEnabled`
   - `userSelectable`
   - 必要时 `required`
4. 在 `tests/gameLibraryCuratorAgent.test.js` 增加顺序、选择、失败隔离、label/icon 和默认启用测试。
5. 如涉及 UI 文案或状态，补充 `Apps.vue` / `ScanResultModal.vue` 的行为验证。

## 注册扩展 Skill

实验性或外部能力可以通过 `registerGameLibrarySkillExtension()` 注册，不需要改默认内置列表：

```js
import {
  registerGameLibrarySkillExtension,
} from './gameLibraryCuratorAgent.js'

const unregister = registerGameLibrarySkillExtension({
  skill: {
    id: 'game.example.annotate',
    type: 'metadata',
    label: 'Example annotation',
    async run(context) {
      return {
        ...context,
        apps: context.apps.map((app) => ({ ...app, example: true })),
      }
    },
  },
  capability: {
    icon: 'fa-vial',
    label: 'Example annotation',
    labels: { zh: '示例标注' },
    defaultEnabled: false,
    userSelectable: true,
  },
})
```

扩展 id 必须全局唯一。测试结束或插件卸载时调用 `unregister()`，避免污染后续运行。

## AI Prompt 与语言

Prompt 语言由 `aiLocale.js` 根据当前页面语言解析：

- `getCurrentLocale()` 优先读 `document.documentElement.lang`，其次读浏览器语言。
- `buildLocalizedInstruction()` 要求模型用当前语言输出面向用户的短理由。
- 标题清洗和封面选择 prompt 都必须返回 JSON，不允许 markdown。

维护 prompt 时要优先约束输出 schema、置信度和不确定时的保守行为。不要把 provider、model、key 等配置写进 prompt 或扫描页。

## 缓存与复核

- `aiCache.js` 使用 localStorage，访问失败时自动降级到内存缓存。
- 元数据缓存 namespace：`game-metadata:v1`。
- 封面选择缓存 namespace：`cover-selection:v1`。
- 调整 prompt、schema 或影响结果含义时，要提升 cache version。
- `reviewQueuePolicy.js` 负责判断是否进入复核队列。默认阈值：
  - 名称置信度低于 `0.82`
  - 封面置信度低于 `0.7`
  - 游戏缺少规范名或封面

## 容错原则

- 扫描结果应先展示，再异步增强，AI 不可用时不能阻塞用户添加应用。
- 单个 batch 或单个封面候选失败时，应跳过该部分并保留原始结果。
- 用户确认过的 `user-override` 和已有封面优先级最高。
- 异步更新必须用 `__scan-key` 或 `getGameResourceKey()` 稳定定位，避免重复名称、重复命令导致错位。

## 维护检查清单

改动这套体系后至少运行：

```powershell
npm run test:webui
npm run build
git diff --check
```

重点测试文件：

- `tests/agentCore.test.js`
- `tests/gameLibraryCuratorAgent.test.js`
- `tests/gameMetadataAi.test.js`
- `tests/aiCache.test.js`
- `tests/scanOverrides.test.js`

如果改了 control panel 的 AI 配置或 provider preset，还要在 `src_assets/common/sunshine-control-panel` 内运行对应构建和测试命令。
