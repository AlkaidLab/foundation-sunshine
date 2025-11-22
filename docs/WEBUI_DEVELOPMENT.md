# WebUI 开发指南

Sunshine 包含一个现代化的 Web 控制界面，基于 Vue 3 和 Composition API 构建，遵循 Vue 最佳实践。

> **注意**: 本文档已更新以反映最新的项目结构优化。所有页面已重构为使用 Composition API 和模块化架构。

## 🛠️ 技术栈

- **前端框架**: Vue 3 + Composition API
- **构建工具**: Vite
- **UI 组件**: Bootstrap 5
- **图标库**: FontAwesome 6
- **国际化**: Vue-i18n 11 (Composition API 模式)
- **拖拽功能**: Vuedraggable 4

> **注意**: 本文档已更新以反映最新的项目结构优化。所有页面已重构为使用 Composition API 和模块化架构。

## 🚀 开发环境设置

### 1. 安装依赖

```bash
npm install
```

### 2. 开发命令

```bash
# 开发模式 - 实时构建和监听文件变化
npm run dev

# 开发服务器 - 启动HTTPS开发服务器 (推荐)
npm run dev-server

# 完整开发环境 - 包含模拟API服务
npm run dev-full

# 构建生产版本
npm run build

# 清理构建目录并重新构建
npm run build-clean

# 预览生产构建
npm run preview
```

### 3. 开发服务器特性

- **HTTPS支持**: 自动生成本地SSL证书
- **热重载**: 实时更新代码变更
- **代理配置**: 自动代理API请求到Sunshine服务
- **模拟数据**: 开发模式下提供模拟API响应
- **端口**: 默认运行在 `https://localhost:3000`

## 📁 项目结构

```
src_assets/common/assets/web/
├── views/                    # 页面组件（路由级组件）
│   ├── Home.vue             # 首页
│   ├── Apps.vue             # 应用管理页面
│   ├── Config.vue            # 配置管理页面
│   ├── Troubleshooting.vue  # 故障排除页面
│   ├── Pin.vue              # PIN 配对页面
│   ├── Password.vue         # 密码修改页面
│   └── Welcome.vue          # 欢迎页面
│
├── components/              # Vue 组件
│   ├── layout/              # 布局组件
│   │   ├── Navbar.vue       # 导航栏
│   │   └── PlatformLayout.vue # 平台布局组件
│   ├── common/              # 通用组件
│   │   ├── ThemeToggle.vue  # 主题切换
│   │   ├── ResourceCard.vue  # 资源卡片
│   │   ├── VersionCard.vue  # 版本信息卡片
│   │   ├── ErrorLogs.vue    # 错误日志组件
│   │   └── Locale.vue        # 语言组件
│   ├── SetupWizard.vue       # 设置向导
│   └── ...                  # 其他功能组件
│
├── composables/             # 组合式函数（可复用逻辑）
│   ├── useVersion.js        # 版本管理
│   ├── useLogs.js           # 日志管理
│   ├── useSetupWizard.js    # 设置向导逻辑
│   ├── useApps.js           # 应用管理
│   ├── useConfig.js         # 配置管理
│   ├── useTroubleshooting.js # 故障排除
│   ├── usePin.js            # PIN 配对
│   ├── useWelcome.js        # 欢迎页面
│   └── useTheme.js          # 主题管理
│
├── config/                  # 配置文件
│   ├── firebase.js          # Firebase 配置
│   └── i18n.js              # 国际化配置
│
├── services/                # API 服务
│   └── appService.js         # 应用服务
│
├── utils/                   # 工具函数
│   ├── constants.js         # 常量定义
│   ├── helpers.js           # 辅助函数
│   ├── validation.js        # 表单验证
│   ├── theme.js             # 主题工具
│   └── ...
│
├── styles/                  # 样式文件
│   ├── apps.css             # 应用页面样式
│   ├── welcome.css          # 欢迎页面样式
│   └── ...
│
├── public/                  # 静态资源
│   ├── assets/
│   │   ├── css/             # 全局样式
│   │   └── locale/          # 国际化文件
│   └── images/              # 图片资源
│
├── configs/                  # 配置页面子组件
│   └── tabs/                # 配置标签页组件
│
├── *.html                   # 页面入口文件（已简化）
└── init.js                  # 应用初始化
```

## 🎯 架构设计原则

### 1. 目录组织

- **views/**: 页面级组件，对应路由
- **components/layout/**: 布局相关组件（Navbar, PlatformLayout）
- **components/common/**: 通用可复用组件
- **components/**: 功能特定组件
- **composables/**: 可复用的业务逻辑
- **config/**: 配置文件
- **services/**: API 服务层
- **utils/**: 纯函数工具

### 2. 组件分类

#### 页面组件 (views/)
- 对应一个完整的页面
- 使用 Composition API (`<script setup>`)
- 组合多个子组件和 composables
- 处理页面级状态和生命周期

#### 布局组件 (components/layout/)
- 页面布局相关（如导航栏）
- 可跨页面复用

#### 通用组件 (components/common/)
- 高度可复用的 UI 组件
- 无业务逻辑或逻辑简单

#### 功能组件 (components/)
- 特定功能的组件
- 包含一定业务逻辑

### 3. Composables 设计

Composables 用于提取可复用的业务逻辑：

```javascript
// composables/useExample.js
import { ref, computed } from 'vue'

export function useExample() {
  const data = ref(null)
  const loading = ref(false)
  
  const computedValue = computed(() => {
    // 计算逻辑
  })
  
  const fetchData = async () => {
    // 数据获取逻辑
  }
  
  return {
    data,
    loading,
    computedValue,
    fetchData,
  }
}
```

## 📝 开发规范

### 1. 创建新页面

#### 步骤 1: 创建页面组件

```vue
<!-- views/NewPage.vue -->
<template>
  <div>
    <Navbar />
    <div class="container">
      <h1>{{ $t('newpage.title') }}</h1>
      <!-- 页面内容 -->
    </div>
  </div>
</template>

<script setup>
import Navbar from '../components/layout/Navbar.vue'
// 导入需要的 composables
import { useNewPage } from '../composables/useNewPage.js'

const {
  // 解构需要的状态和方法
} = useNewPage()
</script>

<style scoped>
/* 页面特定样式 */
</style>
```

#### 步骤 2: 创建 Composable（如需要）

```javascript
// composables/useNewPage.js
import { ref, computed } from 'vue'

export function useNewPage() {
  const data = ref(null)
  
  const fetchData = async () => {
    // 数据获取逻辑
  }
  
  return {
    data,
    fetchData,
  }
}
```

#### 步骤 3: 创建 HTML 入口文件

```html
<!-- newpage.html -->
<!DOCTYPE html>
<html lang="en" data-bs-theme="auto">
  <head>
    <%- header %>
  </head>

  <body id="app" v-cloak>
    <!-- Vue 应用挂载点 -->
  </body>

  <script type="module">
    import { createApp } from 'vue'
    import { initApp } from './init'
    import NewPage from './views/NewPage.vue'

    const app = createApp(NewPage)
    initApp(app)
  </script>
</html>
```

### 2. 使用 Composition API

**推荐使用 `<script setup>` 语法：**

```vue
<script setup>
import { ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'

const { t } = useI18n()
const count = ref(0)

const doubleCount = computed(() => count.value * 2)

onMounted(() => {
  // 初始化逻辑
})
</script>
```

### 3. 国际化使用

#### 在模板中

```vue
<template>
  <div>
    <!-- 使用 $t (通过 globalInjection) -->
    <h1>{{ $t('common.title') }}</h1>
    
    <!-- 或使用 t 函数（Composition API） -->
    <p>{{ t('common.description') }}</p>
  </div>
</template>

<script setup>
import { useI18n } from 'vue-i18n'
const { t } = useI18n()
</script>
```

#### 在 Composables 中

```javascript
import { useI18n } from 'vue-i18n'

export function useExample() {
  const { t } = useI18n()
  
  const showMessage = (key) => {
    alert(t(key))
  }
  
  return { showMessage }
}
```

### 4. 样式组织

- **全局样式**: `public/assets/css/` 或 `styles/`
- **组件样式**: 使用 `<style scoped>` 在组件内
- **页面特定样式**: 在对应的页面组件中

### 5. API 调用

使用 `services/` 目录组织 API 调用：

```javascript
// services/exampleService.js
export class ExampleService {
  static async getData() {
    const response = await fetch('/api/example')
    return response.json()
  }
  
  static async saveData(data) {
    const response = await fetch('/api/example', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data),
    })
    return response.json()
  }
}
```

## 🚀 开发流程

### 1. 开发新功能

1. **分析需求**：确定是页面、组件还是功能增强
2. **创建 Composables**：提取可复用的业务逻辑
3. **创建组件**：实现 UI 和交互
4. **创建页面**：组合组件和 composables
5. **添加路由**：创建 HTML 入口文件
6. **测试验证**：确保功能正常

### 2. 代码审查要点

- ✅ 是否遵循目录结构规范
- ✅ 是否使用 Composition API
- ✅ 业务逻辑是否提取到 composables
- ✅ 组件是否可复用
- ✅ 样式是否合理组织
- ✅ 是否添加了必要的错误处理

## 📚 示例代码

### 完整页面示例

```vue
<!-- views/Example.vue -->
<template>
  <div>
    <Navbar />
    <div class="container">
      <h1>{{ $t('example.title') }}</h1>
      
      <ExampleCard 
        v-for="item in items" 
        :key="item.id"
        :item="item"
        @action="handleAction"
      />
      
      <div v-if="loading" class="text-center">
        <div class="spinner-border" role="status">
          <span class="visually-hidden">Loading...</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted } from 'vue'
import Navbar from '../components/layout/Navbar.vue'
import ExampleCard from '../components/ExampleCard.vue'
import { useExample } from '../composables/useExample.js'
import { trackEvents } from '../config/firebase.js'

const {
  items,
  loading,
  fetchItems,
  handleAction,
} = useExample()

onMounted(async () => {
  trackEvents.pageView('example')
  await fetchItems()
})
</script>

<style scoped>
.container {
  padding: 1rem;
}
</style>
```

### Composables 示例

```javascript
// composables/useExample.js
import { ref, computed } from 'vue'
import { ExampleService } from '../services/exampleService.js'
import { trackEvents } from '../config/firebase.js'

export function useExample() {
  const items = ref([])
  const loading = ref(false)
  const error = ref(null)
  
  const itemCount = computed(() => items.value.length)
  
  const fetchItems = async () => {
    loading.value = true
    error.value = null
    try {
      items.value = await ExampleService.getItems()
      trackEvents.userAction('items_loaded', { count: items.value.length })
    } catch (err) {
      error.value = err.message
      trackEvents.errorOccurred('fetch_items', err.message)
    } finally {
      loading.value = false
    }
  }
  
  const handleAction = async (itemId) => {
    try {
      await ExampleService.performAction(itemId)
      await fetchItems() // 刷新列表
    } catch (err) {
      console.error('Action failed:', err)
    }
  }
  
  return {
    items,
    loading,
    error,
    itemCount,
    fetchItems,
    handleAction,
  }
}
```

## 🔧 配置说明

### i18n 配置

```javascript
// config/i18n.js
const i18n = createI18n({
  legacy: false,           // 使用 Composition API 模式
  locale: locale,
  fallbackLocale: 'en',
  messages: messages,
  globalInjection: true,   // 允许在模板中使用 $t
})
```

### Firebase 配置

```javascript
// config/firebase.js
import { initFirebase, trackEvents } from './config/firebase.js'

// 初始化
initFirebase()

// 使用
trackEvents.pageView('page_name')
trackEvents.userAction('action_name', { data })
```

## 🎨 样式指南

### 使用 Bootstrap 5

项目使用 Bootstrap 5 作为 UI 框架，优先使用 Bootstrap 组件和工具类。

### 自定义样式

- 组件特定样式使用 `<style scoped>`
- 全局样式放在 `styles/` 目录
- 使用 CSS 变量进行主题定制

## 📦 依赖管理

主要依赖：
- `vue` - Vue 3 框架
- `vue-i18n` - 国际化（Composition API 模式）
- `bootstrap` - UI 框架
- `vuedraggable` - 拖拽功能
- `marked` - Markdown 解析

## 🐛 调试技巧

1. **使用 Vue DevTools**：安装 Vue DevTools 浏览器扩展
2. **控制台日志**：使用 `console.log` 进行调试
3. **网络请求**：使用浏览器开发者工具查看 API 请求
4. **组件检查**：在 Vue DevTools 中检查组件状态

## 🔧 开发配置

### Vite 配置

- **开发配置**: `vite.dev.config.js` - 开发环境专用配置
- **生产配置**: `vite.config.js` - 生产构建配置
- **EJS模板**: 支持HTML模板预处理
- **路径别名**: 配置了Vue和Bootstrap的路径别名

### 代理配置

开发服务器包含以下代理设置：
- `/api/*` → `https://localhost:47990` (Sunshine API)
- `/steam-api/*` → Steam API服务
- `/steam-store/*` → Steam商店服务

## 🌍 国际化支持

- 支持多语言切换
- 基于 Vue-i18n 11 (Composition API 模式)
- 语言文件位于 `public/assets/locale/` 目录
- 配置在 `config/i18n.js` 中

## 🎨 主题系统

- 支持明暗主题切换
- 基于 CSS 变量实现
- 主题工具在 `utils/theme.js` 中
- 使用 `composables/useTheme.js` 在组件中管理主题

## 📱 响应式设计

- 基于 Bootstrap 5 的响应式布局
- 支持桌面端和移动端
- 优化的触摸交互体验

## 🧪 测试和调试

- 开发模式下启用源码映射
- 详细的代理请求日志
- 模拟 API 数据用于前端开发
- 使用 Vue DevTools 进行组件调试

## 📦 构建和部署

```bash
# 生产构建
npm run build

# 构建输出目录: build/assets/web/
# 包含所有静态资源和HTML文件
```

## 📖 相关资源

- [Vue 3 文档](https://vuejs.org/)
- [Vue I18n 文档](https://vue-i18n.intlify.dev/)
- [Bootstrap 5 文档](https://getbootstrap.com/docs/5.3/)
- [Composition API 指南](https://vuejs.org/guide/extras/composition-api-faq.html)
- [Vue I18n Composition API 模式](https://vue-i18n.intlify.dev/guide/advanced/composition.html)

## 🔄 迁移指南

### 从 Options API 迁移到 Composition API

如果遇到旧的 Options API 组件，可以按以下步骤迁移：

1. 将 `data()` 改为 `ref()` 或 `reactive()`
2. 将 `computed` 改为 `computed()`
3. 将 `methods` 改为普通函数
4. 将生命周期钩子改为组合式 API 版本
5. 使用 `<script setup>` 简化代码

### 示例迁移

**之前 (Options API):**
```javascript
export default {
  data() {
    return {
      count: 0
    }
  },
  computed: {
    double() {
      return this.count * 2
    }
  },
  methods: {
    increment() {
      this.count++
    }
  }
}
```

**之后 (Composition API):**
```javascript
<script setup>
import { ref, computed } from 'vue'

const count = ref(0)
const double = computed(() => count.value * 2)
const increment = () => count.value++
</script>
```

## ✅ 最佳实践检查清单

- [ ] 使用 Composition API (`<script setup>`)
- [ ] 业务逻辑提取到 composables
- [ ] 组件按功能分类到正确目录
- [ ] 样式使用 scoped 或放在 styles 目录
- [ ] 使用 TypeScript 类型（如适用）
- [ ] 添加错误处理
- [ ] 使用国际化 (`$t` 或 `t`)
- [ ] 添加必要的用户反馈
- [ ] 代码格式化统一
- [ ] 添加必要的注释

## 📋 快速参考

### 文件命名规范

- **页面组件**: `PascalCase.vue` (如 `Home.vue`, `Apps.vue`)
- **Composables**: `useXxx.js` (如 `useVersion.js`, `useApps.js`)
- **服务类**: `xxxService.js` (如 `appService.js`)
- **工具函数**: `camelCase.js` (如 `helpers.js`, `validation.js`)

### 导入路径规范

```javascript
// 页面组件
import Navbar from '../components/layout/Navbar.vue'

// Composables
import { useVersion } from '../composables/useVersion.js'

// 服务
import { AppService } from '../services/appService.js'

// 工具函数
import { debounce } from '../utils/helpers.js'

// 配置
import { trackEvents } from '../config/firebase.js'
```

### 常用 Composables

| Composable | 用途 | 返回内容 |
|-----------|------|---------|
| `useVersion` | 版本管理 | version, githubVersion, fetchVersions |
| `useLogs` | 日志管理 | logs, fatalLogs, fetchLogs |
| `useApps` | 应用管理 | apps, loadApps, save, editApp |
| `useConfig` | 配置管理 | config, save, apply |
| `useTheme` | 主题管理 | - |
| `usePin` | PIN 配对 | clients, unpairAll, save |

## 🎯 下一步

- 考虑添加 TypeScript 支持
- 考虑添加单元测试
- 考虑添加 E2E 测试
- 优化性能（懒加载、代码分割）

## 🤝 贡献指南

欢迎为 WebUI 贡献代码！请确保：

1. **遵循代码规范**
   - 使用 Composition API
   - 业务逻辑提取到 composables
   - 组件按功能分类

2. **代码质量**
   - 添加必要的错误处理
   - 使用国际化
   - 添加必要的注释

3. **测试验证**
   - 提交前运行构建命令确保无错误
   - 测试新功能在不同浏览器中的表现

4. **文档更新**
   - 更新相关文档
   - 添加必要的代码注释

## 📝 更新日志

### 最新更新 (2024)

- ✅ 所有页面重构为 Composition API
- ✅ 业务逻辑提取到 composables
- ✅ 组件按功能重新组织
- ✅ 配置文件统一管理
- ✅ Vue I18n 迁移到 Composition API 模式
- ✅ 简化所有 HTML 入口文件
