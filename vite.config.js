import fs from 'fs'
import { resolve } from 'path'
import { defineConfig } from 'vite'
import { ViteEjsPlugin } from 'vite-plugin-ejs'
import vue from '@vitejs/plugin-vue'
import process from 'process'

/**
 * Before actually building the pages with Vite, we do an intermediate build step using ejs
 * Importing this separately and joining them using ejs
 * allows us to split some repeating HTML that cannot be added
 * by Vue itself (e.g. style/script loading, common meta head tags, Widgetbot)
 * The vite-plugin-ejs handles this automatically
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

// https://vitejs.dev/config/
export default defineConfig({
  resolve: {
    alias: {
      vue: 'vue/dist/vue.esm-bundler.js',
    },
  },
  plugins: [vue(), ViteEjsPlugin({ header })],
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
    rollupOptions: {
      input: {
        apps: resolve(assetsSrcPath, 'apps.html'),
        config: resolve(assetsSrcPath, 'config.html'),
        index: resolve(assetsSrcPath, 'index.html'),
        password: resolve(assetsSrcPath, 'password.html'),
        pin: resolve(assetsSrcPath, 'pin.html'),
        troubleshooting: resolve(assetsSrcPath, 'troubleshooting.html'),
        welcome: resolve(assetsSrcPath, 'welcome.html'),
      },
      output: {
        // 优化chunk命名
        chunkFileNames: 'assets/[name]-[hash].js',
        // 优化入口文件命名
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
    include: ['vue', 'vue-i18n', 'bootstrap', '@popperjs/core', 'marked', 'nanoid', 'vuedraggable'],
  },
})
