import test from 'node:test'
import assert from 'node:assert/strict'

import {
  filterValidFps,
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
