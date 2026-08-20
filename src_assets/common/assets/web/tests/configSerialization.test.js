import test from 'node:test'
import assert from 'node:assert/strict'

import {
  filterValidFps,
  normalizeEnabledDisabledValue,
  parseResolutions,
  serializeFps,
  serializeResolutions,
} from '../composables/useConfig.js'

test('resolution config round-trips Sunshine bracket syntax', () => {
  const parsed = parseResolutions('[1280x720,1920x1080]')

  assert.deepEqual(parsed, ['1280x720', '1920x1080'])
  assert.equal(serializeResolutions(parsed), '[1280x720,1920x1080]')
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
