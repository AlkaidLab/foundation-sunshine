import test from 'node:test'
import assert from 'node:assert/strict'

import {
  __scanOverrideTestUtils,
  applyScanOverrides,
  getScanOverrideKey,
  learnScanOverride,
} from '../utils/scanOverrides.js'

test('scan overrides learn and reapply confirmed metadata', () => {
  __scanOverrideTestUtils.clear()

  const scanned = {
    name: 'game.exe',
    cmd: 'C:\\Games\\Game\\game.exe',
    source_path: 'C:\\Games\\Game',
    'working-dir': 'C:\\Games\\Game',
    'image-path': '',
    'is-game': true,
  }
  const finalApp = {
    name: 'Correct Game',
    cmd: 'C:\\Games\\Game\\game.exe',
    'working-dir': 'C:\\Games\\Game',
    'image-path': 'cover.jpg',
  }

  assert.equal(learnScanOverride(scanned, finalApp), true)

  const [overridden] = applyScanOverrides([{ ...scanned, name: 'game.exe' }])
  assert.equal(overridden.name, 'Correct Game')
  assert.equal(overridden['image-path'], 'cover.jpg')
  assert.equal(overridden['canonical-name'], 'Correct Game')
  assert.equal(overridden['user-override'], true)
  assert.equal(overridden['ai-confidence'], 1)
})

test('scan override keys normalize path case and separators', () => {
  assert.equal(
    getScanOverrideKey({ source_path: 'C:\\Games\\Game' }),
    getScanOverrideKey({ source_path: 'c:/games/game' })
  )
})
