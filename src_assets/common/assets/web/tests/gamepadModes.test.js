import test from 'node:test'
import assert from 'node:assert/strict'

import {
  PER_APP_GAMEPAD_MODES,
  normalizePerAppGamepadMode,
} from '../utils/gamepadModes.js'

test('per-app gamepad choices include DualSense', () => {
  assert.deepEqual(
    PER_APP_GAMEPAD_MODES.map(({ value }) => value),
    ['', 'auto', 'x360', 'ds4', 'ds5'],
  )
})

test('per-app gamepad normalization accepts known modes only', () => {
  for (const { value } of PER_APP_GAMEPAD_MODES) {
    assert.equal(normalizePerAppGamepadMode(value), value)
  }

  assert.equal(normalizePerAppGamepadMode('invalid'), '')
  assert.equal(normalizePerAppGamepadMode(undefined), '')
})
