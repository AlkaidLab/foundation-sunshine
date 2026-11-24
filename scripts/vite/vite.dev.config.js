import fs from 'fs'
import { resolve } from 'path'
import { fileURLToPath } from 'url'
import { defineConfig } from 'vite'
import { ViteEjsPlugin } from './vite-plugin-ejs.js'
import { foundationUiAliases } from './foundationUi.js'
import vue from '@vitejs/plugin-vue'
import mkcert from 'vite-plugin-mkcert'

// 静态资源路径
const assetsSrcPath = 'src_assets/common/assets/web'
// 读取开发环境模板头文件
const header = fs.readFileSync(resolve(assetsSrcPath, 'template_header_dev.html'))
// 登录页首帧的内联脚本（login-boot.js + 默认记录），与生产构建同一来源（vite.config.js）
const bootDefaults = fs.existsSync(resolve(assetsSrcPath, 'login-boot-defaults.json')) ? fs.readFileSync(resolve(assetsSrcPath, 'login-boot-defaults.json'), 'utf8').trim() : 'null'
const bootScript = fs.readFileSync(resolve(assetsSrcPath, 'login-boot.js'), 'utf8').replace('/*DEFAULTS*/null', () => bootDefaults)

// 支持无.html后缀访问的中间件
function htmlExtensionMiddleware(htmlFiles) {
  return (req, res, next) => {
    if (req.method !== 'GET') return next()
    const url = req.url.split('?')[0]
    if (url.endsWith('/') || url.includes('.')) return next()
    const page = url.replace(/^\//, '')
    if (htmlFiles.includes(page)) {
      res.writeHead(302, { Location: `${url}.html` })
      res.end()
      return
    }
    next()
  }
}

// 需要支持的html页面
const htmlPages = ['apps', 'downloads', 'config', 'index', 'password', 'pin', 'troubleshooting', 'welcome']

// 代理配置复用函数
function createProxyLogger(prefix, target, rewritePath) {
  return {
    target,
    changeOrigin: true,
    secure: true,
    rewrite: (path) => path.replace(rewritePath, ''),
    configure(proxy) {
      proxy.on('proxyReq', (proxyReq, req) => {
        console.log(`${prefix}请求:`, req.method, req.url, '-> ' + target + req.url.replace(rewritePath, ''))
      })
      proxy.on('proxyRes', (proxyRes, req) => {
        console.log(`✅ ${prefix}响应:`, req.url, '状态码:', proxyRes.statusCode)
      })
    },
  }
}

// 无后端时的 mock 响应：登录放行（config 不带凭据也 200）、应用列表、日志、重启
const mockResponses = {
  '/api/config': {
    platform: 'windows',
    version: '0.21.0-dev',
    notify_pre_releases: true,
    locale: 'zh',
    sunshine_name: 'Sunshine Development Server',
    min_log_level: 2,
    port: 47990,
    upnp: true,
    enable_ipv6: false,
    origin_web_ui_allowed: 'pc',
  },
  '/api/auth/session': {
    authenticated: true,
    local: false,
    setup_required: false,
    username: 'mock-user',
    session_id: 'mock-session',
    created_at: 0,
    last_seen: 0,
    remember: true,
    csrf_token: 'mock-csrf',
  },
  '/api/apps': {
    apps: [
      {
        name: 'Steam',
        output: 'steam-output',
        cmd: 'steam.exe',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
      {
        name: 'Notepad',
        output: 'notepad-output',
        cmd: 'notepad.exe',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
      // 极端场景：无空格超长名（测省略号/换行）、带全角与 emoji 的名字、超长命令行、数量撑满网格换行
      {
        name: 'AnExtremelyLongApplicationNameWithoutAnySpacesAtAllThatJustKeepsGoingAndGoingForeverAndEverUntilItCannotFitAnywhere',
        output: 'long-output',
        cmd: 'C:\\Program Files (x86)\\Some Vendor\\Deeply Nested Folder Structure\\Yet Another Level\\bin\\vendor-tool.exe --mode=extended --profile="C:\\Users\\someone\\Documents\\Vendor Configurations\\Profiles\\default-v2-extended.json"',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
      {
        name: '🎮🎮🎮 全角名字テスト・验证ＡＢＣＤＥＦＧ 🎮🎮🎮',
        output: 'emoji-output',
        cmd: 'game.exe',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
      {
        name: 'Third Game',
        output: 'third-output',
        cmd: 'game3.exe',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
      {
        name: 'Fourth Game',
        output: 'fourth-output',
        cmd: 'game4.exe',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
      {
        name: 'Fifth Game',
        output: 'fifth-output',
        cmd: 'game5.exe',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
      {
        name: 'Sixth Game',
        output: 'sixth-output',
        cmd: 'game6.exe',
        'exclude-global-prep-cmd': false,
        elevated: false,
        'auto-detach': true,
        'wait-all': true,
        'exit-timeout': 5,
        'prep-cmd': [],
        'menu-cmd': [],
        detached: [],
        'image-path': '',
        'working-dir': '',
      },
    ],
  },
  '/api/logs': 'Sunshine Development Server - Mock Logs\n[INFO] Server started\n[INFO] Development mode enabled\n',
  '/api/restart': { status: 'ok', message: 'Restart initiated (mock)' },
  // 已配对设备：三种类型出三个分组（组标题），一个停用行（stopped 布局），一个长名字（省略号）
  '/api/clients/list': {
    status: true,
    named_certs: [
      { name: 'Pixel 9 Pro', uuid: 'mock-phone', os: 'Android', enabled: true, streaming: false },
      { name: 'Living Room PC（客厅电脑，一个特别长的名字用来验证省略号）', uuid: 'mock-pc', os: 'Windows', enabled: true, streaming: true },
      { name: 'Steam Deck 掌机', uuid: 'mock-handheld', os: 'SteamOS', enabled: false, streaming: false },
    ],
  },
}

export default defineConfig(({ mode }) => {
  // foundation-ui 的检出位置（与生产构建同一规则，见 vite.config.js / foundationUi.js）
  const foundationUi = process.env.FOUNDATION_UI_DIR || fileURLToPath(new URL('../../../ds5-ui-m3e', import.meta.url))
  // /api 与 /boxart 的后端：默认真机 Sunshine（47990）。`--mode mock`（npm run dev-server -- --mode mock）指向不可达地址，
  // 页面落到上面的内置 mock 响应——无需登录、无需真机即可走查 UI；SUNSHINE_DEV_API_TARGET 覆盖后端地址。
  const mockOnly = mode === 'mock'
  const apiTarget = mockOnly ? 'http://localhost:9' : (process.env.SUNSHINE_DEV_API_TARGET || 'https://localhost:47990')
  const proxy = {
    '/steam-api': createProxyLogger('🎮 Steam API', 'https://api.steampowered.com', /^\/steam-api/),
    '/steam-store': createProxyLogger('🛒 Steam Store', 'https://store.steampowered.com', /^\/steam-store/),
    '/boxart': {
      target: apiTarget,
      changeOrigin: true,
      secure: false,
      configure(proxy) {
        proxy.on('error', (err, req, res) => {
          console.log('❌ Boxart 代理错误:', err.message)
          if (!res.headersSent) {
            res.writeHead(500, { 'Content-Type': 'text/plain' })
          }
          res.end('Boxart proxy error: ' + err.message)
        })
        proxy.on('proxyReq', (proxyReq, req) => {
          console.log('🖼️  Boxart 请求:', req.method, req.url, '-> ' + apiTarget + req.url)
        })
        proxy.on('proxyRes', (proxyRes, req) => {
          console.log('✅ Boxart 响应:', req.url, '状态码:', proxyRes.statusCode)
          // 清理可能有问题的响应头
          delete proxyRes.headers['content-encoding']
        })
      },
    },
    '/api': {
      target: apiTarget,
      changeOrigin: true,
      secure: false,
      configure(proxy) {
        proxy.on('error', (err, req, res) => {
          console.log('API proxy error:', err.message)
          // 如果响应头已发送，不能再次发送
          if (res.headersSent) {
            return
          }

          // 只有 --mode mock 才用样例数据顶替后端；联调真机时的断连要如实报错，
          // 否则样例（如已配对设备）会通过结构校验，把真实列表顶掉还清掉连接错误
          if (!mockOnly) {
            res.writeHead(502, { 'Content-Type': 'application/json' })
            res.end(JSON.stringify({ error: 'Backend unreachable: ' + err.message }))
            return
          }

          // mock 模式：返回 mock 响应，让页面无需登录也能渲染
          if (req.url === '/api/logs') {
            const mockData = mockResponses[req.url] || 'No logs available (mock)'
            res.writeHead(200, { 'Content-Type': 'text/plain' })
            res.end(typeof mockData === 'string' ? mockData : String(mockData))
          } else {
            const mockData = mockResponses[req.url] || { error: 'Mock endpoint not found' }
            res.writeHead(200, { 'Content-Type': 'application/json' })
            res.end(JSON.stringify(mockData))
          }
        })
        proxy.on('proxyReq', (proxyReq, req) => {
          console.log('🔗 代理请求:', req.method, req.url, '-> ' + apiTarget + req.url)
        })
        proxy.on('proxyRes', (proxyRes, req) => {
          console.log('✅ 代理响应:', req.url, '状态码:', proxyRes.statusCode)
        })
      },
    },
  }

  return {
    resolve: {
      alias: [
        { find: /^vue$/, replacement: 'vue/dist/vue.esm-bundler.js' },
        ...foundationUiAliases(foundationUi),
      ],
    },
    plugins: [
      vue(),
      mkcert(),
      ViteEjsPlugin({
        header,
        bootScript,
        sunshineVersion: {
          version: '0.21.0-dev',
          release: 'development',
          commit: 'dev-build',
        },
      }),
      {
        name: 'html-extension-middleware',
        configureServer(server) {
          server.middlewares.use(htmlExtensionMiddleware(htmlPages))
        },
      },
    ],
    root: resolve(assetsSrcPath),
    server: {
      https: true,
      port: 3000,
      host: '0.0.0.0',
      open: true,
      cors: true,
      // HMR 配置：确保 WebSocket 直接连接到 Vite 服务器，而不是通过代理
      hmr: {
        protocol: 'wss',
        host: 'localhost',
        port: 3000,
      },
      proxy,
      fs: {
        allow: [resolve('node_modules'), resolve(assetsSrcPath), resolve('.'), resolve(foundationUi)],
      },
    },
    build: {
      chunkSizeWarningLimit: 1000, // 提高警告阈值到1MB
      rolldownOptions: {
        input: htmlPages.reduce((acc, name) => {
          acc[name] = resolve(assetsSrcPath, `${name}.html`)
          return acc
        }, {}),
        output: {
          advancedChunks: {
            groups: [
              // 将Vue相关库分离到单独的chunk
              { name: 'vue-vendor', test: /[\\/]node_modules[\\/](vue|vue-i18n)[\\/]/ },
              // 将Bootstrap和FontAwesome分离
              // 将其他第三方库分离
              { name: 'utils-vendor', test: /[\\/]node_modules[\\/](marked|nanoid|vuedraggable-es)[\\/]/ },
            ],
          },
          // 优化chunk命名
          chunkFileNames: (chunkInfo) => {
            const facadeModuleId = chunkInfo.facadeModuleId
            if (facadeModuleId) {
              const fileName = facadeModuleId
                .split('/')
                .pop()
                .replace(/\.[^/.]+$/, '')
              return `assets/${fileName}-[hash].js`
            }
            return 'assets/[name]-[hash].js'
          },
          // 优化资源文件命名
          assetFileNames: (assetInfo) => {
            const info = assetInfo.name.split('.')
            const ext = info[info.length - 1]
            if (/\.(css)$/.test(assetInfo.name)) {
              return `assets/[name]-[hash].${ext}`
            }
            if (/\.(woff2?|eot|ttf|otf)$/.test(assetInfo.name)) {
              return `assets/fonts/[name]-[hash].${ext}`
            }
            if (/\.(png|jpe?g|gif|svg|webp|avif)$/.test(assetInfo.name)) {
              return `assets/images/[name]-[hash].${ext}`
            }
            return `assets/[name]-[hash].${ext}`
          },
        },
      },
    },
    define: {
      __DEV__: true,
      __PROD__: false,
      __SUNSHINE_VERSION__: JSON.stringify({
        version: '0.21.0-dev',
        release: 'development',
        commit: 'dev-build',
      }),
    },
    css: {
      devSourcemap: true,
    },
  }
})
