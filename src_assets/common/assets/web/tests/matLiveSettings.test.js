import test from 'node:test'
import assert from 'node:assert/strict'
import { useConfig } from '../composables/useConfig.js'
import { useMatLiveStatus } from '../composables/useMatLiveStatus.js'
import { validateMatLiveSettings } from '../utils/matLiveSettings.js'

test('partial MAT settings block save and restart from any tab', async (t) => {
  const fetch = t.mock.method(globalThis, 'fetch', () => { throw new Error('Must not save') })
  const state = useConfig()
  state.currentTab.value = 'network'
  for (const config of [{ mat_live_pipe: 'pipe' }, { mat_live_producer: 'producer.exe' }]) {
    state.config.value = config
    assert.equal(await state.save(), false)
    assert.equal(state.validationError.value, 'config.mat_live_pair_required')
    await state.apply()
    assert.equal(state.restarted.value, false)
  }
  assert.equal(fetch.mock.callCount(), 0)
  assert.equal(validateMatLiveSettings({}), '')
  assert.equal(validateMatLiveSettings({ mat_live_pipe: '', mat_live_producer: '' }), '')
  assert.equal(validateMatLiveSettings({ mat_live_pipe: 'pipe', mat_live_producer: 'producer.exe' }), '')
})

test('paired MAT settings round-trip without changing Windows paths; clearing both removes defaults', async (t) => {
  const previousWindow = globalThis.window
  globalThis.window = { dispatchEvent() {} }
  t.after(() => { globalThis.window = previousWindow })
  const posted = []
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(url, '/api/config')
    posted.push(JSON.parse(options.body))
    return new Response(JSON.stringify({ status: true }), { status: 200 })
  })
  const state = useConfig()
  state.initTabs()
  const settings = { mat_live_pipe: String.raw`\\.\pipe\SunshineMatLive-test`, mat_live_producer: String.raw`C:\Program Files\MAT\producer.exe` }
  state.config.value = { ...settings }
  assert.equal(await state.save(), true)
  for (const [key, value] of Object.entries(settings)) assert.equal(posted[0][key], value)
  state.config.value = { mat_live_pipe: '', mat_live_producer: '' }
  assert.equal(await state.save(), true)
  assert.equal('mat_live_pipe' in posted[1], false)
  assert.equal('mat_live_producer' in posted[1], false)
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
