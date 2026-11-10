import test from 'node:test'
import assert from 'node:assert/strict'

import { normalizeResolution, RESOLUTION_INPUT_PATTERN } from '../utils/resolutionInput.js'

const inputPattern = new RegExp(`^(?:${RESOLUTION_INPUT_PATTERN})$`, 'v')

test('resolution input accepts multiplication signs and stores the backend x format', () => {
  for (const separator of ['x', 'X', '*', '×']) {
    for (const value of [`1920${separator}1080`, ` 1920 ${separator} 1080 `]) {
      assert.equal(inputPattern.test(value), true, value)
      assert.equal(normalizeResolution(value), '1920x1080', value)
    }
  }
  assert.equal(normalizeResolution('2560×1440'), '2560x1440')
})

test('resolution input still rejects malformed dimensions and multiple separators', () => {
  for (const value of ['', '1920', 'x1080', '1920x', '1920**1080', '1920x1080x60',
    '1920×1080junk', '-1920x1080', '1920.5x1080', '19 20x1080', '1920/1080']) {
    assert.equal(inputPattern.test(value), false, value)
    assert.equal(normalizeResolution(value), '', value)
  }
  assert.equal(normalizeResolution(null), '')
})
