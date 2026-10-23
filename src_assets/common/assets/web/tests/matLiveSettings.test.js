import test from 'node:test'
import assert from 'node:assert/strict'
import { useConfig } from '../composables/useConfig.js'
import { useMatLiveStatus } from '../composables/useMatLiveStatus.js'
import { resetBootstrapDataCache } from '../config/bootstrapData.js'

test('MAT persists one switch and normalizes saved checkbox values', async (t) => {
  const previousWindow = globalThis.window
  globalThis.window = { dispatchEvent() {} }
  t.after(() => { globalThis.window = previousWindow; resetBootstrapDataCache() })
  const posted = []
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(url, '/api/config')
    if (options.method === 'POST') {
      posted.push(JSON.parse(options.body))
      return new Response(JSON.stringify({ status: true }), { status: 200 })
    }
    return new Response(JSON.stringify({ platform: 'windows', mat_live: 'true' }), { status: 200 })
  })
  resetBootstrapDataCache()
  const state = useConfig()
  state.initTabs()
  await state.loadConfig()
  assert.equal(state.config.value.mat_live, 'enabled')
  assert.equal(await state.save(), true)
  assert.equal(posted[0].mat_live, 'enabled')
  assert.equal('mat_live_pipe' in posted[0], false)
  assert.equal('mat_live_producer' in posted[0], false)
  state.config.value.mat_live = 'disabled'
  assert.equal(await state.save(), true)
  assert.equal('mat_live' in posted[1], false)
})

test('automatic status polling stops when hidden or unmounted', async (t) => {
  const previousDocument = globalThis.document
  globalThis.document = { hidden: false }
  t.after(() => { globalThis.document = previousDocument })
  let tick, cleared
  t.mock.method(globalThis, 'setInterval', (callback, delay) => { assert.equal(delay, 3000); tick = callback; return 42 })
  t.mock.method(globalThis, 'clearInterval', (timer) => { cleared = timer })
  const fetch = t.mock.method(globalThis, 'fetch', async () => new Response(JSON.stringify({ state: 'unobserved' }), { status: 200 }))
  const state = useMatLiveStatus()
  state.start()
  state.start()
  await new Promise((resolve) => setImmediate(resolve))
  assert.equal(fetch.mock.callCount(), 1)
  globalThis.document.hidden = true
  tick()
  assert.equal(fetch.mock.callCount(), 1)
  globalThis.document.hidden = false
  tick()
  await new Promise((resolve) => setImmediate(resolve))
  assert.equal(fetch.mock.callCount(), 2)
  state.dispose()
  assert.equal(cleared, 42)
  tick()
  assert.equal(fetch.mock.callCount(), 2)
})

test('status uses GET only, prevents overlap and clears stale readiness on errors', async (t) => {
  let finish
  const fetch = t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(url, '/api/mat/status')
    assert.equal(options.method, undefined)
    assert.equal(options.body, undefined)
    assert.equal(options.cache, 'no-store')
    return await new Promise((resolve) => { finish = resolve })
  })
  const state = useMatLiveStatus()
  const pending = state.refresh()
  await state.refresh()
  assert.equal(fetch.mock.callCount(), 1)
  finish(new Response(JSON.stringify({ state: 'streaming', last_error: '' }), { status: 200 }))
  await pending
  assert.equal(state.status.value.state, 'streaming')
  const failed = state.refresh()
  finish(new Response('{}', { status: 503 }))
  await failed
  assert.equal(state.status.value.state, 'unavailable')
  const stale = state.refresh()
  state.dispose()
  finish(new Response(JSON.stringify({ state: 'streaming' }), { status: 200 }))
  await stale
  assert.equal(state.status.value.state, 'unavailable')
  await state.refresh()
  assert.equal(fetch.mock.callCount(), 3)
})

test('a status request timeout clears the previous connected state', async (t) => {
  const controller = new AbortController()
  t.mock.method(AbortSignal, 'timeout', (delay) => { assert.equal(delay, 2500); return controller.signal })
  t.mock.method(globalThis, 'fetch', async (_url, options) => await new Promise((resolve, reject) => {
    options.signal.addEventListener('abort', () => reject(new Error('Timed out')), { once: true })
  }))
  const state = useMatLiveStatus()
  state.status.value = { state: 'ready' }
  const pending = state.refresh()
  controller.abort()
  await pending
  assert.equal(state.status.value.state, 'unavailable')
  assert.equal(state.loading.value, false)
})
