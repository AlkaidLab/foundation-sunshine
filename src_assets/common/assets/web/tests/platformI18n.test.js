import test from 'node:test'
import assert from 'node:assert/strict'
import { createSSRApp, h } from 'vue'
import { renderToString } from 'vue/server-renderer'
import { $tp, setPlatformHint, setPlatformI18n, usePlatformI18n } from '../platform-i18n.js'

const catalog = (messages) => ({ t: (key) => (key in messages ? messages[key] : key) })
const reset = () => { setPlatformI18n(null); setPlatformHint('') }

test('$tp outside of a component never throws and answers the default, then the key', () => {
  reset()
  assert.equal($tp('config.host_audio', 'Host audio'), 'Host audio')
  assert.equal($tp('config.host_audio'), 'config.host_audio')
  assert.equal(typeof usePlatformI18n().getMessageUsingPlatform, 'function')
})

test('$tp reads the registered catalog and the platform hint when nothing is injected (an async callback)', () => {
  reset()
  setPlatformI18n(catalog({ 'k_windows': 'win', 'k_unix': 'unix', k: 'plain' }))
  setPlatformHint('windows')
  assert.equal($tp('k', 'dflt'), 'win')
  setPlatformHint('linux')
  assert.equal($tp('k', 'dflt'), 'unix')
  setPlatformHint('')
  assert.equal($tp('k', 'dflt'), 'unix', 'an unknown platform tries the unix variant')
  setPlatformHint('windows')
  assert.equal($tp('missing', 'dflt'), 'dflt', 'windows does not fall back to unix')
  reset()
})

test('a catalog that throws or returns a non-string gives the default', () => {
  reset()
  setPlatformI18n({ t: () => { throw new Error('bad catalog') } })
  assert.equal($tp('k', 'dflt'), 'dflt')
  setPlatformI18n({ t: () => 42 })
  assert.equal($tp('k', 'dflt'), 'dflt')
  reset()
})

test('rendering a component that asks $tp with no provider renders (the home page with a live session)', async () => {
  reset()
  const Home = { render: () => h('span', $tp('config.host_audio', 'Host audio')) }
  assert.equal(await renderToString(createSSRApp(Home)), '<span>Host audio</span>')
  setPlatformI18n(catalog({ 'config.host_audio_unix': 'Spiele Audio' }))
  setPlatformHint('linux')
  assert.equal(await renderToString(createSSRApp(Home)), '<span>Spiele Audio</span>')
  reset()
})

test('the injected platform of the settings page still wins over the hint', async () => {
  reset()
  setPlatformHint('linux')
  const cat = catalog({ 'k_windows': 'win', 'k_unix': 'unix' })
  const app = createSSRApp({ render: () => h('span', $tp('k', 'dflt')) })
  app.provide('i18n', cat)
  app.provide('platform', { value: 'windows' })
  assert.equal(await renderToString(app), '<span>win</span>')
  reset()
})

test('no source reads inject(name).value without a default: a page that does not provide it would crash', async () => {
  const { readdirSync, readFileSync, statSync } = await import('node:fs')
  const { join } = await import('node:path')
  const root = new URL('..', import.meta.url).pathname
  const hits = []
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      if (name === 'tests' || name === 'node_modules') continue
      const path = join(dir, name)
      if (statSync(path).isDirectory()) walk(path)
      else if (/\.(vue|js)$/.test(name) && /inject\('[a-zA-Z]+'\)\.value/.test(readFileSync(path, 'utf8'))) hits.push(path)
    }
  }
  walk(root)
  assert.deepEqual(hits, [])
})
