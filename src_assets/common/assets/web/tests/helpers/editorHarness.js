import { createRequire } from 'node:module'
import { fileURLToPath } from 'node:url'
import './fakeDom.js'
import { doc, findAll, isDisabled, click, textOf } from './fakeDom.js'
import { createServer } from 'vite'
import vue from '@vitejs/plugin-vue'
import { foundationUiAliases } from '../../../../../../scripts/vite/foundationUi.js'

// Mounts a real page of the Web UI (a .vue file with its real components and foundation-ui) with Vue's DOM runtime on the small DOM of fakeDom.js, so a test can open the page, look at what
// it drew, press its buttons and read the state of 保存. The Vue plugin compiles templates for the server when a module is loaded the way vite loads one in node; this page is mounted with
// the DOM runtime, so the plugin is told it is not a server build.
const forceClient = (plugin) => {
  if (Array.isArray(plugin)) return plugin.map(forceClient)
  const wrap = (hook) => {
    if (typeof hook === 'function') return function (code, id, opt) { return hook.call(this, code, id, { ...opt, ssr: false }) }
    if (hook && hook.handler) return { ...hook, handler(code, id, opt) { return hook.handler.call(this, code, id, { ...opt, ssr: false }) } }
    return hook
  }
  return { ...plugin, transform: wrap(plugin.transform), load: wrap(plugin.load) }
}

const foundationUi = process.env.FOUNDATION_UI_DIR || fileURLToPath(new URL('../../../../../../../ds5-ui-m3e', import.meta.url))
const webRoot = fileURLToPath(new URL('../..', import.meta.url))
const require = createRequire(import.meta.url)

export async function startHarness() {
  const server = await createServer({
    configFile: false, logLevel: 'silent', appType: 'custom', root: webRoot, server: { middlewareMode: true, hmr: false, watch: null }, optimizeDeps: { noDiscovery: true, include: [] },
    resolve: { dedupe: ['vue'], alias: foundationUiAliases(foundationUi) },
    ssr: { noExternal: ['foundation-ui', /^@material\//] },
    plugins: [forceClient(vue({ template: { compilerOptions: { isCustomElement: (tag) => tag.startsWith('md-') } } }))],
  })
  const { createApp, nextTick } = require('vue')
  const { createI18n } = require('vue-i18n')
  const warn = console.warn
  console.warn = () => {} // Vue's development warnings about the fake DOM (teleport targets, custom elements) are not what a test is about

  const settle = async (rounds = 12) => { for (let i = 0; i < rounds; i++) { await new Promise((r) => setTimeout(r, 15)); await nextTick() } }

  /** Mounts `Page` with `props`; resolves a handle on what it drew. `events` maps an event name to a function that receives its arguments; `messages` are the zh texts (the page shows the keys without them). */
  async function mount(Page, props, events = {}, { messages = {} } = {}) {
    const root = doc.createElement('div')
    const listeners = Object.fromEntries(Object.entries(events).map(([name, fn]) => [`on${name[0].toUpperCase()}${name.slice(1)}`, fn]))
    const app = createApp(Page, { ...props, ...listeners })
    app.use(createI18n({ legacy: false, locale: 'zh', messages: { zh: messages, en: {} } }))
    app.mount(root)
    await settle()
    const buttons = (label) => findAll(root, (n) => n.tagName === 'button' && textOf(n).includes(label))
    return {
      root,
      settle,
      text: () => textOf(root),
      buttons,
      /** the button whose label is `label` (the last one: 保存 is at the end of the page) */
      button: (label) => { const all = buttons(label); if (!all.length) throw new Error(`no button "${label}"`); return all[all.length - 1] },
      saveDisabled: (label = '保存') => isDisabled(root && buttons(label).at(-1)),
      radios: (name) => findAll(root, (n) => n.attrs.role === 'radio' && n.attrs['aria-label'] === name),
      selected: (node) => node.attrs['aria-checked'] === 'true',
      switches: (name) => findAll(root, (n) => n.attrs.role === 'switch' && (n.attrs['aria-label'] === name)),
      press: async (node) => { click(node); await settle() },
      unmount: () => app.unmount(),
    }
  }

  return {
    server,
    load: (path) => server.ssrLoadModule(path),
    mount,
    settle,
    async close() { console.warn = warn; await server.close() },
  }
}
