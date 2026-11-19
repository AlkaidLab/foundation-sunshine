import test from 'node:test'
import assert from 'node:assert/strict'
import { resetBootstrapDataCache } from '../config/bootstrapData.js'
import { CONFIG_CACHE_KEY, readCachedConfig, useConfig } from '../composables/useConfig.js'

// The first render of a settings page waits on no request: the last answer of the host is kept and drawn at once, the fresh one replaces it in place.
const memoryStorage = () => { const m = new Map(); return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)), removeItem: (k) => m.delete(k) } }
const answer = (extra = {}) => ({ platform: 'windows', sunshine_name: 'Zako', min_log_level: 2, ...extra })
const jsonResponse = (body) => ({ ok: true, status: 200, headers: new Headers({ 'content-type': 'application/json' }), json: async () => body, text: async () => JSON.stringify(body) })

test('with a kept answer the config is there before the host has answered, then the fresh answer replaces it', async (t) => {
  const storage = memoryStorage()
  Object.defineProperty(globalThis, 'localStorage', { value: storage, configurable: true, writable: true }); t.after(() => { delete globalThis.localStorage })
  storage.setItem(CONFIG_CACHE_KEY, JSON.stringify(answer({ sunshine_name: 'Kept' })))
  resetBootstrapDataCache()
  let release
  t.mock.method(globalThis, 'fetch', () => new Promise((resolve) => { release = () => resolve(jsonResponse(answer({ sunshine_name: 'Fresh' }))) }))
  const state = useConfig()
  const loading = state.loadConfig()
  await Promise.resolve()
  assert.equal(state.config.value?.sunshine_name, 'Kept', 'drawn from the kept answer while the request is still out')
  assert.equal(state.platform.value, 'windows')
  release()
  await loading
  assert.equal(state.config.value.sunshine_name, 'Fresh', 'replaced in place')
  assert.equal(readCachedConfig().sunshine_name, 'Fresh', 'and kept for the next start')
})

test('an edit made on the kept values is not taken away by the fresh answer', async (t) => {
  const storage = memoryStorage()
  Object.defineProperty(globalThis, 'localStorage', { value: storage, configurable: true, writable: true }); t.after(() => { delete globalThis.localStorage })
  storage.setItem(CONFIG_CACHE_KEY, JSON.stringify(answer()))
  resetBootstrapDataCache()
  let release
  t.mock.method(globalThis, 'fetch', () => new Promise((resolve) => { release = () => resolve(jsonResponse(answer({ sunshine_name: 'Fresh' }))) }))
  const state = useConfig()
  const loading = state.loadConfig()
  await Promise.resolve()
  state.config.value.sunshine_name = 'Typed'
  release()
  await loading
  assert.equal(state.config.value.sunshine_name, 'Typed')
})

test('without a kept answer nothing is invented, and a host that fails leaves the kept values on screen', async (t) => {
  const storage = memoryStorage()
  Object.defineProperty(globalThis, 'localStorage', { value: storage, configurable: true, writable: true }); t.after(() => { delete globalThis.localStorage })
  resetBootstrapDataCache()
  t.mock.method(globalThis, 'fetch', () => Promise.reject(new Error('down')))
  t.mock.method(console, 'error', () => {})
  const none = useConfig(); await none.loadConfig()
  assert.equal(none.config.value, null ?? none.config.value)
  storage.setItem(CONFIG_CACHE_KEY, JSON.stringify(answer({ sunshine_name: 'Kept' })))
  resetBootstrapDataCache()
  const kept = useConfig(); await kept.loadConfig()
  assert.equal(kept.config.value.sunshine_name, 'Kept', 'a failing host does not blank the page')
})
