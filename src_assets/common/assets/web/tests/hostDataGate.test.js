import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { hostData, HOST_TOPICS, keepKey } from '../composables/hostData.js'
import { resetBootstrapDataCache } from '../config/bootstrapData.js'

// Gate: the first render of a page waits on no request. Host data comes through composables/hostData.js only (the one module the dedicated host channel will replace),
// and a topic has a kept answer that is read synchronously.
const root = new URL('../', import.meta.url).pathname
const walk = (dir, out = []) => { for (const e of fs.readdirSync(dir, { withFileTypes: true })) { if (['node_modules', 'tests', 'public'].includes(e.name)) continue; const f = path.join(dir, e.name); if (e.isDirectory()) walk(f, out); else if (/\.(vue|js)$/.test(e.name)) out.push(f) } return out }
const memoryStorage = () => { const m = new Map(); return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)) } }

test('no page or composable reads the host\'s configuration except through hostData', () => {
  const offenders = walk(root).filter((f) => !/composables\/hostData\.js$|config\/bootstrapData\.js$/.test(f) && /getBootstrapConfig\s*\(/.test(fs.readFileSync(f, 'utf8'))).map((f) => path.relative(root, f))
  assert.deepEqual(offenders, [], 'read host data with hostData.peek/refresh/subscribe')
})

test('peek is synchronous and subscribe gives the kept answer before the fresh one', async (t) => {
  const storage = memoryStorage()
  Object.defineProperty(globalThis, 'localStorage', { value: storage, configurable: true, writable: true }); t.after(() => { delete globalThis.localStorage })
  assert.deepEqual(HOST_TOPICS, ['config', 'apps'])
  assert.equal(hostData.peek('config'), null)
  storage.setItem(keepKey('config'), JSON.stringify({ sunshine_name: 'Kept' }))
  assert.equal(hostData.peek('config').sunshine_name, 'Kept')
  resetBootstrapDataCache()
  t.mock.method(globalThis, 'fetch', async () => ({ ok: true, status: 200, headers: new Headers({ 'content-type': 'application/json' }), json: async () => ({ sunshine_name: 'Fresh' }), text: async () => '{"sunshine_name":"Fresh"}' }))
  const seen = []
  await new Promise((resolve) => { hostData.subscribe('config', (value, { stale }) => { seen.push(`${value.sunshine_name}:${stale}`); if (!stale) resolve() }) })
  assert.deepEqual(seen, ['Kept:true', 'Fresh:false'])
})

test('the applications page draws from the kept list at once; the fresh list replaces it', () => {
  const page = fs.readFileSync(new URL('../views/Apps.vue', import.meta.url), 'utf8')
  assert.match(page, /const keptApps = hostData\.peek\('apps'\)\nconst hasKept = ref\(!!\(keptApps && Array\.isArray\(keptApps\.apps\)\)\)\nconst ready = computed\(\(\) => isLoaded\.value \|\| hasKept\.value\)/)
  assert.match(page, /<MSkeleton v-if="!ready" :rows="4" \/>/)
  assert.match(page, /if \(hasKept\.value\) \{ apps\.value = keptApps\.apps; originalApps\.value = deepClone\(keptApps\.apps\) \}\n  await Promise\.all\(\[loadApps\(\), loadPlatform\(\)\]\)/)
  const service = fs.readFileSync(new URL('../services/appService.js', import.meta.url), 'utf8')
  assert.match(service, /hostData\.refresh\('apps'\)/)
})
