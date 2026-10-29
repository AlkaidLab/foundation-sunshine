import test from 'node:test'
import assert from 'node:assert/strict'

import {
  filterValidFps,
  normalizeEnabledDisabledValue,
  parseResolutions,
  parseHevcGopSize,
  serializeFps,
  serializeResolutions,
  useConfig,
} from '../composables/useConfig.js'

test('resolution config round-trips Sunshine bracket syntax', () => {
  const parsed = parseResolutions('[1280x720,1920x1080]')

  assert.deepEqual(parsed, ['1280x720', '1920x1080'])
  assert.equal(serializeResolutions(parsed), '[1280x720,1920x1080]')
})

test('Windows sends custom EDID sizes and the default explicitly so the driver XML resets', async (t) => {
  const originalWindow = globalThis.window
  globalThis.window = { dispatchEvent() {} }
  t.after(() => { globalThis.window = originalWindow })
  const requests = []
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    if (url === '/api/config') requests.push(JSON.parse(options.body))
    return new Response(JSON.stringify({ status: true }), {
      status: 200, headers: { 'Content-Type': 'application/json' },
    })
  })
  const state = useConfig()
  state.initTabs()
  state.platform.value = 'windows'
  state.resolutions.value = ['1920x1080', '3440x1440', '3840x2160']
  state.fps.value = ['60']
  for (const value of ['3840x2160', '3440x1440', 'default']) {
    state.config.value = { vdd_edid_native_resolution: value }
    assert.equal(await state.save(), true)
    assert.equal(requests.at(-1).vdd_edid_native_resolution, value)
  }
})

test('removed or unencodable EDID selections prevent saving without making an API request', async (t) => {
  const fetch = t.mock.method(globalThis, 'fetch', () => { throw new Error('Invalid EDID selection must not reach the API') })
  const state = useConfig()
  state.platform.value = 'windows'
  state.resolutions.value = ['1920x1080', '3840x2160', '5120x1440']
  state.fps.value = ['120']
  for (const value of ['3440x1440', '5120x1440', '3840x2160']) {
    state.config.value = { vdd_edid_native_resolution: value }
    assert.equal(await state.save(), false)
    assert.equal(state.validationError.value, 'config.vdd_edid_native_resolution_invalid')
  }
  assert.equal(fetch.mock.callCount(), 0)
})

test('HEVC GOP preserves driver default and explicit on-demand zero', () => {
  for (const value of [-1, '-1', 0, '0', 60, '60', 1000, '1000']) {
    assert.equal(parseHevcGopSize(value), Number(value))
  }
  for (const value of [undefined, null, '', ' ', true, -2, 1001, 1.5, '1.5', '60frames', NaN, Infinity]) {
    assert.equal(parseHevcGopSize(value), undefined)
  }
})

test('invalid GOP reports an error across tabs and prevents save/apply requests', async (t) => {
  const fetch = t.mock.method(globalThis, 'fetch', () => {
    throw new Error('Invalid configuration must not reach the API')
  })
  const state = useConfig()
  state.currentTab.value = 'network'
  for (const value of ['', 1001, 1.5]) {
    state.config.value = { amd_hevc_gop_size: value }
    assert.equal(await state.save(), false)
    assert.equal(state.validationError.value, 'config.amd_hevc_gop_size_desc')
    await state.apply()
    assert.equal(state.restarted.value, false)
    assert.equal(state.saved.value, false)
  }
  assert.equal(fetch.mock.callCount(), 0)
})

test('parseResolutions falls back to an empty array for invalid input', () => {
  assert.deepEqual(parseResolutions('not valid json'), [])
  assert.deepEqual(parseResolutions(''), [])
})

test('fps config removes invalid values before serialization', () => {
  const valid = filterValidFps(['24', '30', 60, '500', 501])

  assert.deepEqual(valid, ['30', 60, '500'])
  assert.equal(serializeFps(valid), '[30,60,500]')
})

test('boolean-like config values normalize to enabled and disabled select values', () => {
  assert.equal(normalizeEnabledDisabledValue('true'), 'enabled')
  assert.equal(normalizeEnabledDisabledValue('YES'), 'enabled')
  assert.equal(normalizeEnabledDisabledValue('1'), 'enabled')
  assert.equal(normalizeEnabledDisabledValue(true), 'enabled')
  assert.equal(normalizeEnabledDisabledValue('false'), 'disabled')
  assert.equal(normalizeEnabledDisabledValue('off'), 'disabled')
  assert.equal(normalizeEnabledDisabledValue('0'), 'disabled')
  assert.equal(normalizeEnabledDisabledValue(false), 'disabled')
  assert.equal(normalizeEnabledDisabledValue(''), '')
  assert.equal(normalizeEnabledDisabledValue(undefined), undefined)
})
