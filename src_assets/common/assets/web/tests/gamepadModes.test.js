import test from 'node:test'
import assert from 'node:assert/strict'

import {
  PER_APP_GAMEPAD_MODES,
} from '../utils/gamepadModes.js'

test('per-app gamepad choices include DualSense', () => {
  assert.deepEqual(
    PER_APP_GAMEPAD_MODES.map(({ value }) => value),
    ['', 'auto', 'x360', 'ds4', 'ds5'],
  )
})
