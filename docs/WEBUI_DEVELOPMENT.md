# WebUI开发指南

Sunshine包含一个现代化的Web控制界面，基于Vue 3和Vite构建。

## 🛠️ 技术栈
- **前端框架**: Vue 3.5.18 + Composition API
- **构建工具**: Vite 4.5.14
- **UI组件**: Bootstrap 5.3.3
- **图标库**: FontAwesome 6.6.0
- **国际化**: Vue-i18n 11.1.11
- **拖拽功能**: Vuedraggable 4.1.0

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
├── components/          # Vue组件
├── configs/            # 配置文件
├── locale/             # 国际化文件
├── public/             # 静态资源
├── services/           # API服务
├── styles/             # 样式文件
├── utils/              # 工具函数
├── *.html              # 页面模板
├── *.vue               # Vue单文件组件
└── *.js                # JavaScript模块
```

## 🔧 开发配置

### Vite配置
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
- 基于Vue-i18n实现
- 语言文件位于 `locale/` 目录

## 🎨 主题系统
- 支持明暗主题切换
- 基于CSS变量实现
- 主题配置在 `theme.js` 中

## 📱 响应式设计
- 基于Bootstrap 5的响应式布局
- 支持桌面端和移动端
- 优化的触摸交互体验

## 🧪 测试和调试
- 开发模式下启用源码映射
- 详细的代理请求日志
- 模拟API数据用于前端开发

## 📦 构建和部署
```bash
# 生产构建
npm run build

# 构建输出目录: build/assets/web/
# 包含所有静态资源和HTML文件
```

## 🔗 相关链接
- [Vue 3官方文档](https://vuejs.org/)
- [Vite官方文档](https://vitejs.dev/)
- [Bootstrap 5文档](https://getbootstrap.com/docs/5.3/)
- [FontAwesome图标库](https://fontawesome.com/)

## 🤝 贡献指南
欢迎为WebUI贡献代码！请确保：
1. 遵循现有的代码风格
2. 添加必要的测试
3. 更新相关文档
4. 提交前运行构建命令确保无错误
