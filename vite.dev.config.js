import fs from 'fs'
import { resolve } from 'path'
import { defineConfig } from 'vite'
import { ViteEjsPlugin } from 'vite-plugin-ejs'
import vue from '@vitejs/plugin-vue'
import mkcert from 'vite-plugin-mkcert'

// 静态资源路径
const assetsSrcPath = 'src_assets/common/assets/web'
// 读取开发环境模板头文件
const header = fs.readFileSync(resolve(assetsSrcPath, 'template_header_dev.html'))

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
const htmlPages = ['apps', 'config', 'index', 'password', 'pin', 'troubleshooting', 'welcome']

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

export default defineConfig({
  resolve: {
    alias: {
      vue: 'vue/dist/vue.esm-bundler.js',
      '@fortawesome/fontawesome-free': resolve('node_modules/@fortawesome/fontawesome-free'),
      bootstrap: resolve('node_modules/bootstrap'),
    },
  },
  plugins: [
    vue(),
    mkcert(),
    ViteEjsPlugin({
      header,
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
    proxy: {
      '/steam-api': createProxyLogger('🎮 Steam API', 'https://api.steampowered.com', /^\/steam-api/),
      '/steam-store': createProxyLogger('🛒 Steam Store', 'https://store.steampowered.com', /^\/steam-store/),
      '/boxart': {
        target: 'https://localhost:47990',
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
            console.log('🖼️  Boxart 请求:', req.method, req.url, '-> https://localhost:47990' + req.url)
          })
          proxy.on('proxyRes', (proxyRes, req) => {
            console.log('✅ Boxart 响应:', req.url, '状态码:', proxyRes.statusCode)
            // 清理可能有问题的响应头
            delete proxyRes.headers['content-encoding']
          })
        },
      },
      '/api': {
        target: 'https://localhost:47990',
        changeOrigin: true,
        secure: false,
        configure(proxy) {
          proxy.on('error', (err, req, res) => {
            console.log('API proxy error:', err.message)
            // 如果响应头已发送，不能再次发送
            if (res.headersSent) {
              return
            }

            const mockResponses = {
              '/api/config': {
                platform: 'windows',
                version: '0.21.0-dev',
                notify_pre_releases: true,
                locale: 'zh_CN',
                sunshine_name: 'Sunshine Development Server',
                min_log_level: 2,
                port: 47990,
                upnp: true,
                enable_ipv6: false,
                origin_web_ui_allowed: 'pc',
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
                ],
              },
              '/api/logs':
                'Sunshine Development Server - Mock Logs\n[INFO] Server started\n[INFO] Development mode enabled\n',
              '/api/restart': { status: 'ok', message: 'Restart initiated (mock)' },
            }

            // 处理特殊端点
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
            console.log('🔗 代理请求:', req.method, req.url, '-> https://localhost:47990' + req.url)
          })
          proxy.on('proxyRes', (proxyRes, req) => {
            console.log('✅ 代理响应:', req.url, '状态码:', proxyRes.statusCode)
          })
        },
      },
    },
    fs: {
      allow: [resolve('node_modules'), resolve(assetsSrcPath), resolve('.')],
    },
  },
  build: {
    chunkSizeWarningLimit: 1000, // 提高警告阈值到1MB
    rollupOptions: {
      input: htmlPages.reduce((acc, name) => {
        acc[name] = resolve(assetsSrcPath, `${name}.html`)
        return acc
      }, {}),
      output: {
        manualChunks: {
          // 将Vue相关库分离到单独的chunk
          'vue-vendor': ['vue', 'vue-i18n'],
          // 将Bootstrap和FontAwesome分离
          'ui-vendor': ['bootstrap', '@fortawesome/fontawesome-free', '@popperjs/core'],
          // 将其他第三方库分离
          'utils-vendor': ['marked', 'nanoid', 'vuedraggable'],
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
})
