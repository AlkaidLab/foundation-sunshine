import { basename, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { normalizePath } from 'vite'
import { parse } from 'vue/compiler-sfc'

// These modules travel together on a cold start. Keep route-specific charts and
// settings controls in separate groups, and leave other features to automatic splitting.
export function foundationChunkGroups(uiDir, assetsDir) {
  const ui = normalizePath(resolve(uiDir, 'src')) + '/'
  const web = normalizePath(resolve(assetsDir)) + '/'
  const names = new Map()
  function add(group, root, paths) {
    for (const path of paths) names.set(root + path, group)
  }

  add('web-bootstrap', ui, ['locale/texts/index.js', 'locale/texts/resolve.js'])
  add('web-bootstrap', web, ['config/localePacks.js', 'utils/apiFetch.js'])
  names.set('\0vite/preload-helper.js', 'web-bootstrap')

  add('shell-common', ui, ['a11y/modalLayer.js', 'dialog/store.js', 'vue/MPageLoading.vue'])
  add('shell-common', web, ['composables/askDialog.js', 'composables/useShellLive.js', 'utils/shellLive.js'])
  add('sheet-controls', ui + 'vue/', [
    'MSheet.vue', 'MIconButton.vue', 'MSeg.vue', 'useModalBridge.js', 'useHistoryClose.js', 'warmDialogRaster.js',
  ])
  add('value-pickers', ui + 'vue/', ['MChoiceItem.vue', 'MPickSheet.vue', 'MValueItem.vue', 'MValueDialog.vue'])
  add('chips', ui + 'vue/', ['MChip.vue', 'MChipSet.vue'])
  add('charts', ui, ['vue/MChart.vue', 'vue/MChartX.vue', 'vue/MRing.vue', 'vue/MProgressRing.vue', 'charts/chartMath.js'])
  add('host-live', web, ['composables/useHostLive.js', 'utils/homeData.js'])

  add('settings-fields', web + 'components/cfg/', [
    'CfgTop.vue', 'CfgSub.vue', 'CfgRow.vue', 'CfgExpert.vue', 'CfgSwitch.vue', 'CfgChoice.vue', 'CfgField.vue', 'cfgValue.js',
  ])
  add('settings-fields', web, ['utils/browsePath.js', 'composables/useSegFit.js'])
  add('settings-fields', ui, ['vue/MExpertItem.vue'])

  const vueRuntime = /\/node_modules\/(?:vue|vue-i18n|@vue\/(?:shared|reactivity|runtime-core|runtime-dom)|@intlify\/(?:shared|core-base|message-compiler))\//
  return [{
    name(id, context) {
      // An explicit import() of a library pane takes precedence over its group.
      if (id.startsWith(ui + 'vue/') && id.endsWith('.vue') && context.getModuleInfo(id)?.dynamicImporters.length) {
        return basename(id, '.vue')
      }
      if (vueRuntime.test(id)) return 'vue-runtime'
      return names.get(id) ?? null
    },
    includeDependenciesRecursively: false,
  }]
}

// Keep the pinned component library's exports on the routes that consume them.
// Its public Vue entry also registers custom elements and tooltips; that entry keeps its side effects.
export function foundationChunks(uiDir, assetsDir) {
  const ui = normalizePath(resolve(uiDir, 'src')) + '/'
  const registry = ui + 'locale/texts/index.js'
  const packPrefix = 'virtual:foundation-locale/'
  const runtime = normalizePath(resolve(assetsDir, 'config/localePacks.js'))
  return {
    name: 'foundation-route-chunks',
    apply: 'build',
    enforce: 'pre',
    transform: {
      filter: { id: /\.vue$/ },
      handler(source, id) {
        if (!normalizePath(id).startsWith(ui)) return
        const { descriptor } = parse(source, { filename: id })
        // A normal <script> can perform work at module load. Setup-only components
        // execute on mount. Keep any explicit side-effect imports eager as well.
        if (descriptor.script || !descriptor.scriptSetup || /^\s*import\s*['"]/m.test(descriptor.scriptSetup.content)) return
        return { code: source, moduleSideEffects: false }
      },
    },
    resolveId(id) {
      if (id.startsWith(packPrefix)) return '\0' + id
    },
    async load(id) {
      const pack = id.startsWith('\0' + packPrefix)
      if (!pack && normalizePath(id) !== registry) return
      const { TEXT_CODES, TEXT_MODULES } = await import(pathToFileURL(registry).href)
      if (pack) {
        const code = id.slice(packPrefix.length + 1)
        if (!TEXT_CODES.includes(code)) this.error('Unknown foundation-ui locale ' + code)
        return TEXT_MODULES.map((name) =>
          `export { default as ${name} } from ${JSON.stringify(ui + 'locale/texts/' + code + '/' + name + '.js')}`
        ).join('\n')
      }
      const loaders = TEXT_CODES.map((code) => `${JSON.stringify(code)}: () => import(${JSON.stringify(packPrefix + code)})`)
      return `
        export * from './resolve.js'
        import { createLocalePacks } from ${JSON.stringify(runtime)}
        export const TEXT_CODES = ${JSON.stringify(TEXT_CODES)}
        export const TEXT_MODULES = ${JSON.stringify(TEXT_MODULES)}
        export const { PARTIALS, loadLocale, partialFor, textTable } = createLocalePacks({${loaders.join(',\n')}})
      `
    },
  }
}
