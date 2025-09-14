import { fileURLToPath, URL } from 'node:url'
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

let header = fs.readFileSync(resolve(assetsSrcPath, 'template_header.html'))

// https://vitejs.dev/config/
export default defineConfig({
  resolve: {
    alias: {
      vue: 'vue/dist/vue.esm-bundler.js',
    },
  },
  plugins: [vue(), ViteEjsPlugin({ header })],
  root: resolve(assetsSrcPath),
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
            const fileName = facadeModuleId.split('/').pop().replace(/\.[^/.]+$/, '')
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
})
