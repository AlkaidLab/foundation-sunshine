import fs from 'fs'
import { resolve } from 'path'
import { foundationUiAliases } from './foundationUi.js'
import { defineConfig } from 'vite'
import { ViteEjsPlugin } from './vite-plugin-ejs.js'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath } from 'url'
import process from 'process'

/**
 * Before actually building the pages with Vite, we do an intermediate build step using ejs
 * Importing this separately and joining them using ejs
 * allows us to split some repeating HTML that cannot be added
 * by Vue itself (e.g. style/script loading, common meta head tags, Widgetbot)
 * The local EJS transform handles this automatically.
 */
let assetsSrcPath = 'src_assets/common/assets/web'
let assetsDstPath = 'build/assets/web'

if (process.env.SUNSHINE_BUILD_HOMEBREW) {
  console.log('Building for homebrew, using default paths')
} else {
  if (process.env.SUNSHINE_SOURCE_ASSETS_DIR) {
    console.log('Using srcdir from Cmake: ' + resolve(process.env.SUNSHINE_SOURCE_ASSETS_DIR, 'common/assets/web'))
    assetsSrcPath = resolve(process.env.SUNSHINE_SOURCE_ASSETS_DIR, 'common/assets/web')
  }
  if (process.env.SUNSHINE_ASSETS_DIR) {
    console.log('Using destdir from Cmake: ' + resolve(process.env.SUNSHINE_ASSETS_DIR, 'assets/web'))
    assetsDstPath = resolve(process.env.SUNSHINE_ASSETS_DIR, 'assets/web')
  }
}

const header = fs.readFileSync(resolve(assetsSrcPath, 'template_header.html'))
// the first frame of the sign-in page: an inline script (login-boot.js) with the bundled default record (login-boot-defaults.json, made by tools gen-login-boot-defaults from a real first start)
const bootDefaults = fs.existsSync(resolve(assetsSrcPath, 'login-boot-defaults.json')) ? fs.readFileSync(resolve(assetsSrcPath, 'login-boot-defaults.json'), 'utf8').trim() : 'null'
const bootScript = fs.readFileSync(resolve(assetsSrcPath, 'login-boot.js'), 'utf8').replace('/*DEFAULTS*/null', () => bootDefaults)

// https://vitejs.dev/config/
// foundation-ui is the shared Material 3 Expressive package (sibling checkout, see its README).
const foundationUi = process.env.FOUNDATION_UI_DIR || fileURLToPath(new URL('../../../ds5-ui-m3e', import.meta.url))

export default defineConfig({
  resolve: {
    dedupe: ['vue'],
    alias: [
      { find: /^vue$/, replacement: 'vue/dist/vue.esm-bundler.js' },
      ...foundationUiAliases(foundationUi),
    ],
  },
  plugins: [vue({ template: { compilerOptions: { isCustomElement: (tag) => tag.startsWith('md-') } } }), ViteEjsPlugin({ header, bootScript })],
  root: resolve(assetsSrcPath),
  preview: {
    port: 3000,
    host: '0.0.0.0',
    open: false,
  },
  build: {
    outDir: resolve(assetsDstPath),
    emptyOutDir: true,
    chunkSizeWarningLimit: 1000,
    // Vite 8 使用 rolldown 内核：多入口 HTML 必须放在 rolldownOptions.input
    // 放到 rollupOptions.input 会被忽略，导致除 index.html 外其它页面 404
    // Source: ds5-vite-build-20261031, Multi-Page App and build.rolldownOptions.
    rolldownOptions: {
      input: {
        apps: resolve(assetsSrcPath, 'apps.html'),
        downloads: resolve(assetsSrcPath, 'downloads.html'),
        config: resolve(assetsSrcPath, 'config.html'),
        dualsense: resolve(assetsSrcPath, 'dualsense.html'),
        index: resolve(assetsSrcPath, 'index.html'),
        login: resolve(assetsSrcPath, 'login.html'),
        password: resolve(assetsSrcPath, 'password.html'),
        pin: resolve(assetsSrcPath, 'pin.html'),
        troubleshooting: resolve(assetsSrcPath, 'troubleshooting.html'),
        welcome: resolve(assetsSrcPath, 'welcome.html'),
      },
      output: {
        // 优化chunk命名
        chunkFileNames: 'assets/[name]-[hash].js',
        // 优化入口文件命名（只影响 JS 文件）
        entryFileNames: 'assets/[name]-[hash].js',
        // 优化资源文件命名
        assetFileNames: (assetInfo) => {
          const name = assetInfo.name || ''
          const ext = name.split('.').pop()
          
          if (/\.(css)$/.test(name)) {
            return 'assets/[name]-[hash].[ext]'
          }
          if (/\.(woff2?|eot|ttf|otf)$/.test(name)) {
            return 'assets/fonts/[name]-[hash].[ext]'
          }
          if (/\.(png|jpe?g|gif|svg|webp|avif)$/.test(name)) {
            return 'assets/images/[name]-[hash].[ext]'
          }
          return 'assets/[name]-[hash].[ext]'
        },
      },
    },
    // 启用CSS代码分割
    cssCodeSplit: true,
    // 启用源码映射（生产环境可选）
    sourcemap: false,
    // 优化依赖预构建
    commonjsOptions: {
      include: [/node_modules/],
    },
  },
  // 优化依赖预构建
  optimizeDeps: {
    include: ['vue', 'vue-i18n', 'marked', 'nanoid', 'vuedraggable-es'],
  },
})
